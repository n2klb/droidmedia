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

// Defines some useful fourcc video formats
#include <linux/videodev2.h>

#include <utils/SystemClock.h>

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
#include <media/MediaProfiles.h>
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
    DroidMediaPhotoBuffer *buffer = nullptr;
};

class DroidMediaPhotoMetadata : public android::RefBase {
public:
    DroidMediaPhotoMetadata(ACameraMetadata *data)
        : m_data(data)
    {
    }

    ~DroidMediaPhotoMetadata()
    {
        ACameraMetadata_free(m_data);
    }

    void read(PhotoMetadataRequest *req);

private:
    ACameraMetadata *m_data;
};

struct DroidMediaPhotoStream : public PhotoBackendStream {
    ACameraOutputTarget *output_target = nullptr;
    ACaptureSessionOutput *output = nullptr;
    android::sp<android::IGraphicBufferProducer> producer;
    android::sp<android::IGraphicBufferConsumer> consumer;
    android::sp<DroidMediaPhotoStreamListener> listener;
    android::sp<ANativeWindow> window;
    android::Mutex slot_lock;
    DroidMediaPhotoBufferSlot slots[android::BufferQueue::NUM_BUFFER_SLOTS];
    std::deque<android::sp<DroidMediaPhotoMetadata>> metadata_queue;

    int sequence_id = -1;
};

struct DroidMediaPhotoCamera : public PhotoBackendCamera {
    const DroidMediaPhotoInterface *plugin_interface = nullptr;
    char *id = nullptr;
    ACameraManager *manager = nullptr;
    ACameraMetadata *metadata = nullptr;
    ACameraDevice *device = nullptr;
    ACameraCaptureSession *session = nullptr;
    ACaptureRequest *preview_request = nullptr;
    ACaptureRequest *still_capture_request = nullptr;
    ACaptureRequest *recording_request = nullptr;
    ACaptureRequest *video_snapshot_request = nullptr;
    ACaptureRequest *repeating_request = nullptr;

    android::Mutex droid_sequence_lock;
    android::Mutex metadata_lock;
    int picture_seq_id = -1;
    bool ae_precapture = false;
    bool ae_precapture_locked = false;
    bool ae_unlock = false;
    bool ae_unlock_wait_lock = false;
    bool af_active_scan = false;
    std::deque<DroidMediaPhotoStream *> stopping_streams;
    std::unordered_map<int, std::vector<DroidMediaPhotoStream *> *> active_stream_lists;

    int32_t max_video_width = 0;
    int32_t max_video_height = 0;

    int32_t max_default_width = INT_MAX;
    int32_t max_default_height = INT_MAX;

    int32_t white_level = 0;
    int32_t color_filter_arrangement = -1;

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
        void *data = nullptr;

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
    case 0:
        if (pstream->usage == PHOTO_STREAM_USAGE_PREVIEW) {
            usage = android::GraphicBuffer::USAGE_HW_TEXTURE;
        } else if (pstream->usage == PHOTO_STREAM_USAGE_RECORDING) {
            usage = android::GraphicBuffer::USAGE_HW_VIDEO_ENCODER;
        }
        format = HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED;
        break;
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_NV21:
        format = HAL_PIXEL_FORMAT_YCBCR_420_888;
        break;
    case V4L2_PIX_FMT_JPEG:
        format = HAL_PIXEL_FORMAT_BLOB;
        break;
    case V4L2_PIX_FMT_SBGGR10P:
    case V4L2_PIX_FMT_SGBRG10P:
    case V4L2_PIX_FMT_SGRBG10P:
    case V4L2_PIX_FMT_SRGGB10P:
        format = HAL_PIXEL_FORMAT_RAW10;
        break;
    case V4L2_PIX_FMT_SBGGR12P:
    case V4L2_PIX_FMT_SGBRG12P:
    case V4L2_PIX_FMT_SGRBG12P:
    case V4L2_PIX_FMT_SRGGB12P:
        format = HAL_PIXEL_FORMAT_RAW12;
        break;
    case V4L2_PIX_FMT_SBGGR10:
    case V4L2_PIX_FMT_SGBRG10:
    case V4L2_PIX_FMT_SGRBG10:
    case V4L2_PIX_FMT_SRGGB10:
    case V4L2_PIX_FMT_SBGGR12:
    case V4L2_PIX_FMT_SGBRG12:
    case V4L2_PIX_FMT_SGRBG12:
    case V4L2_PIX_FMT_SRGGB12:
    case V4L2_PIX_FMT_SBGGR14:
    case V4L2_PIX_FMT_SGBRG14:
    case V4L2_PIX_FMT_SGRBG14:
    case V4L2_PIX_FMT_SRGGB14:
    case V4L2_PIX_FMT_SBGGR16:
    case V4L2_PIX_FMT_SGBRG16:
    case V4L2_PIX_FMT_SGRBG16:
    case V4L2_PIX_FMT_SRGGB16:
        format = HAL_PIXEL_FORMAT_RAW16;
        break;
    default:
        ALOGE("unexpected image format 0x%08x", pstream->config->format);
        return false;
    }

    android::BufferQueue::createBufferQueue(&stream->producer, &stream->consumer);

    stream->consumer->setMaxAcquiredBufferCount(kMaxAcquiredBuffers);
    stream->consumer->setConsumerName(android::String8("PhotoStream"));
    stream->consumer->setConsumerUsageBits(usage);
    stream->consumer->setDefaultBufferFormat(format);
    if (pstream->config->format == V4L2_PIX_FMT_JPEG)
        stream->consumer->setDefaultBufferDataSpace(HAL_DATASPACE_V0_JFIF);
    stream->consumer->setDefaultBufferSize(pstream->config->width,
                                           pstream->config->height);

    stream->listener = android::sp<DroidMediaPhotoStreamListener>::make(stream);

    // controlledByApp needs to be true for queue to drop buffers
    if (stream->consumer->consumerConnect(stream->listener, true) != android::NO_ERROR) {
        ALOGE("Failed to set buffer consumer");
        return false;
    }

    stream->window = android::sp<android::Surface>::make(stream->producer, true);

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
        stream->output = nullptr;
    }

    if (stream->output_target) {
        ACameraOutputTarget_free(stream->output_target);
        stream->output_target = nullptr;
    }

    stream->window.clear();

    stream->consumer->consumerDisconnect();
    stream->listener.clear();

    teardown_buffers(stream);

    stream->consumer.clear();
    stream->producer.clear();

    android::AutoMutex lock(camera->metadata_lock);
    stream->metadata_queue.clear();
}

static bool update_repeating_request(DroidMediaPhotoCamera *camera)
{
    PhotoCamera *pcamera = camera->photo_camera;
    bool recording = false, preview = false;
    int min_fps = 0, max_fps = INT_MAX;

    ALOGD("update_repeating_request");

    std::vector<DroidMediaPhotoStream *> *streams = nullptr;

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

        if (!streams) {
            streams = new std::vector<DroidMediaPhotoStream *>;
        }

        streams->push_back(static_cast<DroidMediaPhotoStream *>(pstream->backend));

        if (pstream->usage == PHOTO_STREAM_USAGE_PREVIEW) {
            preview = true;
        } else {
            recording = true;
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
        camera->repeating_request = nullptr;

        camera->plugin_interface->begin_state_change(camera);

        bool changed = false;

        camera->ae_precapture = false;
        camera->af_active_scan = false;

        if (pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AE] != PHOTO_CONTROL_STATE_DISABLED) {
            pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AE] = PHOTO_CONTROL_STATE_DISABLED;
            changed = true;
        }
        if (pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AF] != PHOTO_CONTROL_STATE_DISABLED) {
            pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AF] = PHOTO_CONTROL_STATE_DISABLED;
            changed = true;
        }

        camera->plugin_interface->end_state_change(camera, changed);

        ACameraCaptureSession_stopRepeating(camera->session);
        return true;
    }

    ACaptureRequest_setUserContext(camera->repeating_request, streams);

    int32_t fps_range[2] = { min_fps, max_fps };
    ACaptureRequest_setEntry_i32(camera->repeating_request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE,
                                 2, fps_range);

    android::AutoMutex lock(camera->droid_sequence_lock);

    int seq_id = -1;
    camera_status_t status = ACameraCaptureSession_setRepeatingRequest(camera->session,
            &camera->capture_callbacks, 1, &camera->repeating_request, &seq_id);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to set repeating request");
        ACaptureRequest_setUserContext(camera->repeating_request, nullptr);
        delete streams;
        return false;
    }

    camera->active_stream_lists[seq_id] = streams;
    ALOGD("bound sequence id %d: %p - repeating request", seq_id, streams);

    for (int i = 0; i < pcamera->num_streams; i++) {
        PhotoStream *pstream = pcamera->streams[i];
        DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pstream->backend);

        if (pstream->running) {
            stream->sequence_id = seq_id;
        }
    }

    return true;
}

static bool convert_format(DroidMediaPhotoCamera *camera, int32_t format, unsigned int *out)
{
    switch (format) {
    case AIMAGE_FORMAT_PRIVATE:
    case AIMAGE_FORMAT_RAW_PRIVATE:
        *out = 0;
        return true;
    case AIMAGE_FORMAT_YUV_420_888:
        // TODO: we need a way to tell which YUV format the camera supports.
        // The YUV_420_888 format is Android's "flexible YCbCr" format, which
        // means it could be I420, YV12, NV21, NV12 or even something else.
        // It is not possible to determine the format without mapping the
        // buffer, but some programs like GStreamer need to know it in advance.
        // Assume here that the format is NV21, since this seems to be the most
        // common case. On devices that use a different format,
        // new_buffer_from_item() will detect that and fail.
        *out = V4L2_PIX_FMT_NV21;
        return true;
    case AIMAGE_FORMAT_JPEG:
        *out = V4L2_PIX_FMT_JPEG;
        return true;
    case AIMAGE_FORMAT_RAW10:
        switch (camera->color_filter_arrangement) {
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB:
            *out = V4L2_PIX_FMT_SRGGB10P;
            return true;
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GRBG:
            *out = V4L2_PIX_FMT_SGRBG10P;
            return true;
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GBRG:
            *out = V4L2_PIX_FMT_SGBRG10P;
            return true;
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_BGGR:
            *out = V4L2_PIX_FMT_SBGGR10P;
            return true;
        default:
            return false;
        }
    case AIMAGE_FORMAT_RAW12:
        switch (camera->color_filter_arrangement) {
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB:
            *out = V4L2_PIX_FMT_SRGGB12P;
            return true;
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GRBG:
            *out = V4L2_PIX_FMT_SGRBG12P;
            return true;
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GBRG:
            *out = V4L2_PIX_FMT_SGBRG12P;
            return true;
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_BGGR:
            *out = V4L2_PIX_FMT_SBGGR12P;
            return true;
        default:
            return false;
        }
    case AIMAGE_FORMAT_RAW16:
        switch (camera->color_filter_arrangement) {
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB:
            *out = camera->white_level >= (1 << 14) ? V4L2_PIX_FMT_SRGGB16 :
                   camera->white_level >= (1 << 12) ? V4L2_PIX_FMT_SRGGB14 :
                   camera->white_level >= (1 << 10) ? V4L2_PIX_FMT_SRGGB12 :
                   V4L2_PIX_FMT_SRGGB10;
            return true;
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GRBG:
            *out = camera->white_level >= (1 << 14) ? V4L2_PIX_FMT_SGRBG16 :
                   camera->white_level >= (1 << 12) ? V4L2_PIX_FMT_SGRBG14 :
                   camera->white_level >= (1 << 10) ? V4L2_PIX_FMT_SGRBG12 :
                   V4L2_PIX_FMT_SGRBG10;
            return true;
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GBRG:
            *out = camera->white_level >= (1 << 14) ? V4L2_PIX_FMT_SGBRG16 :
                   camera->white_level >= (1 << 12) ? V4L2_PIX_FMT_SGBRG14 :
                   camera->white_level >= (1 << 10) ? V4L2_PIX_FMT_SGBRG12 :
                   V4L2_PIX_FMT_SGBRG10;
            return true;
        case ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_BGGR:
            *out = camera->white_level >= (1 << 14) ? V4L2_PIX_FMT_SBGGR16 :
                   camera->white_level >= (1 << 12) ? V4L2_PIX_FMT_SBGGR14 :
                   camera->white_level >= (1 << 10) ? V4L2_PIX_FMT_SBGGR12 :
                   V4L2_PIX_FMT_SBGGR10;
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

static void enumerate_fps_ranges(DroidMediaPhotoStream *stream, const PhotoConfig *filter,
                                 PhotoConfigIteratorCallback cb, void *userdata,
                                 PhotoStreamConfigFlags flags, PhotoAvailableConfig *info,
                                 ACameraMetadata_const_entry &target_fps_ranges)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(stream->photo_stream->camera->backend);

    if (filter && ((filter->width >= 0 && filter->width != info->width.min) ||
                    (filter->height >= 0 && filter->height != info->height.min) ||
                    (filter->format && filter->format != info->format))) {
        return;
    }

    // TODO: Checking AVAILABLE_MIN_FRAME_DURATIONS may be useful on some
    // devices, but on others it is inaccurate and makes the result worse.

    for (int i = 0; i < target_fps_ranges.count; i += 2) {
        int32_t min_fps = target_fps_ranges.data.i32[i + 0];
        int32_t max_fps = target_fps_ranges.data.i32[i + 1];

        if (filter && ((filter->min_fps >= 0 && filter->min_fps != min_fps) ||
                       (filter->max_fps >= 0 && filter->max_fps != max_fps))) {
            continue;
        }

        info->min_fps.min = min_fps;
        info->min_fps.max = min_fps;
        info->max_fps.min = max_fps;
        info->max_fps.max = max_fps;

        cb(userdata, info);
    }
}

static void dmp_stream_enumerate_configs(PhotoStream *pstream, const PhotoConfig *filter,
                                         PhotoConfigIteratorCallback cb, void *userdata,
                                         PhotoStreamConfigFlags flags, PhotoAvailableConfig *info)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pstream->camera->backend);
    DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pstream->backend);

    ACameraMetadata_const_entry entry, target_fps_ranges;
    camera_status_t status;

    status = ACameraMetadata_getConstEntry(camera->metadata,
            ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &target_fps_ranges);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get available target FPS ranges");
        return;
    }

    int32_t max_width = INT32_MAX, max_height = INT32_MAX;
    int32_t recommended_format = -1;

    if (pstream->usage == PHOTO_STREAM_USAGE_RECORDING) {
        max_width = camera->max_video_width;
        max_height = camera->max_video_height;
    }

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
        case PHOTO_STREAM_USAGE_RAW:
            stream_usecase = ACAMERA_SCALER_AVAILABLE_RECOMMENDED_STREAM_CONFIGURATIONS_RAW;
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

                int32_t width = entry.data.i32[i + 0];
                int32_t height = entry.data.i32[i + 1];
                int32_t format = entry.data.i32[i + 2];
                int32_t usecases = entry.data.i32[i + 4];

                if (usecases & (1 << stream_usecase)) {
                    if (!convert_format(camera, format, &info->format)) {
                        continue;
                    }

                    info->width.min = width;
                    info->width.max = width;
                    info->height.min = height;
                    info->height.max = height;

                    enumerate_fps_ranges(stream, filter, cb, userdata, flags, info,
                                         target_fps_ranges);
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
            case PHOTO_STREAM_USAGE_RAW:
                recommended_format = AIMAGE_FORMAT_RAW16;
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
            int32_t format = entry.data.i32[i + 0];
            int32_t width = entry.data.i32[i + 1];
            int32_t height = entry.data.i32[i + 2];

            if (entry.data.i32[i + 3]) {
                // ignore input formats
                continue;
            }

            bool format_valid = true;

            if (recommended_format >= 0) {
                format_valid = (format == recommended_format);
            } else if (pstream->usage == PHOTO_STREAM_USAGE_STILL_CAPTURE) {
                format_valid = (format == AIMAGE_FORMAT_JPEG);
            } else if (pstream->usage == PHOTO_STREAM_USAGE_RAW) {
                format_valid = (format == AIMAGE_FORMAT_RAW10 ||
                                format == AIMAGE_FORMAT_RAW12 ||
                                format == AIMAGE_FORMAT_RAW16 ||
                                format == AIMAGE_FORMAT_RAW_PRIVATE);
            } else {
                format_valid &= format != AIMAGE_FORMAT_JPEG &&
                                format != AIMAGE_FORMAT_RAW10 &&
                                format != AIMAGE_FORMAT_RAW12 &&
                                format != AIMAGE_FORMAT_RAW16 &&
                                format != AIMAGE_FORMAT_RAW_PRIVATE;

                if (pstream->usage != PHOTO_STREAM_USAGE_PREVIEW &&
                        pstream->usage != PHOTO_STREAM_USAGE_RECORDING) {
                    format_valid &= format != AIMAGE_FORMAT_PRIVATE;
                }
            }

            if (!format_valid || !convert_format(camera, format, &info->format) ||
                    width > camera->max_default_width || height > camera->max_default_height ||
                    width > max_width || height > max_height) {
                continue;
            }

            info->width.min = width;
            info->width.max = width;
            info->height.min = height;
            info->height.max = height;

            enumerate_fps_ranges(stream, filter, cb, userdata, flags, info, target_fps_ranges);
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

    {
        android::AutoMutex lock(camera->droid_sequence_lock);
        ALOGD("stream will stop with sequence ID %d", stream->sequence_id);
        camera->stopping_streams.push_back(stream);
    }

    ACaptureRequest_removeTarget(camera->preview_request, stream->output_target);
    ACaptureRequest_removeTarget(camera->still_capture_request, stream->output_target);
    ACaptureRequest_removeTarget(camera->recording_request, stream->output_target);
    ACaptureRequest_removeTarget(camera->video_snapshot_request, stream->output_target);

    update_repeating_request(camera);
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

    std::vector<DroidMediaPhotoStream *> *streams =
        new std::vector<DroidMediaPhotoStream *>;

    streams->push_back(stream);

    for (int i = 0; i < pstream->camera->num_streams; i++) {
        PhotoStream *other_stream = pstream->camera->streams[i];
        if (other_stream->running) {
            streams->push_back(static_cast<DroidMediaPhotoStream *>(other_stream->backend));
        }
    }

    ACaptureRequest_setUserContext(request, streams);

    status = ACameraCaptureSession_capture(camera->session,
            &camera->capture_callbacks, 1, &request, &camera->picture_seq_id);
    if (status == ACAMERA_OK) {
        camera->active_stream_lists[camera->picture_seq_id] = streams;
        ALOGD("bound sequence id %d: %p - still capture", camera->picture_seq_id, streams);
    } else {
        ALOGE("Submitting a capture request failed");
        delete streams;
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

    if (format == HAL_PIXEL_FORMAT_BLOB ||
            format == HAL_PIXEL_FORMAT_RAW_OPAQUE) {
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
    } else if (format == HAL_PIXEL_FORMAT_RAW10 ||
                format == HAL_PIXEL_FORMAT_RAW12 ||
                format == HAL_PIXEL_FORMAT_RAW16) {
        num_planes = 1;
        planes = new PhotoBufferPlaneInfo[1];
        planes[0].stride = buffer->graphic_buffer->stride *
                            (format == HAL_PIXEL_FORMAT_RAW16 ? 2 : 1);
        planes[0].max_length = planes[0].stride * buffer->graphic_buffer->height;
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

void DroidMediaPhotoMetadata::read(PhotoMetadataRequest *req)
{
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    switch (req->id) {
    case PHOTO_CONTROL_EXPOSURE_TIME:
        status = ACameraMetadata_getConstEntry(m_data, ACAMERA_SENSOR_EXPOSURE_TIME, &entry);
        if (status != ACAMERA_OK || entry.count != 1)
            break;

        req->dest.f[0] = entry.data.i64[0] / 1000000000.0;
        req->out_valid = true;
        break;

    case PHOTO_CONTROL_SENSITIVITY:
        status = ACameraMetadata_getConstEntry(m_data, ACAMERA_SENSOR_SENSITIVITY, &entry);
        if (status != ACAMERA_OK || entry.count != 1)
            break;

        req->dest.f[0] = entry.data.i32[0];
        req->out_valid = true;
        break;

    case PHOTO_CONTROL_CHANNEL_GAINS:
        status = ACameraMetadata_getConstEntry(m_data, ACAMERA_COLOR_CORRECTION_GAINS, &entry);
        if (status != ACAMERA_OK || entry.count != 4)
            break;

        for (int i = 0; i < 4; i++)
            req->dest.f[i] = entry.data.f[i];
        req->out_valid = true;
        break;

    case PHOTO_CONTROL_COLOR_TRANSFORM:
        status = ACameraMetadata_getConstEntry(m_data, ACAMERA_COLOR_CORRECTION_TRANSFORM, &entry);
        if (status != ACAMERA_OK || entry.count != 9)
            break;

        for (int i = 0; i < 9; i++)
            req->dest.f[i] = (float)entry.data.r[i].numerator / (float)entry.data.r[i].denominator;
        req->out_valid = true;
        break;

    default:
        break;
    }
}

static PhotoBuffer *dmp_stream_get_next_buffer(PhotoStream *pstream,
                                               int num_meta_reqs,
                                               PhotoMetadataRequest *meta_reqs)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pstream->camera->backend);
    DroidMediaPhotoStream *stream = static_cast<DroidMediaPhotoStream *>(pstream->backend);

    if (!stream->consumer.get()) {
        ALOGE("Cannot get buffer from inactive stream");
        return nullptr;
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

    if (buffer) {
        android::AutoMutex lock(camera->metadata_lock);
        if (stream->metadata_queue.empty()) {
            ALOGE("got buffer but no metadata");
        } else {
            DroidMediaPhotoMetadata *metadata = stream->metadata_queue.front().get();

            for (int i = 0; i < num_meta_reqs; i++) {
                metadata->read(&meta_reqs[i]);
            }

            stream->metadata_queue.pop_front();
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

    PhotoControlLockState lock_state = pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AF];

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

        if (lock_state != pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AF]) {
            ALOGV("-> af_lock_state %d", lock_state);
            pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AF] = lock_state;
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

    PhotoControlLockState lock_state = pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AE];

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

        if (lock_state != pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AE]) {
            ALOGV("-> ae_lock_state %d", lock_state);
            pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AE] = lock_state;
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

    {
        android::AutoMutex lock(camera->droid_sequence_lock);
        if (camera->session != session) {
            ALOGD("old session, ignoring capture result");
            return;
        }
    }


    ACameraMetadata *metadata = ACameraMetadata_copy(result);
    if (!metadata) {
        return;
    }

    android::sp<DroidMediaPhotoMetadata> metadata_ref =
        android::sp<DroidMediaPhotoMetadata>::make(metadata);

    void *user_context = nullptr;
    ACaptureRequest_getUserContext(request, &user_context);
    if (user_context) {
        std::vector<DroidMediaPhotoStream *> *streams =
            static_cast<std::vector<DroidMediaPhotoStream *> *>(user_context);

        android::AutoMutex lock(camera->metadata_lock);
        for (DroidMediaPhotoStream *stream : *streams) {
            stream->metadata_queue.push_back(metadata_ref);
        }
    } else {
        ALOGE("request user context is null");
    }

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
    if (failure->reason != CAPTURE_FAILURE_REASON_FLUSHED) {
        ALOGW("capture error");
    }
}

static void capture_session_on_capture_sequence_completed(
    void *context, ACameraCaptureSession *session,
    int sequenceId, int64_t frameNumber)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(context);

    ALOGD("capture sequence completed: %d", sequenceId);

    android::AutoMutex lock(camera->droid_sequence_lock);

    if (camera->session != session) {
        ALOGD("old session, ignoring event");
        return;
    }

    auto it = camera->active_stream_lists.find(sequenceId);
    if (it != camera->active_stream_lists.end()) {
        ALOGD("erase %p", it->second);
        delete it->second;
        camera->active_stream_lists.erase(it);
    }

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

    if (camera->session != session) {
        ALOGD("old session, ignoring event");
        return;
    }

    auto it = camera->active_stream_lists.find(sequenceId);
    if (it != camera->active_stream_lists.end()) {
        ALOGD("erase %p", it->second);
        delete it->second;
        camera->active_stream_lists.erase(it);
    }

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
            ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE, &entry);
    if (status == ACAMERA_OK) {
        ALOGD("Total pixel array size: %dx%d", entry.data.i32[0], entry.data.i32[1]);
    }

    status = ACameraMetadata_getConstEntry(camera->metadata,
            ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE_MAXIMUM_RESOLUTION, &entry);
    if (status == ACAMERA_OK) {
        ALOGD("Total high-res pixel array size: %dx%d", entry.data.i32[0], entry.data.i32[1]);
    }

    status = ACameraMetadata_getConstEntry(camera->metadata,
            ACAMERA_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, &entry);
    if (status == ACAMERA_OK) {
        ALOGD("Pre-correction active region: %d,%d %dx%d", entry.data.i32[0],
              entry.data.i32[1], entry.data.i32[2], entry.data.i32[3]);
    }

    status = ACameraMetadata_getConstEntry(camera->metadata,
            ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &entry);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera active pixel array size: %d", status);
        return false;
    }

    ALOGD("Default pixel array region: %d,%d %dx%d", entry.data.i32[0],
          entry.data.i32[1], entry.data.i32[2], entry.data.i32[3]);

    info->active_array_width = entry.data.i32[2];
    info->active_array_height = entry.data.i32[3];

    camera->max_default_width = info->active_array_width;
    camera->max_default_height = info->active_array_height;

#if ANDROID_MAJOR >= 12
    status = ACameraMetadata_getConstEntry(camera->metadata,
            ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE_MAXIMUM_RESOLUTION, &entry);
    if (status == ACAMERA_OK) {
        ALOGD("High-res pixel array region: %d,%d %dx%d", entry.data.i32[0],
              entry.data.i32[1], entry.data.i32[2], entry.data.i32[3]);
        info->active_array_width = entry.data.i32[2];
        info->active_array_height = entry.data.i32[3];
    } else {
        ALOGD("High-resolution mode not supported");
    }
#endif

    status = ACameraMetadata_getConstEntry(camera->metadata,
            ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &entry);
    if (status == ACAMERA_OK) {
        ALOGD("color filter arrangement: %d", entry.data.i32[0]);
        camera->color_filter_arrangement = entry.data.i32[0];
    } else {
        ALOGW("Failed to get color filter arrangement, raw capture unsupported?");
    }

    status = ACameraMetadata_getConstEntry(camera->metadata,
            ACAMERA_SENSOR_INFO_WHITE_LEVEL, &entry);
    if (status == ACAMERA_OK) {
        ALOGD("white level: 0x%x", entry.data.i32[0]);
        camera->white_level = entry.data.i32[0];
    } else {
        ALOGW("Failed to get white level, raw capture unsupported?");
    }

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

    if (camera->device) {
        ACameraDevice_close(camera->device);
        camera->device = nullptr;
    }

    if (camera->preview_request) {
        ACaptureRequest_free(camera->preview_request);
        camera->preview_request = nullptr;
    }

    if (camera->still_capture_request) {
        ACaptureRequest_free(camera->still_capture_request);
        camera->still_capture_request = nullptr;
    }

    if (camera->recording_request) {
        ACaptureRequest_free(camera->recording_request);
        camera->recording_request = nullptr;
    }

    if (camera->video_snapshot_request) {
        ACaptureRequest_free(camera->video_snapshot_request);
        camera->video_snapshot_request = nullptr;
    }

    if (camera->manager) {
        ACameraManager_delete(camera->manager);
        camera->manager = nullptr;
    }
}

static void dmp_camera_query_ae_compensation(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    PhotoControlFloatRange *ae_compensation = &pcamera->ae_compensation_info;
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

static void dmp_camera_query_exposure_modes(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_CONTROL_AE_AVAILABLE_MODES,
                                           &entry);
    if (status == ACAMERA_OK) {
        for (int i = 0; i < entry.count; i++) {
            switch (entry.data.u8[i]) {
            case ACAMERA_CONTROL_AE_MODE_OFF:
                photo_set_bit(pcamera->exposure_mode_info, PHOTO_MANUAL_EXPOSURE);
                photo_set_bit(pcamera->exposure_mode_info, PHOTO_PRIORITIZE_EXPOSURE_TIME);
                photo_set_bit(pcamera->exposure_mode_info, PHOTO_PRIORITIZE_SENSITIVITY);
                break;
            case ACAMERA_CONTROL_AE_MODE_ON:
                // Documentation says that this is supported on all devices
                photo_set_bit(pcamera->exposure_mode_info, PHOTO_AUTO_EXPOSURE);
                break;
            default:
                break;
            }
        }
    }
}

static void dmp_camera_query_exposure_time(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE,
                                           &entry);
    if (status == ACAMERA_OK && entry.count == 2) {
        pcamera->exposure_time_info.min = (float)entry.data.i64[0] / 1000000000;
        pcamera->exposure_time_info.max = (float)entry.data.i64[1] / 1000000000;
    }
}

static void dmp_camera_query_flash_modes(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_FLASH_INFO_AVAILABLE, &entry);
    if (status == ACAMERA_OK && entry.count == 1 &&
            entry.data.u8[0] == ACAMERA_FLASH_INFO_AVAILABLE_TRUE) {
        photo_set_bit(pcamera->flash_mode_info, PHOTO_AUTO_FLASH);
        photo_set_bit(pcamera->flash_mode_info, PHOTO_AUTO_FLASH_REDEYE);
        photo_set_bit(pcamera->flash_mode_info, PHOTO_FLASH_ON);
        photo_set_bit(pcamera->flash_mode_info, PHOTO_FLASH_OFF);
        photo_set_bit(pcamera->flash_mode_info, PHOTO_FLASH_TORCH);
    }
}

static void dmp_camera_query_focus_modes(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_CONTROL_AF_AVAILABLE_MODES,
                                           &entry);
    if (status == ACAMERA_OK) {
        for (int i = 0; i < entry.count; i++) {
            switch (entry.data.u8[i]) {
            case ACAMERA_CONTROL_AF_MODE_AUTO:
            case ACAMERA_CONTROL_AF_MODE_MACRO:
                photo_set_bit(pcamera->focus_mode_info, PHOTO_AUTO_FOCUS);
                break;
            case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
            case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
                photo_set_bit(pcamera->focus_mode_info, PHOTO_CONTINUOUS_AUTO_FOCUS);
                break;
            case ACAMERA_CONTROL_AF_MODE_OFF:
                photo_set_bit(pcamera->focus_mode_info, PHOTO_MANUAL_FOCUS);
                break;
            default:
                break;
            }
        }
    }
}

static void dmp_camera_query_lens_focus(PhotoCamera *pcamera)
{
    // TODO: lens focus
}

static void dmp_camera_query_sensitivity(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE,
                                           &entry);
    if (status == ACAMERA_OK && entry.count == 2) {
        pcamera->sensitivity_info.min = entry.data.i32[0];
        pcamera->sensitivity_info.max = entry.data.i32[1];
    }
}

static void dmp_camera_query_wb_modes(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_CONTROL_AWB_AVAILABLE_MODES,
                                           &entry);
    if (status == ACAMERA_OK) {
        for (int i = 0; i < entry.count; i++) {
            switch (entry.data.u8[i]) {
            case ACAMERA_CONTROL_AWB_MODE_OFF:
                photo_set_bit(pcamera->white_balance_mode_info, PHOTO_MANUAL_WHITE_BALANCE);
                break;
            case ACAMERA_CONTROL_AWB_MODE_AUTO:
                photo_set_bit(pcamera->white_balance_mode_info, PHOTO_AUTO_WHITE_BALANCE);
                break;
            case ACAMERA_CONTROL_AWB_MODE_INCANDESCENT:
                photo_set_bit(pcamera->white_balance_mode_info, PHOTO_INCANDESCENT);
                break;
            case ACAMERA_CONTROL_AWB_MODE_FLUORESCENT:
                photo_set_bit(pcamera->white_balance_mode_info, PHOTO_FLUORESCENT);
                break;
            case ACAMERA_CONTROL_AWB_MODE_WARM_FLUORESCENT:
                photo_set_bit(pcamera->white_balance_mode_info, PHOTO_WARM_FLUORESCENT);
                break;
            case ACAMERA_CONTROL_AWB_MODE_DAYLIGHT:
                photo_set_bit(pcamera->white_balance_mode_info, PHOTO_SUNLIGHT);
                break;
            case ACAMERA_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT:
                photo_set_bit(pcamera->white_balance_mode_info, PHOTO_CLOUDY);
                break;
            case ACAMERA_CONTROL_AWB_MODE_TWILIGHT:
                photo_set_bit(pcamera->white_balance_mode_info, PHOTO_TWILIGHT);
                break;
            case ACAMERA_CONTROL_AWB_MODE_SHADE:
                photo_set_bit(pcamera->white_balance_mode_info, PHOTO_SHADE);
                break;
            default:
                break;
            }
        }
    }
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
        pcamera->zoom_info.min = entry.data.f[0];
        pcamera->zoom_info.max = entry.data.f[1];
    }
#else
    status = ACameraMetadata_getConstEntry(camera->metadata,
                                           ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &entry);
    if (status == ACAMERA_OK && entry.count == 1) {
        pcamera->zoom_info.min = 1.0f;
        pcamera->zoom_info.max = entry.data.f[0];
    }
#endif
}

static void dmp_camera_query_scene_modes(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    ACameraMetadata_const_entry entry;
    camera_status_t status;

    photo_set_bit(pcamera->scene_mode_info, PHOTO_NORMAL_SCENE);

    status = ACameraMetadata_getConstEntry(camera->metadata, ACAMERA_CONTROL_AVAILABLE_SCENE_MODES,
                                           &entry);
    if (status == ACAMERA_OK) {
        for (int i = 0; i < entry.count; i++) {
            switch (entry.data.u8[i]) {
            case ACAMERA_CONTROL_SCENE_MODE_PORTRAIT:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_PORTRAIT_SCENE);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_LANDSCAPE:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_LANDSCAPE_SCENE);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_SPORTS:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_SPORTS);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_NIGHT:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_NIGHT);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_ACTION:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_ACTION);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_NIGHT_PORTRAIT:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_NIGHT_PORTRAIT);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_THEATRE:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_THEATRE);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_BEACH:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_BEACH);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_SNOW:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_SNOW);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_SUNSET:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_SUNSET);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_STEADYPHOTO:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_STEADY_PHOTO);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_FIREWORKS:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_FIREWORKS);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_PARTY:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_PARTY);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_CANDLELIGHT:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_CANDLELIGHT);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_BARCODE:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_BARCODE);
                break;
            case ACAMERA_CONTROL_SCENE_MODE_HDR:
                photo_set_bit(pcamera->scene_mode_info, PHOTO_HDR);
                break;
            default:
                break;
            }
        }
    }
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
    uint8_t value_u8;

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_AE_COMPENSATION)) {
        int32_t value_i32 = round(pcamera->ae_compensation / pcamera->ae_compensation_info.step);
        ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION,
                                     1, &value_i32);
    }

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_EXPOSURE_TIME)) {
        int64_t value_i64 = round(pcamera->exposure_time * 1000000000);
        ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &value_i64);
    }

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_SENSITIVITY)) {
        int32_t value_i32 = round(pcamera->sensitivity);
        ACaptureRequest_setEntry_i32(request, ACAMERA_SENSOR_SENSITIVITY, 1, &value_i32);
    }

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_SCENE_MODE)) {
        switch (pcamera->scene_mode) {
        case PHOTO_NORMAL_SCENE:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_DISABLED;
            break;
        case PHOTO_PORTRAIT_SCENE:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_PORTRAIT;
            break;
        case PHOTO_LANDSCAPE_SCENE:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_LANDSCAPE;
            break;
        case PHOTO_SPORTS:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_SPORTS;
            break;
        case PHOTO_NIGHT:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_NIGHT;
            break;
        case PHOTO_ACTION:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_ACTION;
            break;
        case PHOTO_NIGHT_PORTRAIT:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_NIGHT_PORTRAIT;
            break;
        case PHOTO_THEATRE:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_THEATRE;
            break;
        case PHOTO_BEACH:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_BEACH;
            break;
        case PHOTO_SNOW:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_SNOW;
            break;
        case PHOTO_SUNSET:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_SUNSET;
            break;
        case PHOTO_STEADY_PHOTO:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_STEADYPHOTO;
            break;
        case PHOTO_FIREWORKS:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_FIREWORKS;
            break;
        case PHOTO_PARTY:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_PARTY;
            break;
        case PHOTO_CANDLELIGHT:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_CANDLELIGHT;
            break;
        case PHOTO_BARCODE:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_BARCODE;
            break;
        case PHOTO_HDR:
            value_u8 = ACAMERA_CONTROL_SCENE_MODE_HDR;
            break;
        }

        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_SCENE_MODE, 1, &value_u8);

        if (pcamera->scene_mode == PHOTO_NORMAL_SCENE) {
            value_u8 = ACAMERA_CONTROL_MODE_AUTO;
        } else {
            value_u8 = ACAMERA_CONTROL_MODE_USE_SCENE_MODE;
        }

        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_MODE, 1, &value_u8);
    }

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_EXPOSURE_MODE) ||
            photo_check_bit(pcamera->dirty, PHOTO_CONTROL_FLASH_MODE)) {
        if (pcamera->exposure_mode == PHOTO_MANUAL_EXPOSURE) {
            value_u8 = ACAMERA_CONTROL_AE_MODE_OFF;
        } else {
            switch (pcamera->flash_mode) {
            case PHOTO_AUTO_FLASH:
                value_u8 = ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH;
                break;
            case PHOTO_AUTO_FLASH_REDEYE:
                value_u8 = ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE;
                break;
            case PHOTO_FLASH_ON:
                value_u8 = ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH;
                break;
            default:
                value_u8 = ACAMERA_CONTROL_AE_MODE_ON;
                break;
            }
        }

#if 0
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_MODE, 1, &value_u8);

        // This is a relatively new addition (API level 36), and MediaTek doesn't implement it.
        switch (pcamera->exposure_mode) {
        case PHOTO_PRIORITIZE_EXPOSURE_TIME:
            value_u8 = ACAMERA_CONTROL_AE_PRIORITY_MODE_SENSOR_EXPOSURE_TIME_PRIORITY;
            break;
        case PHOTO_PRIORITIZE_SENSITIVITY:
            value_u8 = ACAMERA_CONTROL_AE_PRIORITY_MODE_SENSOR_SENSITIVITY_PRIORITY;
            break;
        default:
            value_u8 = ACAMERA_CONTROL_AE_PRIORITY_MODE_OFF;
            break;
        }
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_PRIORITY_MODE, 1, &value_u8);
#else
        // This is the alternative that seems to work, although it is non-standard.
        // TODO: find a way to check if the device supports this.
        if (pcamera->exposure_mode == PHOTO_PRIORITIZE_EXPOSURE_TIME) {
            int32_t value_i32 = 0;
            ACaptureRequest_setEntry_i32(request, ACAMERA_SENSOR_SENSITIVITY, 1, &value_i32);
            value_u8 = ACAMERA_CONTROL_AE_MODE_OFF;
        } else if (pcamera->exposure_mode == PHOTO_PRIORITIZE_SENSITIVITY) {
            int64_t value_i64 = 0;
            ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &value_i64);
            value_u8 = ACAMERA_CONTROL_AE_MODE_OFF;
        }

        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_MODE, 1, &value_u8);
#endif

        switch (pcamera->flash_mode) {
        case PHOTO_FLASH_TORCH:
            value_u8 = ACAMERA_FLASH_MODE_TORCH;
            break;
        case PHOTO_FLASH_ON:
            value_u8 = ACAMERA_FLASH_MODE_SINGLE;
            break;
        default:
            value_u8 = ACAMERA_FLASH_MODE_OFF;
            break;
        }
        ACaptureRequest_setEntry_u8(request, ACAMERA_FLASH_MODE, 1, &value_u8);
    }

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_FOCUS_MODE)) {
        switch (pcamera->focus_mode) {
        case PHOTO_AUTO_FOCUS:
            value_u8 = ACAMERA_CONTROL_AF_MODE_AUTO;
            break;
        case PHOTO_CONTINUOUS_AUTO_FOCUS:
            if (is_video)
                value_u8 = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
            else
                value_u8 = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
            break;
        case PHOTO_MANUAL_FOCUS:
            value_u8 = ACAMERA_CONTROL_AF_MODE_OFF;
            break;
        default:
            ALOGW("Unknown AF mode, disabling AF");
            value_u8 = ACAMERA_CONTROL_AF_MODE_OFF;
            break;
        }
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_MODE, 1, &value_u8);
    }

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_LENS_FOCUS)) {
        // TODO: lens focus
    }

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_WHITE_BALANCE_MODE)) {
        switch (pcamera->white_balance_mode) {
        case PHOTO_MANUAL_WHITE_BALANCE:
            value_u8 = ACAMERA_CONTROL_AWB_MODE_OFF;
            break;
        case PHOTO_AUTO_WHITE_BALANCE:
            value_u8 = ACAMERA_CONTROL_AWB_MODE_AUTO;
            break;
        case PHOTO_INCANDESCENT:
            value_u8 = ACAMERA_CONTROL_AWB_MODE_INCANDESCENT;
            break;
        case PHOTO_FLUORESCENT:
            value_u8 = ACAMERA_CONTROL_AWB_MODE_FLUORESCENT;
            break;
        case PHOTO_WARM_FLUORESCENT:
            value_u8 = ACAMERA_CONTROL_AWB_MODE_WARM_FLUORESCENT;
            break;
        case PHOTO_SUNLIGHT:
            value_u8 = ACAMERA_CONTROL_AWB_MODE_DAYLIGHT;
            break;
        case PHOTO_CLOUDY:
            value_u8 = ACAMERA_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT;
            break;
        case PHOTO_TWILIGHT:
            value_u8 = ACAMERA_CONTROL_AWB_MODE_TWILIGHT;
            break;
        case PHOTO_SHADE:
            value_u8 = ACAMERA_CONTROL_AWB_MODE_SHADE;
            break;
        default:
            ALOGW("Unknown WB mode, enabling normal AWB");
            value_u8 = ACAMERA_CONTROL_AWB_MODE_AUTO;
            break;
        }
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AWB_MODE, 1, &value_u8);
    }

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_ZOOM)) {
#if ANDROID_MAJOR >= 11
        ACaptureRequest_setEntry_float(request, ACAMERA_CONTROL_ZOOM_RATIO, 1, &pcamera->zoom);
#else
        // TODO: crop
#endif
    }

    int32_t crop_region[4] = {
        0,
        0,
        pcamera->info->active_array_width,
        pcamera->info->active_array_height,
    };

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_AE_AREAS)) {
        uint32_t count;
        int32_t *values = convert_metering_areas(&pcamera->ae_areas, crop_region, &count);
        ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_REGIONS, count, values);
        delete[] values;
    }

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_AF_AREAS)) {
        uint32_t count;
        int32_t *values = convert_metering_areas(&pcamera->af_areas, crop_region, &count);
        ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AF_REGIONS, count, values);
        delete[] values;
    }

    if (photo_check_bit(pcamera->dirty, PHOTO_CONTROL_AWB_AREAS)) {
        uint32_t count;
        int32_t *values = convert_metering_areas(&pcamera->awb_areas, crop_region, &count);
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

static void dmp_camera_trigger(PhotoCamera *pcamera, PhotoControlLockMask locks)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);
    camera_status_t status;

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

    if (locks & PHOTO_CONTROL_LOCK_BIT_AE) {
        PhotoControlLockState lock_state = pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AE];
        if (lock_state != PHOTO_CONTROL_STATE_PENDING && !camera->ae_precapture &&
                lock_state != PHOTO_CONTROL_STATE_FORCE_LOCKED) {
            uint8_t value = ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START;
            ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &value);

            pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AE] = PHOTO_CONTROL_STATE_PENDING;
            changed = true;
        }
    }

    if (locks & PHOTO_CONTROL_LOCK_BIT_AF) {
        ACameraMetadata_const_entry entry;

        status = ACaptureRequest_getConstEntry(request, ACAMERA_CONTROL_AF_MODE, &entry);
        if (status == ACAMERA_OK && entry.data.u8[0] != ACAMERA_CONTROL_AF_MODE_OFF) {
            PhotoControlLockState lock_state = pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AF];
            if (lock_state != PHOTO_CONTROL_STATE_PENDING && !camera->af_active_scan &&
                    lock_state != PHOTO_CONTROL_STATE_LOCKED) {
                uint8_t value = ACAMERA_CONTROL_AF_TRIGGER_START;
                ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_TRIGGER, 1, &value);

                pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AF] = PHOTO_CONTROL_STATE_PENDING;
                changed = true;
            }
        }
    }

    camera->plugin_interface->end_state_change(camera, changed);

    // Copy stream list to avoid use-after-free on repeating sequence completion
    void *user_context = nullptr;
    std::vector<DroidMediaPhotoStream *> *streams = nullptr;
    ACaptureRequest_getUserContext(request, &user_context);
    if (user_context) {
        streams = static_cast<std::vector<DroidMediaPhotoStream *> *>(user_context);
        streams = new std::vector<DroidMediaPhotoStream *>(*streams);
        ACaptureRequest_setUserContext(request, streams);
    }

    int seq_id = -1;
    status = ACameraCaptureSession_capture(camera->session, &camera->capture_callbacks,
                                           1, &request, &seq_id);
    if (status == ACAMERA_OK) {
        android::AutoMutex lock(camera->droid_sequence_lock);
        camera->active_stream_lists[seq_id] = streams;
        ALOGD("bound sequence id %d: %p - lock trigger", seq_id, streams);
    } else {
        ALOGE("Capture request for lock trigger failed");
        delete streams;
    }
}

static void set_ae_lock_for_request(PhotoCamera *pcamera, ACaptureRequest *request, bool value)
{
    uint8_t value_u8 = value;

    ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_LOCK, 1, &value_u8);
}

static void dmp_camera_force_lock(PhotoCamera *pcamera, PhotoControlLockMask locks)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);

    if (!camera->repeating_request) {
        return;
    }

    camera->plugin_interface->begin_state_change(camera);

    bool changed = false;

    if (locks & PHOTO_CONTROL_LOCK_BIT_AE) {
        pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AE] = PHOTO_CONTROL_STATE_FORCE_LOCKING;
        changed = true;

        set_ae_lock_for_request(pcamera, camera->preview_request, true);
        set_ae_lock_for_request(pcamera, camera->still_capture_request, true);
        set_ae_lock_for_request(pcamera, camera->recording_request, true);
        set_ae_lock_for_request(pcamera, camera->video_snapshot_request, true);
    }

    camera->plugin_interface->end_state_change(camera, changed);
}

static void dmp_camera_unlock(PhotoCamera *pcamera, PhotoControlLockMask locks)
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

    if (locks & PHOTO_CONTROL_LOCK_BIT_AE) {
        PhotoControlLockState lock_state = pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AE];
        camera->ae_unlock_wait_lock = lock_state == PHOTO_CONTROL_STATE_FORCE_LOCKING;
        camera->ae_unlock = lock_state == PHOTO_CONTROL_STATE_FORCE_LOCKED ||
                camera->ae_unlock_wait_lock;

        // If a precapture sequence was in progress, it must be cancelled
        // before attempting to unlock AE.
        if (lock_state == PHOTO_CONTROL_STATE_PENDING || camera->ae_precapture) {
            uint8_t value = ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL;
            ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &value);
            camera->ae_precapture = false;

            pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AE] = PHOTO_CONTROL_STATE_UNLOCKING;
            changed = true;
        } else if (camera->ae_unlock) {
            set_ae_lock_for_request(pcamera, request, false);

            pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AE] = PHOTO_CONTROL_STATE_UNLOCKING;
            changed = true;
        }
    }

    if (locks & PHOTO_CONTROL_LOCK_BIT_AF) {
        PhotoControlLockState lock_state = pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AF];
        if (lock_state == PHOTO_CONTROL_STATE_PENDING || camera->af_active_scan ||
                lock_state == PHOTO_CONTROL_STATE_LOCKED) {
            uint8_t value = ACAMERA_CONTROL_AF_TRIGGER_CANCEL;
            ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_TRIGGER, 1, &value);
            camera->af_active_scan = false;

            pcamera->lock_states[PHOTO_CONTROL_LOCK_ID_AF] = PHOTO_CONTROL_STATE_UNLOCKING;
            changed = true;
        }
    }

    // Copy stream list to avoid use-after-free on repeating sequence completion
    void *user_context = nullptr;
    std::vector<DroidMediaPhotoStream *> *streams = nullptr;
    ACaptureRequest_getUserContext(request, &user_context);
    if (user_context) {
        streams = static_cast<std::vector<DroidMediaPhotoStream *> *>(user_context);
        streams = new std::vector<DroidMediaPhotoStream *>(*streams);
        ACaptureRequest_setUserContext(request, streams);
    }

    int seq_id = -1;
    camera_status_t status = ACameraCaptureSession_capture(camera->session,
                                &camera->capture_callbacks, 1, &request, &seq_id);
    if (status == ACAMERA_OK) {
        android::AutoMutex lock(camera->droid_sequence_lock);
        camera->active_stream_lists[seq_id] = streams;
        ALOGD("bound sequence id %d: %p - unlock trigger", seq_id, streams);
    } else {
        ALOGE("Capture request for unlock trigger failed");
        delete streams;
    }

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
        return nullptr;
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

    ACaptureSessionOutputContainer *outputs = nullptr;

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

    {
        android::AutoMutex lock(camera->droid_sequence_lock);

        status = ACameraDevice_createCaptureSession(camera->device, outputs,
                &camera->capture_session_state_callbacks, &camera->session);
    }

    ACaptureSessionOutputContainer_free(outputs);
    if (status != ACAMERA_OK) {
        return false;
    }

    return true;
}

static void dmp_camera_release_streams(PhotoCamera *pcamera)
{
    DroidMediaPhotoCamera *camera = static_cast<DroidMediaPhotoCamera *>(pcamera->backend);

    ALOGD("releasing streams");

    // At this point, the session may still remain open for some time since
    // ACameraCaptureSession_close() is asynchronous, but we must ensure that
    // it no longer has access to the capture sequence state. This is done
    // using session == camera->session checks in all callbacks.
    {
        android::AutoMutex lock(camera->droid_sequence_lock);

        if (camera->session) {
            ACameraCaptureSession_abortCaptures(camera->session);
            ACameraCaptureSession_close(camera->session);
            camera->session = nullptr;
        }

        for (auto it : camera->active_stream_lists) {
            ALOGD("erase %p", it.second);
            delete it.second;
        }

        camera->active_stream_lists.clear();

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
    .update_params = dmp_camera_update_params,
    .trigger = dmp_camera_trigger,
    .force_lock = dmp_camera_force_lock,
    .unlock = dmp_camera_unlock,
    .create_stream = dmp_camera_create_stream,
    .request_streams = dmp_camera_request_streams,
    .release_streams = dmp_camera_release_streams,
    .num_controls = PHOTO_CONTROL_ID_NUM,
    .query_control = (void (*[PHOTO_CONTROL_ID_NUM])(PhotoCamera *)){
        [PHOTO_CONTROL_AE_COMPENSATION] = dmp_camera_query_ae_compensation,
        [PHOTO_CONTROL_EXPOSURE_MODE] = dmp_camera_query_exposure_modes,
        [PHOTO_CONTROL_FLASH_MODE] = dmp_camera_query_flash_modes,
        [PHOTO_CONTROL_FOCUS_MODE] = dmp_camera_query_focus_modes,
        [PHOTO_CONTROL_LENS_FOCUS] = dmp_camera_query_lens_focus,
        [PHOTO_CONTROL_SENSITIVITY] = dmp_camera_query_sensitivity,
        [PHOTO_CONTROL_WHITE_BALANCE_MODE] = dmp_camera_query_wb_modes,
        [PHOTO_CONTROL_ZOOM] = dmp_camera_query_zoom,
        [PHOTO_CONTROL_SCENE_MODE] = dmp_camera_query_scene_modes,
    }
};

extern "C" {

bool droid_media_photo_plugin_init(const DroidMediaPhotoInterface *interface)
{
    camera_status_t status;
    ACameraIdList *camera_id_list = nullptr;
    ACameraManager *camera_manager = ACameraManager_create();

    struct {
        int32_t max_width = 0;
        int32_t max_height = 0;
    } video_info;

    // Based on frameworks/av/services/camera/libcameraservice/api1/client2/Parameters.cpp
    // Treat the H.264 max size as the max supported video size.
    // At some point, Android started using relative search paths for the
    // media profile XML files, which is why we need this hack to change to
    // the root directory before listing them.
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        ALOGE("pipe2() failed");
    } else {
        int ret = fork();
        if (ret == 0) {
            chdir("/");
            android::MediaProfiles *profiles = android::MediaProfiles::getInstance();
            android::Vector<android::video_encoder> encoders = profiles->getVideoEncoders();
            for (size_t i = 0; i < encoders.size(); i++) {
                int width = profiles->getVideoEncoderParamByName("enc.vid.width.max", encoders[i]);
                int height = profiles->getVideoEncoderParamByName("enc.vid.height.max", encoders[i]);
                if (width > video_info.max_width) {
                    video_info.max_width = width;
                }
                if (height > video_info.max_height) {
                    video_info.max_height = height;
                }
            }
            write(pipefd[1], &video_info, sizeof(video_info));
            _exit(0);
        } else if (ret > 0) {
            close(pipefd[1]);
            if (read(pipefd[0], &video_info, sizeof(video_info)) != sizeof(video_info)) {
                ALOGE("read() from pipe failed");
            }
            close(pipefd[0]);
        } else {
            ALOGE("fork() failed");
            close(pipefd[0]);
            close(pipefd[1]);
        }
    }

    ALOGD("Maximum supported video size: %dx%d",
          video_info.max_width, video_info.max_height);

    status = ACameraManager_getCameraIdList(camera_manager, &camera_id_list);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera id list: %d", status);
        return false;
    }

    for (int i = 0; i < camera_id_list->numCameras; i++) {
        DroidMediaPhotoCamera *camera = new DroidMediaPhotoCamera;
        const char *id = camera_id_list->cameraIds[i];

        camera->plugin_interface = interface;
        camera->max_video_width = video_info.max_width;
        camera->max_video_height = video_info.max_height;
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
