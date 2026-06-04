/*
 * Copyright (C) 2026 Jolla Mobile Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "droidmediaphotoplugin.h"

// This needs to be first because of broken includes in Android < 10
#include <camera/NdkCaptureRequest.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraError.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <gui/BufferQueue.h>
#include <gui/Surface.h>
#include <hardware/camera2.h>
#include <media/NdkImage.h>
#include <media/hardware/HardwareAPI.h>
#include <media/openmax/OMX_IVCommon.h>

#if ANDROID_MAJOR <= 9
#include <android/native_window.h>
typedef ANativeWindow ACameraWindowType;
#endif

#include <photo-backend.h>

#undef LOG_TAG
#define LOG_TAG "DroidMediaPhotoPlugin"

namespace {
constexpr int kMaxAcquiredBuffers = android::BufferQueue::NUM_BUFFER_SLOTS / 2;
}

struct DroidMediaPhotoBuffer : public PhotoBackendBuffer {
    int slot;
    int frame_number;
    android::sp<android::GraphicBuffer> graphic_buffer;
    android::VideoNativeMetadata metadata;
};

struct DroidMediaPhotoStream;

namespace {

class DroidMediaPhotoStreamListener
    : public android::BufferQueue::ProxyConsumerListener
{
public:
    DroidMediaPhotoStreamListener(DroidMediaPhotoStream *stream)
        : android::BufferQueue::ProxyConsumerListener(nullptr)
        , m_stream(stream)
    {
    }

    void onFrameAvailable(const android::BufferItem&);
    void onBuffersReleased();
    void onSidebandStreamChanged() {}

    DroidMediaPhotoStream *m_stream;
};

} /* namespace */

struct DroidMediaPhotoBufferSlot {
    DroidMediaPhotoBuffer *buffer = NULL;
};

struct DroidMediaPhotoStream : public PhotoBackendStream {
    ACameraOutputTarget *output_target = NULL;
    ACaptureSessionOutput *output = NULL;
    android::sp<android::IGraphicBufferProducer> producer;
    android::sp<android::IGraphicBufferConsumer> consumer;
    android::sp<DroidMediaPhotoStreamListener> listener;
    android::sp<ANativeWindow> window;
    android::Mutex slot_lock;
    DroidMediaPhotoBufferSlot slots[android::BufferQueue::NUM_BUFFER_SLOTS];

    int sequence_id = -1;
};

struct DroidMediaPhotoCamera : public PhotoBackendCamera {
    const DroidMediaPhotoInterface *plugin_interface = NULL;
    char *id = NULL;
    ACameraManager *manager = NULL;
    ACameraMetadata *metadata = NULL;
    ACameraDevice *device = NULL;
    ACameraCaptureSession *session = NULL;
    ACaptureRequest *preview_request = NULL;
    ACaptureRequest *still_capture_request = NULL;
    ACaptureRequest *recording_request = NULL;
    ACaptureRequest *video_snapshot_request = NULL;
    ACaptureRequest *repeating_request = NULL;

    android::Mutex droid_sequence_lock;
    int picture_seq_id = -1;
    bool ae_precapture = false;
    bool ae_precapture_locked = false;
    bool ae_unlock = false;
    bool ae_unlock_wait_lock = false;
    bool af_active_scan = false;
    std::deque<DroidMediaPhotoStream *> stopping_streams;

    // Callbacks
    ACameraDevice_StateCallbacks device_state_callbacks;
    ACameraCaptureSession_stateCallbacks capture_session_state_callbacks;
    ACameraCaptureSession_captureCallbacks capture_callbacks;
};

static size_t get_jpeg_size(DroidMediaPhotoBuffer *buffer)
{
    if (buffer->graphic_buffer->height != 1) {
        ALOGE("JPEG blob must have height == 1");
        return 0;
    }

    uint32_t width = buffer->graphic_buffer->width;

    const uint8_t *data = reinterpret_cast<const uint8_t *>(buffer->photo_buffer->planes[0].data);
    const uint8_t *header = data + (width - sizeof(struct camera2_jpeg_blob));
    const struct camera2_jpeg_blob *blob =
        reinterpret_cast<const struct camera2_jpeg_blob *>(header);

    if (blob->jpeg_blob_id != CAMERA2_JPEG_BLOB_ID) {
        ALOGE("JPEG blob does not have a valid header");
        return 0;
    }

    // Sanity check
    if (blob->jpeg_size > (width - sizeof(struct camera2_jpeg_blob))) {
        ALOGE("JPEG blob size is invalid: %u", blob->jpeg_size);
        return 0;
    }

    return blob->jpeg_size;
}

static bool dmp_buffer_map(PhotoBuffer *pbuffer)
{
    DroidMediaPhotoBuffer *buffer = static_cast<DroidMediaPhotoBuffer *>(pbuffer->backend);
    android::status_t status;

    uint64_t usage = android::GraphicBuffer::USAGE_SW_READ_RARELY;

    if (pbuffer->info->num_planes == 1) {
        void *data = NULL;

        status = buffer->graphic_buffer->lock(usage, &data);
        if (status != android::NO_ERROR) {
            ALOGE("Failed to lock GraphicBuffer %p for reading",
                  buffer->graphic_buffer.get());
            return false;
        }

        pbuffer->planes[0].data = data;

        if (buffer->graphic_buffer->format == HAL_PIXEL_FORMAT_BLOB) {
            pbuffer->planes[0].length = get_jpeg_size(buffer);
        } else {
            pbuffer->planes[0].length = pbuffer->info->planes[0].max_length;
        }
    } else if (pbuffer->info->num_planes == 2) {
        android_ycbcr ycbcr;

        status = buffer->graphic_buffer->lockYCbCr(usage, &ycbcr);
        if (status != android::NO_ERROR) {
            ALOGE("Failed to lock GraphicBuffer %p for reading",
                  buffer->graphic_buffer.get());
            return false;
        }

        pbuffer->planes[0].data = ycbcr.y;
        pbuffer->planes[1].data = ycbcr.cr;

        pbuffer->planes[0].length = pbuffer->info->planes[0].max_length;
        pbuffer->planes[1].length = pbuffer->info->planes[1].max_length;
    } else if (pbuffer->info->num_planes == 3) {
        android_ycbcr ycbcr;

        status = buffer->graphic_buffer->lockYCbCr(android::GraphicBuffer::USAGE_SW_READ_RARELY, &ycbcr);
        if (status != android::NO_ERROR) {
            ALOGE("Failed to lock GraphicBuffer %p for reading",
                  buffer->graphic_buffer.get());
            return false;
        }

        pbuffer->planes[0].data = ycbcr.y;
        pbuffer->planes[1].data = ycbcr.cb;
        pbuffer->planes[2].data = ycbcr.cr;

        pbuffer->planes[0].length = pbuffer->info->planes[0].max_length;
        pbuffer->planes[1].length = pbuffer->info->planes[1].max_length;
        pbuffer->planes[2].length = pbuffer->info->planes[2].max_length;
    } else {
        ALOGE("cannot map an opaque buffer");
        return false;
    }

    return true;
}

static void dmp_buffer_release(PhotoBuffer *pbuffer)
{
    DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pbuffer->stream->backend);
    DroidMediaPhotoBuffer *buffer = static_cast<DroidMediaPhotoBuffer *>(pbuffer->backend);

    android::status_t status;

    ALOGV("release GraphicBuffer %p", buffer->graphic_buffer.get());

    status = buffer->graphic_buffer->unlock();
    if (status != android::NO_ERROR) {
        ALOGE("failed to unlock buffer: %d", status);
    }

    android::AutoMutex lock(stream->slot_lock);

    if (stream->slots[buffer->slot].buffer != buffer) {
        ALOGE("buffer mismatch in slot %d, buffer released too late?", buffer->slot);
        return;
    }

    status = stream->consumer->releaseBuffer(buffer->slot, buffer->frame_number,
                                             EGL_NO_DISPLAY, nullptr,
                                             android::Fence::NO_FENCE);
    if (status != android::NO_ERROR) {
        ALOGE("failed to release buffer in slot %d: %d", buffer->slot, status);
    }
}

static EGLImageKHR dmp_buffer_create_egl_image(PhotoBuffer *pbuffer,
                                               EGLDisplay dpy, EGLContext ctx)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pbuffer->stream->camera->backend);
    DroidMediaPhotoBuffer *buffer = static_cast<DroidMediaPhotoBuffer *>(pbuffer->backend);

    EGLint attrs[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_NONE,
    };

    PFNEGLCREATEIMAGEKHRPROC func = camera->plugin_interface->egl_create_image;

    return func(dpy, ctx, EGL_NATIVE_BUFFER_ANDROID,
                buffer->graphic_buffer->getNativeBuffer(), attrs);
}

static void dmp_buffer_destroy(PhotoBackendBuffer *backend_buffer)
{
    DroidMediaPhotoBuffer *buffer = static_cast<DroidMediaPhotoBuffer *>(backend_buffer);

    delete buffer;
}

static const PhotoBufferImpl dmp_buffer_impl = {
    .map = dmp_buffer_map,
    .release = dmp_buffer_release,
    .create_egl_image = dmp_buffer_create_egl_image,
    .destroy = dmp_buffer_destroy,
};

void DroidMediaPhotoStreamListener::onFrameAvailable(const android::BufferItem&)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(m_stream->photo_stream->camera->backend);
    camera->plugin_interface->frame_available(m_stream);
}

static void teardown_buffers(DroidMediaPhotoStream *stream)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(stream->photo_stream->camera->backend);

    for (int i = 0; i < android::BufferQueue::NUM_BUFFER_SLOTS; i++) {
        DroidMediaPhotoBuffer *buffer;
        {
            android::AutoMutex lock(stream->slot_lock);

            buffer = stream->slots[i].buffer;
            stream->slots[i].buffer = nullptr;
        }

        if (buffer) {
            camera->plugin_interface->unbind_buffer(buffer);
        }
    }
}

void DroidMediaPhotoStreamListener::onBuffersReleased()
{
    ALOGD("buffers released");

    teardown_buffers(m_stream);
}

static bool setup_stream(DroidMediaPhotoCamera *camera, PhotoStream *pstream,
                         ACaptureSessionOutputContainer *outputs)
{
    DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pstream->backend);
    camera_status_t status;
    uint64_t usage = android::GraphicBuffer::USAGE_SW_READ_OFTEN;
    android::PixelFormat format;

    switch (pstream->config->format) {
    case PHOTO_IMAGE_FORMAT_UNKNOWN:
        if (pstream->usage == PHOTO_STREAM_USAGE_PREVIEW) {
            usage = android::GraphicBuffer::USAGE_HW_TEXTURE;
        } else if (pstream->usage == PHOTO_STREAM_USAGE_RECORDING) {
            usage = android::GraphicBuffer::USAGE_HW_VIDEO_ENCODER;
        }
        format = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
        break;
    case PHOTO_IMAGE_FORMAT_YUV420:
    case PHOTO_IMAGE_FORMAT_NV21:
        format = HAL_PIXEL_FORMAT_YCBCR_420_888;
        break;
    case PHOTO_IMAGE_FORMAT_JPEG:
        format = HAL_PIXEL_FORMAT_BLOB;
        break;
    default:
        ALOGE("unexpected PhotoImageFormat 0x%x", pstream->config->format);
        return false;
    }

    android::BufferQueue::createBufferQueue(&stream->producer, &stream->consumer);

    stream->consumer->setMaxAcquiredBufferCount(kMaxAcquiredBuffers);
    stream->consumer->setConsumerName(android::String8("PhotoStream"));
    stream->consumer->setConsumerUsageBits(usage);
    stream->consumer->setDefaultBufferFormat(format);
    stream->consumer->setDefaultBufferSize(pstream->config->width,
                                           pstream->config->height);

    stream->listener = new DroidMediaPhotoStreamListener(stream);

    // controlledByApp needs to be true for queue to drop buffers
    if (stream->consumer->consumerConnect(stream->listener, true) != android::NO_ERROR) {
        ALOGE("Failed to set buffer consumer");
        return false;
    }

    stream->window = new android::Surface(stream->producer, true);

    status = ACameraOutputTarget_create(stream->window.get(), &stream->output_target);
    if (status != ACAMERA_OK) {
        return false;
    }

    status = ACaptureSessionOutput_create(stream->window.get(), &stream->output);
    if (status != ACAMERA_OK) {
        return false;
    }

    status = ACaptureSessionOutputContainer_add(outputs, stream->output);
    if (status != ACAMERA_OK) {
        return false;
    }

    return true;
}

static void cleanup_stream(DroidMediaPhotoCamera *camera, PhotoStream *pstream)
{
    DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pstream->backend);

    if (stream->output) {
        ACaptureSessionOutput_free(stream->output);
        stream->output = NULL;
    }

    if (stream->output_target) {
        ACameraOutputTarget_free(stream->output_target);
        stream->output_target = NULL;
    }

    stream->window.clear();

    stream->consumer->consumerDisconnect();
    stream->listener.clear();

    teardown_buffers(stream);

    stream->consumer.clear();
    stream->producer.clear();
}

static bool update_repeating_request(DroidMediaPhotoCamera *camera)
{
    PhotoCamera *pcamera = camera->photo_camera;
    bool recording = false, preview = false;
    int min_fps = 0, max_fps = INT_MAX;

    ALOGD("update_repeating_request");

    for (int i = 0; i < pcamera->num_streams; i++) {
        PhotoStream *pstream = pcamera->streams[i];

        ALOGD("stream %d (%s) usage %d size %dx%d format 0x%x fps %d..%d",
              i, pstream->running ? "running" : "stopped", pstream->usage,
              pstream->config->width, pstream->config->height,
              pstream->config->format, pstream->config->min_fps,
              pstream->config->max_fps);

        if (!pstream->running) {
            continue;
        }

        if (pstream->usage == PHOTO_STREAM_USAGE_RECORDING) {
            recording = true;
        } else {
            preview = true;
        }

        // The slowest running stream determines the frame rate
        if (pstream->config->max_fps < max_fps) {
            min_fps = pstream->config->min_fps;
            max_fps = pstream->config->max_fps;
        }
    }

    if (recording) {
        camera->repeating_request = camera->recording_request;
    } else if (preview) {
        camera->repeating_request = camera->preview_request;
    } else {
        camera->repeating_request = NULL;

        camera->plugin_interface->begin_state_change(camera);

        bool changed = false;

        camera->ae_precapture = false;
        camera->af_active_scan = false;

        if (pcamera->control_state->ae_state != PHOTO_CONTROL_STATE_DISABLED) {
            pcamera->control_state->ae_state = PHOTO_CONTROL_STATE_DISABLED;
            pcamera->control_state->locks_changed |= PHOTO_CONTROL_LOCK_AE;
            changed = true;
        }
        if (pcamera->control_state->af_state != PHOTO_CONTROL_STATE_DISABLED) {
            pcamera->control_state->af_state = PHOTO_CONTROL_STATE_DISABLED;
            pcamera->control_state->locks_changed |= PHOTO_CONTROL_LOCK_AF;
            changed = true;
        }

        camera->plugin_interface->end_state_change(camera, changed);

        ACameraCaptureSession_stopRepeating(camera->session);
        return true;
    }

    int32_t fps_range[2] = { min_fps, max_fps };
    ACaptureRequest_setEntry_i32(camera->repeating_request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE,
                                 2, fps_range);

    android::AutoMutex lock(camera->droid_sequence_lock);

    int seq_id = -1;
    camera_status_t status = ACameraCaptureSession_setRepeatingRequest(camera->session,
            &camera->capture_callbacks, 1, &camera->repeating_request, &seq_id);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to set repeating request");
        return false;
    }

    for (int i = 0; i < pcamera->num_streams; i++) {
        PhotoStream *pstream = pcamera->streams[i];
        DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pstream->backend);

        if (pstream->running) {
            stream->sequence_id = seq_id;
        }
    }

    return true;
}

static bool convert_format(int32_t format, PhotoImageFormat *out)
{
    switch (format) {
    case AIMAGE_FORMAT_PRIVATE:
        *out = PHOTO_IMAGE_FORMAT_UNKNOWN;
        return true;
    case AIMAGE_FORMAT_YUV_420_888:
        // TODO we need a way to tell which YUV format the camera supports.
        // The YUV_420_888 format is Android's "flexible YCbCr" format, which
        // means it could be I420, YV12, NV21, NV12 or even something else.
        // It is not possible to determine the format without mapping the
        // buffer, but some programs like GStreamer need to know it in advance.
        // Assume NV21 here because it seems quite common. This will break on
        // some devices.
        *out = PHOTO_IMAGE_FORMAT_NV21;
        return true;
    case AIMAGE_FORMAT_JPEG:
        *out = PHOTO_IMAGE_FORMAT_JPEG;
        return true;
    default:
        break;
    }

    return false;
}

static void enumerate_fps_ranges(DroidMediaPhotoStream *stream, const PhotoConfig *filter,
                                 PhotoConfigIteratorCallback cb, void *userdata,
                                 PhotoStreamConfigFlags flags, PhotoAvailableConfig *info)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(stream->photo_stream->camera->backend);

    if (filter && ((filter->width >= 0 && filter->width != info->width.min) ||
                    (filter->height >= 0 && filter->height != info->height.min) ||
                    (filter->format != PHOTO_IMAGE_FORMAT_UNKNOWN &&
                        filter->format != info->format))) {
        return;
    }

    // TODO checking SCALER_AVAILABLE_MIN_FRAME_DURATIONS may be useful on some devices

    ACameraMetadata_const_entry entry;
    camera_status_t status;

    status = ACameraMetadata_getConstEntry(camera->metadata,
            ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &entry);
    if (status == ACAMERA_OK) {
        for (int i = 0; i < entry.count; i += 2) {
            info->min_fps.min = entry.data.i32[i + 0];
            info->min_fps.max = entry.data.i32[i + 0];
            info->max_fps.min = entry.data.i32[i + 1];
            info->max_fps.max = entry.data.i32[i + 1];

            if (filter && ((filter->min_fps >= 0 && filter->min_fps != info->min_fps.min) ||
                           (filter->max_fps >= 0 && filter->max_fps != info->max_fps.min))) {
                continue;
            }

            cb(userdata, info);
        }
    }
}

static void dmp_stream_enumerate_configs(PhotoStream *pstream, const PhotoConfig *filter,
                                         PhotoConfigIteratorCallback cb, void *userdata,
                                         PhotoStreamConfigFlags flags, PhotoAvailableConfig *info)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pstream->camera->backend);
    DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pstream->backend);

    ACameraMetadata_const_entry entry;
    camera_status_t status;

    int32_t recommended_format = -1;

    info->native_format = "com.android.ANativeWindowBuffer";

    if (flags & PHOTO_STREAM_CONFIG_RECOMMENDED) {
        int stream_usecase = ACAMERA_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS_PREVIEW;
        switch (pstream->usage) {
        case PHOTO_STREAM_USAGE_STILL_CAPTURE:
            stream_usecase = ACAMERA_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS_SNAPSHOT;
            break;
        case PHOTO_STREAM_USAGE_RECORDING:
            stream_usecase = ACAMERA_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS_RECORD;
            break;
        default:
            break;
        }

        status = ACameraMetadata_getConstEntry(camera->metadata,
                ACAMERA_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS, &entry);
        if (status == ACAMERA_OK) {
            for (int i = 0; i < entry.count; i += 5) {
                if (entry.data.i32[i + 3]) {
                    // ignore input formats
                    continue;
                }

                int usecases = entry.data.i32[i + 4];

                if (usecases & (1 << stream_usecase)) {
                    if (!convert_format(entry.data.i32[i + 2], &info->format)) {
                        continue;
                    }

                    info->width.min = entry.data.i32[i + 0];
                    info->width.max = entry.data.i32[i + 0];
                    info->height.min = entry.data.i32[i + 1];
                    info->height.max = entry.data.i32[i + 1];

                    enumerate_fps_ranges(stream, filter, cb, userdata, flags, info);
                }
            }

            // Recommended formats and sizes enumerated, done
            return;
        } else {
            ALOGI("Camera does not define any recommended configurations");

            // Use hardcoded formats as fallback and return all supported sizes

            switch (pstream->usage) {
            case PHOTO_STREAM_USAGE_PREVIEW:
            case PHOTO_STREAM_USAGE_RECORDING:
                recommended_format = AIMAGE_FORMAT_PRIVATE;
                break;
            case PHOTO_STREAM_USAGE_STILL_CAPTURE:
                recommended_format = AIMAGE_FORMAT_JPEG;
                break;
            default:
                recommended_format = AIMAGE_FORMAT_YUV_420_888;
                break;
            }
        }
    }

    status = ACameraMetadata_getConstEntry(camera->metadata,
            ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry);
    if (status == ACAMERA_OK) {
        for (int i = 0; i < entry.count; i += 4) {
            if (entry.data.i32[i + 3]) {
                // ignore input formats
                continue;
            }

            int32_t format = entry.data.i32[i + 0];

            bool format_valid = true;

            if (recommended_format >= 0) {
                format_valid = (format == recommended_format);
            } else if (pstream->usage == PHOTO_STREAM_USAGE_STILL_CAPTURE) {
                format_valid = (format == AIMAGE_FORMAT_JPEG);
            } else {
                format_valid &= (format != AIMAGE_FORMAT_JPEG);

                if (pstream->usage != PHOTO_STREAM_USAGE_PREVIEW &&
                        pstream->usage != PHOTO_STREAM_USAGE_RECORDING) {
                    format_valid &= (format != AIMAGE_FORMAT_PRIVATE);
                }
            }

            if (!format_valid || !convert_format(format, &info->format)) {
                continue;
            }

            info->width.min = entry.data.i32[i + 1];
            info->width.max = entry.data.i32[i + 1];
            info->height.min = entry.data.i32[i + 2];
            info->height.max = entry.data.i32[i + 2];

            enumerate_fps_ranges(stream, filter, cb, userdata, flags, info);
        }
    } else {
        ALOGE("Failed to get any supported configurations");
    }
}

static bool dmp_stream_start(PhotoStream *pstream)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pstream->camera->backend);
    DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pstream->backend);
    camera_status_t status;

    if (!stream->output_target) {
        ALOGE("Cannot start an inactive stream");
        return false;
    }

    if (pstream->usage == PHOTO_STREAM_USAGE_STILL_CAPTURE) {
        ALOGE("Cannot start a still capture stream");
        return false;
    }

    if (pstream->usage == PHOTO_STREAM_USAGE_PREVIEW) {
        status = ACaptureRequest_addTarget(camera->preview_request, stream->output_target);
        if (status != ACAMERA_OK) {
            return false;
        }

        status = ACaptureRequest_addTarget(camera->still_capture_request, stream->output_target);
        if (status != ACAMERA_OK) {
            return false;
        }
    }

    status = ACaptureRequest_addTarget(camera->recording_request, stream->output_target);
    if (status != ACAMERA_OK) {
        return false;
    }

    status = ACaptureRequest_addTarget(camera->video_snapshot_request, stream->output_target);
    if (status != ACAMERA_OK) {
        return false;
    }

    return update_repeating_request(camera);
}

static void dmp_stream_stop(PhotoStream *pstream)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pstream->camera->backend);
    DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pstream->backend);

    if (!stream->output_target) {
        ALOGE("Cannot stop an inactive stream");
        return;
    }

    if (pstream->usage == PHOTO_STREAM_USAGE_STILL_CAPTURE) {
        ALOGE("Cannot stop a still capture stream");
        return;
    }

    ACaptureRequest_removeTarget(camera->preview_request, stream->output_target);
    ACaptureRequest_removeTarget(camera->still_capture_request, stream->output_target);
    ACaptureRequest_removeTarget(camera->recording_request, stream->output_target);
    ACaptureRequest_removeTarget(camera->video_snapshot_request, stream->output_target);

    update_repeating_request(camera);

    android::AutoMutex lock(camera->droid_sequence_lock);
    ALOGD("stream will stop with sequence ID %d", stream->sequence_id);
    camera->stopping_streams.push_back(stream);
}

static bool dmp_stream_take_picture(PhotoStream *pstream)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pstream->camera->backend);
    DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pstream->backend);
    camera_status_t status;

    if (!stream->output_target) {
        ALOGE("Cannot take picture on an inactive stream");
        return false;
    }

    android::AutoMutex lock(camera->droid_sequence_lock);

    if (camera->picture_seq_id != -1) {
        ALOGE("Camera is already taking a picture");
        return false;
    }

    if (pstream->usage != PHOTO_STREAM_USAGE_STILL_CAPTURE) {
        ALOGE("take_picture requires a still capture stream");
        return false;
    }

    ACaptureRequest *request = camera->still_capture_request;
    if (camera->repeating_request == camera->recording_request) {
        ALOGD("take_picture: video snapshot");
        request = camera->video_snapshot_request;
    } else {
        ALOGD("take_picture: still capture");
    }

    status = ACaptureRequest_addTarget(request, stream->output_target);
    if (status != ACAMERA_OK) {
        return false;
    }

    status = ACameraCaptureSession_capture(camera->session,
            &camera->capture_callbacks, 1, &request, &camera->picture_seq_id);
    if (status != ACAMERA_OK) {
        ALOGE("Submitting a capture request failed");
    }

    ACaptureRequest_removeTarget(request, stream->output_target);

    return status == ACAMERA_OK;
}

static void dmp_stream_destroy(PhotoBackendStream *backend_stream)
{
    DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(backend_stream);

    if (stream->output_target) {
        ALOGE("Stream being destroyed is still active!");
        return;
    }

    delete stream;
}

static DroidMediaPhotoBuffer *new_buffer_from_item(DroidMediaPhotoStream *stream,
                                                   const android::BufferItem &item)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(stream->photo_stream->camera->backend);
    DroidMediaPhotoBuffer *buffer = new DroidMediaPhotoBuffer;

    android::status_t status;

    buffer->slot = item.mSlot;
    buffer->frame_number = item.mFrameNumber;
    buffer->graphic_buffer = item.mGraphicBuffer;

    android::PixelFormat format = buffer->graphic_buffer->format;

    unsigned int num_planes = 0;
    PhotoBufferPlaneInfo *planes = nullptr;

    if (format == HAL_PIXEL_FORMAT_BLOB) {
        num_planes = 1;
        planes = new PhotoBufferPlaneInfo[1];
        planes[0].stride = buffer->graphic_buffer->stride;
        // for JPEG buffers, width is the maximum buffer size
        planes[0].max_length = buffer->graphic_buffer->width;
    } else if (format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
        android_ycbcr ycbcr;

        // lock the buffer temporarily to get stride information
        uint64_t usage = android::GraphicBuffer::USAGE_SW_READ_RARELY;
        status = buffer->graphic_buffer->lockYCbCr(usage, &ycbcr);
        if (status != android::NO_ERROR) {
            ALOGE("Failed to lock GraphicBuffer %p to obtain stride",
                  buffer->graphic_buffer.get());
            goto fail;
        }

        buffer->graphic_buffer->unlock();

        // See convert_format(), this may fail on some devices.
        int vu_step = reinterpret_cast<uint8_t *>(ycbcr.cb) -
                        reinterpret_cast<uint8_t *>(ycbcr.cr);
        if (ycbcr.chroma_step != 2 || vu_step != 1) {
            ALOGE("Format is not NV21, please fix YCbCr handling! chroma_step %zu vu_step %d",
                  ycbcr.chroma_step, vu_step);
            goto fail;
        }

        num_planes = 2;
        planes = new PhotoBufferPlaneInfo[2];

        planes[0].stride = ycbcr.ystride;
        planes[1].stride = ycbcr.cstride;

        uint32_t height = buffer->graphic_buffer->height;
        planes[0].max_length = height * ycbcr.ystride;
        planes[1].max_length = (height / 2) * ycbcr.cstride;
#if 0
    } else if (format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
        android_ycbcr ycbcr;

        // lock the buffer temporarily to get stride information
        status = buffer->graphic_buffer->lockYCbCr(0, &ycbcr);
        if (status != android::NO_ERROR) {
            ALOGE("Failed to lock GraphicBuffer %p to obtain stride",
                  buffer->graphic_buffer.get());
            goto fail;
        }

        buffer->graphic_buffer->unlock();

        num_planes = 3;
        planes = new PhotoBufferPlaneInfo[3];

        planes[0].stride = ycbcr.ystride;
        planes[1].stride = ycbcr.cstride;
        planes[2].stride = ycbcr.cstride;

        uint32_t height = buffer->graphic_buffer->height;
        planes[0].max_length = height * ycbcr.ystride;
        planes[1].max_length = (height / 2) * ycbcr.cstride;
        planes[2].max_length = (height / 2) * ycbcr.cstride;
#endif
    }

    if (!camera->plugin_interface->init_buffer(stream, buffer, num_planes, planes,
                                               &dmp_buffer_impl)) {
        return nullptr;
    }

    buffer->metadata.eType = android::kMetadataBufferTypeANWBuffer;
    buffer->metadata.pBuffer = buffer->graphic_buffer->getNativeBuffer();
    buffer->metadata.nFenceFd = -1;

    buffer->photo_buffer->info->native_metadata = &buffer->metadata;
    buffer->photo_buffer->info->native_metadata_size = sizeof(buffer->metadata);

    camera->plugin_interface->bind_buffer(buffer);

    return buffer;

fail:
    stream->consumer->releaseBuffer(buffer->slot, buffer->frame_number,
                                    EGL_NO_DISPLAY, nullptr,
                                    android::Fence::NO_FENCE);
    delete buffer;
    return nullptr;
}

static PhotoBuffer *dmp_stream_get_next_buffer(PhotoStream *pstream)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pstream->camera->backend);
    DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pstream->backend);

    if (!stream->consumer.get()) {
        ALOGE("Cannot get buffer from inactive stream");
        return NULL;
    }

    android::BufferItem item;

    android::status_t status = stream->consumer->acquireBuffer(&item, 0);
    if (status != android::NO_ERROR) {
        if (status != android::BufferQueue::NO_BUFFER_AVAILABLE) {
            ALOGE("Failed to acquire a buffer: %d", status);
        }
        return nullptr;
    }

    DroidMediaPhotoBuffer *buffer = nullptr;

    if (item.mGraphicBuffer.get()) {
        ALOGD("new GraphicBuffer %p in slot %d", item.mGraphicBuffer.get(), item.mSlot);

        buffer = new_buffer_from_item(stream, item);

        DroidMediaPhotoBuffer *replaced_buffer;
        {
            android::AutoMutex lock(stream->slot_lock);

            replaced_buffer = stream->slots[item.mSlot].buffer;
            stream->slots[item.mSlot].buffer = buffer;
        }

        if (replaced_buffer) {
            camera->plugin_interface->unbind_buffer(replaced_buffer);
        }
    } else {
        android::AutoMutex lock(stream->slot_lock);
        if (stream->slots[item.mSlot].buffer) {
            ALOGV("existing buffer in slot %d", item.mSlot);
            buffer = stream->slots[item.mSlot].buffer;

            buffer->frame_number = item.mFrameNumber;
        } else {
            ALOGW("slot %d buffer is null", item.mSlot);
        }
    }

    return buffer ? buffer->photo_buffer : nullptr;
}

static const PhotoStreamImpl dmp_stream_impl = {
    .enumerate_configs = dmp_stream_enumerate_configs,
    .start = dmp_stream_start,
    .stop = dmp_stream_stop,
    .take_picture = dmp_stream_take_picture,
    .get_next_buffer = dmp_stream_get_next_buffer,
    .destroy = dmp_stream_destroy,
};

static void device_on_disconnected(void *context, ACameraDevice *device)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(context);

    ALOGI("camera '%s' disconnected", camera->photo_camera->info->id);

    camera->plugin_interface->notify_disconnected(camera);
}

static void device_on_error(void *context, ACameraDevice *device, int error)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(context);

    ALOGE("camera '%s' error: %d", camera->photo_camera->info->id, error);
}

static void capture_session_on_active(void *context, ACameraCaptureSession *session)
{
    ALOGD("camera session active");
}

static void capture_session_on_closed(void *context, ACameraCaptureSession *session)
{
    ALOGD("camera session closed");
}

static void capture_session_on_ready(void *context, ACameraCaptureSession *session)
{
    ALOGD("camera session ready");
}

static void capture_session_on_capture_started(
    void *context, ACameraCaptureSession *session,
    const ACaptureRequest *request, int64_t timestamp)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(context);

    ACameraMetadata_const_entry entry;
    camera_status_t status;

    status = ACaptureRequest_getConstEntry(request, ACAMERA_CONTROL_CAPTURE_INTENT, &entry);
    if (status == ACAMERA_OK && entry.data.u8[0] == ACAMERA_CONTROL_CAPTURE_INTENT_STILL_CAPTURE) {
        ALOGI("shutter!");
        camera->plugin_interface->notify_shutter(camera);
    }
}

static void capture_session_on_capture_progressed(
    void *context, ACameraCaptureSession *session,
    ACaptureRequest *request, const ACameraMetadata *result)
{
    ALOGV("capture progressed");
}

static bool process_af_result(DroidMediaPhotoCamera *camera, const ACameraMetadata *result)
{
    PhotoCamera *pcamera = camera->photo_camera;
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    PhotoControlLockState lock_state = pcamera->control_state->af_state;

    if (lock_state == PHOTO_CONTROL_STATE_PENDING) {
        status = ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AF_TRIGGER, &entry);
        if (status == ACAMERA_OK && entry.count == 1 &&
                entry.data.u8[0] == ACAMERA_CONTROL_AF_TRIGGER_START) {
            ALOGD("got first result for AF precapture");
            camera->af_active_scan = true;
        } else {
            ALOGD("waiting for AF precapture start");
            return false;
        }
    }

    if (lock_state == PHOTO_CONTROL_STATE_UNLOCKING) {
        status = ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AF_TRIGGER, &entry);
        if (status == ACAMERA_OK && entry.count == 1 &&
                entry.data.u8[0] == ACAMERA_CONTROL_AF_TRIGGER_CANCEL) {
            ALOGD("AF precapture cancel complete");
        } else {
            ALOGD("waiting for AF precapture cancel");
            return false;
        }
    }

    status = ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AF_STATE, &entry);
    if (status == ACAMERA_OK && entry.count == 1) {
        uint8_t af_state = entry.data.u8[0];
        ALOGV("af_lock_state %d, new AF state %d", lock_state, af_state);

        switch (af_state) {
        case ACAMERA_CONTROL_AF_STATE_INACTIVE:
            lock_state = PHOTO_CONTROL_STATE_DISABLED;
            break;
        case ACAMERA_CONTROL_AF_STATE_PASSIVE_FOCUSED:
        case ACAMERA_CONTROL_AF_STATE_PASSIVE_UNFOCUSED:
            lock_state = PHOTO_CONTROL_STATE_UNLOCKED;
            break;
        case ACAMERA_CONTROL_AF_STATE_FOCUSED_LOCKED:
        case ACAMERA_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED:
            camera->af_active_scan = false;
            lock_state = PHOTO_CONTROL_STATE_LOCKED;
            break;
        default:
            lock_state = PHOTO_CONTROL_STATE_SEARCHING;
            break;
        }

        if (lock_state != pcamera->control_state->af_state) {
            ALOGV("-> af_lock_state %d", lock_state);
            pcamera->control_state->af_state = lock_state;
            pcamera->control_state->locks_changed |= PHOTO_CONTROL_LOCK_AF;
            return true;
        }
    }

    return false;
}

static bool process_ae_result(DroidMediaPhotoCamera *camera, const ACameraMetadata *result)
{
    PhotoCamera *pcamera = camera->photo_camera;
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    PhotoControlLockState lock_state = pcamera->control_state->ae_state;

    if (lock_state == PHOTO_CONTROL_STATE_PENDING && !camera->ae_precapture) {
        status = ACameraMetadata_getConstEntry(result,
                ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, &entry);
        if (status == ACAMERA_OK && entry.count == 1 &&
                entry.data.u8[0] == ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START) {
            ALOGD("got first result for AE precapture");
            camera->ae_precapture = true;
            camera->ae_precapture_locked = false;
        } else {
            ALOGD("waiting for AE precapture start");
            return false;
        }
    }

    if (lock_state == PHOTO_CONTROL_STATE_UNLOCKING && !camera->ae_unlock) {
        status = ACameraMetadata_getConstEntry(result,
                ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, &entry);
        if (status == ACAMERA_OK && entry.count == 1 &&
                entry.data.u8[0] == ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL) {
            ALOGD("AE precapture cancel complete");
        } else {
            ALOGD("waiting for AE precapture cancel");
            return false;
        }
    }

    if (camera->ae_unlock_wait_lock) {
        status = ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AE_LOCK, &entry);
        if (status == ACAMERA_OK && entry.count == 1 &&
                entry.data.u8[0] == ACAMERA_CONTROL_AE_LOCK_ON) {
            ALOGD("found AE lock");
            camera->ae_unlock_wait_lock = false;
        } else {
            ALOGD("waiting for AE lock before unlock");
            return false;
        }
    }

    status = ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AE_STATE, &entry);
    if (status == ACAMERA_OK && entry.count == 1) {
        uint8_t ae_state = entry.data.u8[0];
        ALOGV("ae_lock_state %d, new AE state %d", lock_state, ae_state);

        if (camera->ae_unlock) {
            if (ae_state == ACAMERA_CONTROL_AE_STATE_LOCKED) {
                ALOGD("waiting for AE unlock");
                return false;
            } else {
                ALOGD("AE unlock complete");
                camera->ae_unlock = false;
            }
        }

        if (lock_state == PHOTO_CONTROL_STATE_FORCE_LOCKING) {
            if (ae_state == ACAMERA_CONTROL_AE_STATE_LOCKED) {
                ALOGD("AE lock complete");
                lock_state = PHOTO_CONTROL_STATE_FORCE_LOCKED;
            } else {
                ALOGD("waiting for AE lock");
                return false;
            }
        } else if (camera->ae_precapture) {
            switch (ae_state) {
            case ACAMERA_CONTROL_AE_STATE_INACTIVE:
                lock_state = PHOTO_CONTROL_STATE_DISABLED;
                if (camera->ae_precapture_locked) {
                    camera->ae_precapture = false;
                    ALOGD("AE lock lost, inactive");
                }
                break;
            case ACAMERA_CONTROL_AE_STATE_CONVERGED:
            case ACAMERA_CONTROL_AE_STATE_FLASH_REQUIRED:
            case ACAMERA_CONTROL_AE_STATE_LOCKED:
                camera->ae_precapture_locked = true;
                if (lock_state != PHOTO_CONTROL_STATE_FORCE_LOCKED) {
                    lock_state = PHOTO_CONTROL_STATE_LOCKED;
                }
                break;
            default:
                lock_state = PHOTO_CONTROL_STATE_SEARCHING;
                if (camera->ae_precapture_locked) {
                    ALOGD("AE lock lost, searching");
                    camera->ae_precapture = false;
                }
                break;
            }
        } else {
            switch (ae_state) {
            case ACAMERA_CONTROL_AE_STATE_INACTIVE:
                lock_state = PHOTO_CONTROL_STATE_DISABLED;
                break;
            case ACAMERA_CONTROL_AE_STATE_CONVERGED:
            case ACAMERA_CONTROL_AE_STATE_FLASH_REQUIRED:
                lock_state = PHOTO_CONTROL_STATE_UNLOCKED;
                break;
            case ACAMERA_CONTROL_AE_STATE_LOCKED:
                if (lock_state != PHOTO_CONTROL_STATE_FORCE_LOCKED) {
                    lock_state = PHOTO_CONTROL_STATE_LOCKED;
                }
                break;
            default:
                lock_state = PHOTO_CONTROL_STATE_SEARCHING;
                break;
            }
        }

        if (lock_state != pcamera->control_state->ae_state) {
            ALOGV("-> ae_lock_state %d", lock_state);
            pcamera->control_state->ae_state = lock_state;
            pcamera->control_state->locks_changed |= PHOTO_CONTROL_LOCK_AE;
            return true;
        }
    }

    return false;
}

static void capture_session_on_capture_completed(
    void *context, ACameraCaptureSession *session,
    ACaptureRequest *request, const ACameraMetadata *result)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(context);

    ALOGV("capture completed");

    camera->plugin_interface->begin_state_change(camera);
    bool changed = false;
    changed |= process_ae_result(camera, result);
    changed |= process_af_result(camera, result);
    camera->plugin_interface->end_state_change(camera, changed);
}

static void capture_session_on_capture_failed(
    void *context, ACameraCaptureSession *session,
    ACaptureRequest *request, ACameraCaptureFailure *failure)
{
    ALOGW("capture failed");
}

static void capture_session_on_capture_sequence_completed(
    void *context, ACameraCaptureSession *session,
    int sequenceId, int64_t frameNumber)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(context);

    ALOGD("capture sequence completed: %d", sequenceId);

    android::AutoMutex lock(camera->droid_sequence_lock);

    if (sequenceId == camera->picture_seq_id) {
        camera->picture_seq_id = -1;
        camera->plugin_interface->notify_capture(camera, true);
    }

    while (!camera->stopping_streams.empty() &&
            camera->stopping_streams.front()->sequence_id == sequenceId) {
        camera->plugin_interface->stream_stopped(camera->stopping_streams.front());
        camera->stopping_streams.pop_front();
    }
}

static void capture_session_on_capture_sequence_abort(
    void *context, ACameraCaptureSession *session, int sequenceId)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(context);

    ALOGW("capture sequence aborted: %d", sequenceId);

    android::AutoMutex lock(camera->droid_sequence_lock);

    if (sequenceId == camera->picture_seq_id) {
        camera->picture_seq_id = -1;
        camera->plugin_interface->notify_capture(camera, false);
    }

    while (!camera->stopping_streams.empty() &&
            camera->stopping_streams.front()->sequence_id == sequenceId) {
        camera->plugin_interface->stream_stopped(camera->stopping_streams.front());
        camera->stopping_streams.pop_front();
    }
}

static void capture_session_on_capture_buffer_lost(
    void *context, ACameraCaptureSession *session,
    ACaptureRequest *request, ACameraWindowType *window, int64_t frameNumber)
{
    ALOGW("capture buffer lost");
}

static bool dmp_camera_init(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    // Set callbacks
    camera->device_state_callbacks.context = camera;
    camera->device_state_callbacks.onDisconnected = device_on_disconnected;
    camera->device_state_callbacks.onError = device_on_error;

    camera->capture_session_state_callbacks.context = camera;
    camera->capture_session_state_callbacks.onReady = capture_session_on_ready;
    camera->capture_session_state_callbacks.onActive = capture_session_on_active;
    camera->capture_session_state_callbacks.onClosed = capture_session_on_closed;

    camera->capture_callbacks.context = camera;
    camera->capture_callbacks.onCaptureStarted = capture_session_on_capture_started;
    camera->capture_callbacks.onCaptureProgressed = capture_session_on_capture_progressed;
    camera->capture_callbacks.onCaptureCompleted = capture_session_on_capture_completed;
    camera->capture_callbacks.onCaptureFailed = capture_session_on_capture_failed;
    camera->capture_callbacks.onCaptureSequenceCompleted = capture_session_on_capture_sequence_completed;
    camera->capture_callbacks.onCaptureSequenceAborted = capture_session_on_capture_sequence_abort;
    camera->capture_callbacks.onCaptureBufferLost = capture_session_on_capture_buffer_lost;

    PhotoCameraInfo *info = camera->photo_camera->info;

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_LENS_FACING, &entry);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera lens facing: %d", status);
        return false;
    }

    if (entry.data.u8[0] == ACAMERA_LENS_FACING_FRONT) {
        info->facing = PHOTO_CAMERA_FACING_FRONT;
    } else {
        info->facing = PHOTO_CAMERA_FACING_BACK;
    }

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_SENSOR_ORIENTATION, &entry);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera sensor orientation: %d", status);
        return false;
    }

    info->orientation = entry.data.i32[0];

    status = ACameraMetadata_getConstEntry(camera->metadata,
            ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &entry);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera active pixel array size: %d", status);
        return false;
    }

    info->active_array_width = entry.data.i32[2];
    info->active_array_height = entry.data.i32[3];

    return true;
}

static void dmp_camera_destroy(PhotoBackendCamera *backend_camera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(backend_camera);

    if (camera->device) {
        ALOGE("Cannot destroy a camera that is still open!");
        return;
    }

    ACameraMetadata_free(camera->metadata);
    delete camera;
}

static bool dmp_camera_open(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    camera_status_t status;

    camera->manager = ACameraManager_create();
    if (!camera->manager) {
        return false;
    }

    status = ACameraManager_openCamera(camera->manager, pcamera->info->id,
                                       &camera->device_state_callbacks, &camera->device);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to open camera '%s': %d", pcamera->info->id, status);
        return false;
    }

    status = ACameraDevice_createCaptureRequest(camera->device, TEMPLATE_PREVIEW,
                                                &camera->preview_request);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to create request for preview");
        return false;
    }

    status = ACameraDevice_createCaptureRequest(camera->device, TEMPLATE_STILL_CAPTURE,
                                                &camera->still_capture_request);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to create request for still capture");
        return false;
    }

    status = ACameraDevice_createCaptureRequest(camera->device, TEMPLATE_RECORD,
                                                &camera->recording_request);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to create request for video recording");
        return false;
    }

    status = ACameraDevice_createCaptureRequest(camera->device, TEMPLATE_VIDEO_SNAPSHOT,
                                                &camera->video_snapshot_request);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to create request for video snapshot capture");
        return false;
    }

    return true;
}

static void dmp_camera_close(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);

    if (camera->session) {
        ALOGE("Failed to close camera, a capture session is still active!");
        return;
    }

    if (camera->device) {
        ACameraDevice_close(camera->device);
        camera->device = NULL;
    }

    if (camera->preview_request) {
        ACaptureRequest_free(camera->preview_request);
        camera->preview_request = NULL;
    }

    if (camera->still_capture_request) {
        ACaptureRequest_free(camera->still_capture_request);
        camera->still_capture_request = NULL;
    }

    if (camera->recording_request) {
        ACaptureRequest_free(camera->recording_request);
        camera->recording_request = NULL;
    }

    if (camera->video_snapshot_request) {
        ACaptureRequest_free(camera->video_snapshot_request);
        camera->video_snapshot_request = NULL;
    }

    if (camera->manager) {
        ACameraManager_delete(camera->manager);
        camera->manager = NULL;
    }
}

static void dmp_camera_query_ae_compensation(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    PhotoAvailableAECompensation *ae_compensation = pcamera->ae_compensation;
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_CONTROL_AE_COMPENSATION_STEP,
                                           &entry);
    if (status != ACAMERA_OK || entry.count != 1) {
        ALOGE("Failed to get AE compensation step: %d", status);
        return;
    }

    ae_compensation->step = (float)entry.data.r[0].numerator / (float)entry.data.r[0].denominator;

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_CONTROL_AE_COMPENSATION_RANGE,
                                           &entry);
    if (status != ACAMERA_OK || entry.count != 2) {
        ALOGE("Failed to get AE compensation range: %d", status);
        return;
    }

    ae_compensation->min = (float)entry.data.i32[0] * ae_compensation->step;
    ae_compensation->max = (float)entry.data.i32[1] * ae_compensation->step;
}

static void dmp_camera_query_flash_modes(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_FLASH_INFO_AVAILABLE, &entry);
    if (status == ACAMERA_OK && entry.count == 1 &&
            entry.data.u8[0] == ACAMERA_FLASH_INFO_AVAILABLE_TRUE) {
        pcamera->flash_modes->auto_flash = true;
        pcamera->flash_modes->auto_flash_redeye = true;
        pcamera->flash_modes->always_flash = true;
        pcamera->flash_modes->off = true;
        pcamera->flash_modes->torch = true;
    }
}

static void dmp_camera_query_focus_modes(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    PhotoAvailableFocusModes *modes = pcamera->focus_modes;
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_CONTROL_AF_AVAILABLE_MODES,
                                           &entry);
    if (status == ACAMERA_OK) {
        for (int i = 0; i < entry.count; i++) {
            switch (entry.data.u8[i]) {
            case ACAMERA_CONTROL_AF_MODE_AUTO:
            case ACAMERA_CONTROL_AF_MODE_MACRO:
                modes->auto_focus = true;
                break;
            case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
            case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
                modes->continuous_af = true;
                break;
            case ACAMERA_CONTROL_AF_MODE_OFF:
                modes->manual_focus = true;
                break;
            default:
                break;
            }
        }
    }
}

static void dmp_camera_query_lens_focus(PhotoCamera *pcamera)
{
    // TODO lens focus
}

static void dmp_camera_query_zoom(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    ACameraMetadata_const_entry entry;
    camera_status_t status;

#if ANDROID_MAJOR >= 11
    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_CONTROL_ZOOM_RATIO_RANGE,
                                           &entry);
    if (status == ACAMERA_OK && entry.count == 2) {
        pcamera->zoom->min = entry.data.f[0];
        pcamera->zoom->max = entry.data.f[1];
    }
#else
    status = ACameraMetadata_getConstEntry(camera->metadata,
                                           ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &entry);
    if (status == ACAMERA_OK && entry.count == 1) {
        pcamera->zoom->min = 1.0f;
        pcamera->zoom->max = entry.data.f[0];
    }
#endif
}

static int32_t *convert_metering_areas(PhotoControlMeteringAreas *areas, const int32_t *crop_region,
                                       uint32_t *count)
{
    if (areas->count == 0) {
        *count = 5;
        int32_t *values = new int32_t[5];
        memset(values, 0, sizeof(int32_t) * 5);
        return values;
    }

    int left = crop_region[0];
    int top = crop_region[1];
    int width = crop_region[2];
    int height = crop_region[3];

    *count = areas->count * 5;
    int32_t *values = new int32_t[*count];

    for (int i = 0; i < areas->count; i++) {
        values[i * 5 + 0] = left + (areas->rects[i].left * width);
        values[i * 5 + 1] = top + (areas->rects[i].top * height);
        values[i * 5 + 2] = left + (areas->rects[i].right * width);
        values[i * 5 + 3] = top + (areas->rects[i].bottom * height);
        values[i * 5 + 4] = areas->rects[i].weight;
    }

    return values;
}

static void update_params_for_request(PhotoCamera *pcamera, ACaptureRequest *request, bool is_video)
{
    PhotoControlUpdateFlags *update = pcamera->dirty;
    uint8_t value_u8;

    if (update->ae_compensation) {
        int32_t value_i32 = round(pcamera->ae_compensation_value /
                                  pcamera->ae_compensation->step);
        ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION,
                                     1, &value_i32);
    }

    if (update->flash_mode) {
        switch (pcamera->flash_mode) {
        case PHOTO_CONTROL_AUTO_FLASH:
            value_u8 = ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH;
            break;
        case PHOTO_CONTROL_AUTO_FLASH_REDEYE:
            value_u8 = ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE;
            break;
        case PHOTO_CONTROL_ALWAYS_FLASH:
            value_u8 = ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH;
            break;
        default:
            value_u8 = ACAMERA_CONTROL_AE_MODE_ON;
            break;
        }
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_MODE, 1, &value_u8);

        switch (pcamera->flash_mode) {
        case PHOTO_CONTROL_FLASH_TORCH:
            value_u8 = ACAMERA_FLASH_MODE_TORCH;
            break;
        default:
            value_u8 = ACAMERA_FLASH_MODE_OFF;
            break;
        }
        ACaptureRequest_setEntry_u8(request, ACAMERA_FLASH_MODE, 1, &value_u8);
    }

    if (update->focus_mode) {
        switch (pcamera->focus_mode) {
        case PHOTO_CONTROL_AUTO_FOCUS:
            value_u8 = ACAMERA_CONTROL_AF_MODE_AUTO;
            break;
        case PHOTO_CONTROL_CONTINUOUS_AUTO_FOCUS:
            if (is_video)
                value_u8 = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
            else
                value_u8 = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
            break;
        case PHOTO_CONTROL_MANUAL_FOCUS:
            value_u8 = ACAMERA_CONTROL_AF_MODE_OFF;
            break;
        default:
            ALOGW("Unknown AF mode, disabling AF");
            value_u8 = ACAMERA_CONTROL_AF_MODE_OFF;
            break;
        }
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_MODE, 1, &value_u8);
    }

    if (update->lens_focus) {
        // TODO lens focus
    }

    if (update->zoom) {
#if ANDROID_MAJOR >= 11
        ACaptureRequest_setEntry_float(request, ACAMERA_CONTROL_ZOOM_RATIO, 1,
                                       &pcamera->zoom_value);
#else
        // TODO crop
#endif
    }

    int32_t crop_region[4] = {
        0,
        0,
        pcamera->info->active_array_width,
        pcamera->info->active_array_height,
    };

    if (update->metering_areas) {
        uint32_t count;
        int32_t *values = convert_metering_areas(pcamera->ae_metering_areas, crop_region, &count);
        ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_REGIONS, count, values);
        delete[] values;

        values = convert_metering_areas(pcamera->af_metering_areas, crop_region, &count);
        ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AF_REGIONS, count, values);
        delete[] values;

        values = convert_metering_areas(pcamera->awb_metering_areas, crop_region, &count);
        ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AWB_REGIONS, count, values);
        delete[] values;
    }
}

static void dmp_camera_update_params(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);

    update_params_for_request(pcamera, camera->preview_request, false);
    update_params_for_request(pcamera, camera->still_capture_request, false);
    update_params_for_request(pcamera, camera->recording_request, true);
    update_params_for_request(pcamera, camera->video_snapshot_request, true);

    update_repeating_request(camera);
}

static void dmp_camera_trigger(PhotoCamera *pcamera, PhotoControlLockTypes types)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);

    if (!camera->repeating_request) {
        ALOGW("ignoring precapture trigger because no streams are running");
        return;
    }

    ACaptureRequest *request = ACaptureRequest_copy(camera->repeating_request);
    if (!request) {
        return;
    }

    camera->plugin_interface->begin_state_change(camera);

    bool changed = false;

    if (types & PHOTO_CONTROL_LOCK_AE) {
        PhotoControlLockState lock_state = pcamera->control_state->ae_state;
        if (lock_state != PHOTO_CONTROL_STATE_PENDING && !camera->ae_precapture &&
                lock_state != PHOTO_CONTROL_STATE_FORCE_LOCKED) {
            uint8_t value = ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START;
            ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &value);

            pcamera->control_state->ae_state = PHOTO_CONTROL_STATE_PENDING;
            pcamera->control_state->locks_changed |= PHOTO_CONTROL_LOCK_AE;
            changed = true;
        }
    }

    if (types & PHOTO_CONTROL_LOCK_AF) {
        ACameraMetadata_const_entry entry;
        camera_status_t status;

        status = ACaptureRequest_getConstEntry(request, ACAMERA_CONTROL_AF_MODE, &entry);
        if (status == ACAMERA_OK && entry.data.u8[0] != ACAMERA_CONTROL_AF_MODE_OFF) {
            PhotoControlLockState lock_state = pcamera->control_state->af_state;
            if (lock_state != PHOTO_CONTROL_STATE_PENDING && !camera->af_active_scan &&
                    lock_state != PHOTO_CONTROL_STATE_LOCKED) {
                uint8_t value = ACAMERA_CONTROL_AF_TRIGGER_START;
                ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_TRIGGER, 1, &value);

                pcamera->control_state->af_state = PHOTO_CONTROL_STATE_PENDING;
                pcamera->control_state->locks_changed |= PHOTO_CONTROL_LOCK_AF;
                changed = true;
            }
        }
    }

    camera->plugin_interface->end_state_change(camera, changed);

    ACameraCaptureSession_capture(camera->session, &camera->capture_callbacks, 1, &request, NULL);
}

static void set_ae_lock_for_request(PhotoCamera *pcamera, ACaptureRequest *request, bool value)
{
    uint8_t value_u8 = value;

    ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_LOCK, 1, &value_u8);
}

static void dmp_camera_force_lock(PhotoCamera *pcamera, PhotoControlLockTypes types)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);

    if (!camera->repeating_request) {
        return;
    }

    camera->plugin_interface->begin_state_change(camera);

    bool changed = false;

    if (types & PHOTO_CONTROL_LOCK_AE) {
        pcamera->control_state->ae_state = PHOTO_CONTROL_STATE_FORCE_LOCKING;
        pcamera->control_state->locks_changed |= PHOTO_CONTROL_LOCK_AE;
        changed = true;

        set_ae_lock_for_request(pcamera, camera->preview_request, true);
        set_ae_lock_for_request(pcamera, camera->still_capture_request, true);
        set_ae_lock_for_request(pcamera, camera->recording_request, true);
        set_ae_lock_for_request(pcamera, camera->video_snapshot_request, true);
    }

    camera->plugin_interface->end_state_change(camera, changed);
}

static void dmp_camera_unlock(PhotoCamera *pcamera, PhotoControlLockTypes types)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);

    if (!camera->repeating_request) {
        return;
    }

    ACaptureRequest *request = ACaptureRequest_copy(camera->repeating_request);
    if (!request) {
        return;
    }

    camera->plugin_interface->begin_state_change(camera);

    bool changed = false;

    if (types & PHOTO_CONTROL_LOCK_AE) {
        PhotoControlLockState lock_state = pcamera->control_state->ae_state;
        camera->ae_unlock_wait_lock = lock_state == PHOTO_CONTROL_STATE_FORCE_LOCKING;
        camera->ae_unlock = lock_state == PHOTO_CONTROL_STATE_FORCE_LOCKED ||
                camera->ae_unlock_wait_lock;

        // If a precapture sequence was in progress, it must be cancelled
        // before attempting to unlock AE.
        if (lock_state == PHOTO_CONTROL_STATE_PENDING || camera->ae_precapture) {
            uint8_t value = ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL;
            ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &value);
            camera->ae_precapture = false;

            pcamera->control_state->ae_state = PHOTO_CONTROL_STATE_UNLOCKING;
            pcamera->control_state->locks_changed |= PHOTO_CONTROL_LOCK_AE;
            changed = true;
        } else if (camera->ae_unlock) {
            set_ae_lock_for_request(pcamera, request, false);

            pcamera->control_state->ae_state = PHOTO_CONTROL_STATE_UNLOCKING;
            pcamera->control_state->locks_changed |= PHOTO_CONTROL_LOCK_AE;
            changed = true;
        }
    }

    if (types & PHOTO_CONTROL_LOCK_AF) {
        PhotoControlLockState lock_state = pcamera->control_state->af_state;
        if (lock_state == PHOTO_CONTROL_STATE_PENDING || camera->af_active_scan ||
                lock_state == PHOTO_CONTROL_STATE_LOCKED) {
            uint8_t value = ACAMERA_CONTROL_AF_TRIGGER_CANCEL;
            ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_TRIGGER, 1, &value);
            camera->af_active_scan = false;

            pcamera->control_state->af_state = PHOTO_CONTROL_STATE_UNLOCKING;
            pcamera->control_state->locks_changed |= PHOTO_CONTROL_LOCK_AF;
            changed = true;
        }
    }

    ACameraCaptureSession_capture(camera->session, &camera->capture_callbacks, 1, &request, NULL);

    if (camera->ae_unlock) {
        set_ae_lock_for_request(pcamera, camera->preview_request, false);
        set_ae_lock_for_request(pcamera, camera->still_capture_request, false);
        set_ae_lock_for_request(pcamera, camera->recording_request, false);
        set_ae_lock_for_request(pcamera, camera->video_snapshot_request, false);
        update_repeating_request(camera);
    }

    camera->plugin_interface->end_state_change(camera, changed);
}

static PhotoStream *dmp_camera_create_stream(PhotoCamera *pcamera, PhotoStreamUsage usage)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);

    if (!camera->device) {
        ALOGE("camera must be opened before calling create_stream");
        return NULL;
    }

    DroidMediaPhotoStream *stream = new DroidMediaPhotoStream;

    if (!camera->plugin_interface->init_stream(camera, stream, usage, &dmp_stream_impl)) {
        return nullptr;
    }

    return stream->photo_stream;
}

static bool dmp_camera_request_streams(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    camera_status_t status;

    if (!camera->device) {
        ALOGE("camera must be opened before calling request_streams");
        return false;
    }

    ACaptureSessionOutputContainer *outputs = NULL;

    status = ACaptureSessionOutputContainer_create(&outputs);
    if (status != ACAMERA_OK) {
        return false;
    }

    for (int i = 0; i < pcamera->num_streams; i++) {
        if (!setup_stream(camera, pcamera->streams[i], outputs)) {
            ACaptureSessionOutputContainer_free(outputs);
            return false;
        }
    }

    status = ACameraDevice_createCaptureSession(camera->device, outputs,
            &camera->capture_session_state_callbacks, &camera->session);
    ACaptureSessionOutputContainer_free(outputs);
    if (status != ACAMERA_OK) {
        return false;
    }

    return true;
}

static void dmp_camera_release_streams(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);

    if (camera->session) {
        ACameraCaptureSession_close(camera->session);
        camera->session = NULL;
    }

    {
        android::AutoMutex lock(camera->droid_sequence_lock);

        while (!camera->stopping_streams.empty()) {
            camera->plugin_interface->stream_stopped(camera->stopping_streams.front());
            camera->stopping_streams.pop_front();
        }
    }

    for (int i = 0; i < pcamera->num_streams; i++) {
        cleanup_stream(camera, pcamera->streams[i]);
    }
}

static const PhotoCameraImpl dmp_camera_impl = {
    .init = dmp_camera_init,
    .destroy = dmp_camera_destroy,
    .open = dmp_camera_open,
    .close = dmp_camera_close,
    .query_ae_compensation = dmp_camera_query_ae_compensation,
    .query_flash_modes = dmp_camera_query_flash_modes,
    .query_focus_modes = dmp_camera_query_focus_modes,
    .query_lens_focus = dmp_camera_query_lens_focus,
    .query_zoom = dmp_camera_query_zoom,
    .update_params = dmp_camera_update_params,
    .trigger = dmp_camera_trigger,
    .force_lock = dmp_camera_force_lock,
    .unlock = dmp_camera_unlock,
    .create_stream = dmp_camera_create_stream,
    .request_streams = dmp_camera_request_streams,
    .release_streams = dmp_camera_release_streams,
};

extern "C" {

bool droid_media_photo_plugin_init(const DroidMediaPhotoInterface *interface)
{
    camera_status_t status;
    ACameraIdList *camera_id_list = NULL;
    ACameraManager *camera_manager = ACameraManager_create();

    status = ACameraManager_getCameraIdList(camera_manager, &camera_id_list);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera id list: %d", status);
        return false;
    }

    for (int i = 0; i < camera_id_list->numCameras; i++) {
        DroidMediaPhotoCamera *camera = new DroidMediaPhotoCamera;
        const char *id = camera_id_list->cameraIds[i];

        camera->plugin_interface = interface;
        status = ACameraManager_getCameraCharacteristics(camera_manager, id, &camera->metadata);
        if (status == ACAMERA_OK) {
            interface->register_camera(camera, id, &dmp_camera_impl);
        } else {
            ALOGE("Failed to get camera characteristics for camera '%s': %d", id, status);
        }
    }

    ACameraManager_deleteCameraIdList(camera_id_list);
    ACameraManager_delete(camera_manager);

    return true;
}

};
