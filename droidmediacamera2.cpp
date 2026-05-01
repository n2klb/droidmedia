/*
 * Copyright (C) 2023 Jolla Ltd.
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

#if ANDROID_MAJOR >= 7
#include "droidmediacamera.h"

#include <camera/CameraParameters.h>
// This needs to be first because of broken includes in Android < 10
#include <camera/NdkCaptureRequest.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraError.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <media/hardware/MetadataBufferType.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/openmax/OMX_IVCommon.h>
#include <media/stagefright/CameraSource.h>

#if ANDROID_MAJOR <= 9
#include <android/native_window.h>
typedef ANativeWindow ACameraWindowType;
#endif

#include <string>
#include <unordered_map>
#include <set>
#include <cerrno>
#include <cstdlib>
#include <limits>

#include "droidmediabuffer.h"
#include "private.h"
#include "private2.h"

#undef LOG_TAG
#define LOG_TAG "DroidMediaCamera"

namespace android {
    int32_t getColorFormat(const char *colorFormat) {
        if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_YUV420P)) {
           return OMX_COLOR_FormatYUV420Planar;
        }

        if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_YUV422SP)) {
           return OMX_COLOR_FormatYUV422SemiPlanar;
        }

        if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_YUV420SP)) {
            return OMX_COLOR_FormatYUV420SemiPlanar;
        }

        if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_YUV422I)) {
            return OMX_COLOR_FormatYCbYCr;
        }

        if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_RGB565)) {
           return OMX_COLOR_Format16bitRGB565;
        }

        if (!strcmp(colorFormat, "OMX_TI_COLOR_FormatYUV420PackedSemiPlanar")) {
           return OMX_TI_COLOR_FormatYUV420PackedSemiPlanar;
        }
        if (!strcmp(colorFormat, CameraParameters::PIXEL_FORMAT_ANDROID_OPAQUE)) {
            return OMX_COLOR_FormatAndroidOpaque;
        }
        return -1;
    }

    const char *getColorFormatString(int32_t colorFormat, bool &found) {
        found = true;
        switch (colorFormat) {
        case AIMAGE_FORMAT_YUV_420_888: {
            std::string out = CameraParameters::PIXEL_FORMAT_YUV420P;
            out += ",";
            out += CameraParameters::PIXEL_FORMAT_YUV420SP;
            const char *o = out.c_str();
            return o;
        }
        default:
            found = false;
            return "";
        }
    }

}

extern "C" {

struct _DroidMediaCameraRecordingData
{
    android::sp<android::IMemory> mem;
    nsecs_t ts;
};

static void update_request(DroidMediaCamera *camera, ACaptureRequest *request,
    std::unordered_map<std::string, std::string> &param_map);

enum StillCaptureState {
    STILL_CAPTURE_STATE_IDLE = 0,
    STILL_CAPTURE_STATE_WAITING_PRECAPTURE_DONE,
    STILL_CAPTURE_STATE_CAPTURING,
};

enum PrecaptureState {
    PRECAPTURE_STATE_IDLE = 0,
    PRECAPTURE_STATE_PENDING,
    PRECAPTURE_STATE_BUSY,
    PRECAPTURE_STATE_LOCKED,
};

struct MeteringArea {
    int32_t xmin;
    int32_t ymin;
    int32_t xmax;
    int32_t ymax;
    int32_t weight;
};

struct _DroidMediaCamera
{
    _DroidMediaCamera() :
        m_cb_data(NULL) {
        memset(&m_cb, 0x0, sizeof(m_cb));
    }

    ~_DroidMediaCamera() {
        if (m_manager) {
            ACameraManager_delete(m_manager);
            m_manager = NULL;
        }
    }

    ACameraDevice *m_device = NULL;
    ACameraIdList *m_camera_id_list = NULL;
    ACameraManager *m_manager = NULL;
    ACameraMetadata *m_metadata = NULL;

    // Callbacks
    ACameraDevice_StateCallbacks m_device_state_callbacks;
    ACameraCaptureSession_stateCallbacks m_capture_session_state_callbacks;
    ACameraCaptureSession_captureCallbacks m_capture_callbacks;

    // Capture session
    ACameraCaptureSession *m_session = NULL;
    ACaptureSessionOutputContainer *m_capture_session_output_container = NULL;

    // Requests
    ACaptureRequest *m_preview_request = NULL;
    ACaptureRequest *m_image_request = NULL;
    ACaptureRequest *m_video_request = NULL;
    ACaptureRequest *m_ext_video_request = NULL;

    // Preview
    ACameraOutputTarget *m_preview_output_target = NULL;
    ACaptureSessionOutput *m_preview_output = NULL;
    ANativeWindow *m_preview_anw = NULL;
    bool m_preview_enabled = false;

    // Image capture
    AImageReader *m_image_reader = NULL;
    AImageReader_ImageListener m_image_listener;
    ANativeWindow *m_image_reader_anw = NULL;
    ACaptureSessionOutput *m_image_reader_output = NULL;
    ACameraOutputTarget *m_image_reader_output_target = NULL;

    // Video recording
    ACameraOutputTarget *m_video_output_target = NULL;
    ACaptureSessionOutput *m_video_output = NULL;
    ANativeWindow *m_video_anw = NULL;
    // External video recorder
    ACameraOutputTarget *m_ext_video_output_target = NULL;
    ACaptureSessionOutput *m_ext_video_output = NULL;

    bool m_video_recording_enabled = false;
    int m_preview_callback_flag = 0;

    // Queues
    android::sp<DroidMediaBufferQueue> m_queue;
    android::sp<DroidMediaBufferQueue> m_recording_queue;

    int32_t image_format = AIMAGE_FORMAT_JPEG;
    int32_t image_height = -1;
    int32_t image_width = -1;
    int32_t preview_height = -1;
    int32_t preview_width = -1;
    int32_t video_height = -1;
    int32_t video_width = -1;

    int32_t max_ae_regions = 0;
    int32_t max_awb_regions = 0;
    int32_t max_focus_regions = 0;
    StillCaptureState m_still_capture_state = STILL_CAPTURE_STATE_IDLE;
    int32_t m_still_capture_sequence_id = -1;
    PrecaptureState m_ae_precapture_state;
    int32_t m_ae_precapture_result_count = 0;
    PrecaptureState m_af_precapture_state;
    int32_t m_af_precapture_result_count = 0;
    std::unordered_map<std::string, std::string> m_param_map;

    DroidMediaCameraCallbacks m_cb;
    void *m_cb_data;
};

static bool restart_enabled_streams(DroidMediaCamera *camera)
{
    if (!camera || !camera->m_session) {
        return false;
    }

    if (camera->m_video_recording_enabled) {
        ACaptureRequest *request = NULL;
        if (camera->m_ext_video_request) {
            request = camera->m_ext_video_request;
        } else if (camera->m_video_request) {
            request = camera->m_video_request;
        }

        if (request) {
            camera_status_t status = ACameraCaptureSession_setRepeatingRequest(camera->m_session,
                &camera->m_capture_callbacks, 1, &request, NULL);
            if (status != ACAMERA_OK) {
                ALOGE("Failed to start video capture");
                return false;
            }
        }
    } else if (camera->m_preview_enabled && camera->m_preview_request) {
        camera_status_t status = ACameraCaptureSession_setRepeatingRequest(camera->m_session,
            &camera->m_capture_callbacks, 1, &camera->m_preview_request, NULL);
        if (status != ACAMERA_OK) {
            ALOGE("Failed to start preview");
            return false;
        }
    }

    return true;
}

static bool start_precapture_trigger(DroidMediaCamera *camera)
{
    if (!camera || !camera->m_session || !camera->m_preview_request || !camera->m_preview_enabled) {
        return false;
    }

    camera_status_t status;

    ACaptureRequest *request = ACaptureRequest_copy(camera->m_preview_request);
    if (!request) {
        return false;
    }

    PrecaptureState ae_state = camera->m_ae_precapture_state;
    PrecaptureState af_state = camera->m_af_precapture_state;

    if (ae_state == PRECAPTURE_STATE_IDLE) {
        uint8_t aeTrigger = ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START;
        status = ACaptureRequest_setEntry_u8(request,
            ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &aeTrigger);
        if (status == ACAMERA_OK) {
            ae_state = PRECAPTURE_STATE_PENDING;
            camera->m_ae_precapture_result_count = 0;
        } else {
            ALOGW("Failed to set AE precapture trigger to START");
        }
    }

    if (af_state == PRECAPTURE_STATE_IDLE) {
        ACameraMetadata_const_entry entry;
        status = ACaptureRequest_getConstEntry(request, ACAMERA_CONTROL_AF_MODE, &entry);
        if (status == ACAMERA_OK && entry.count > 0 && entry.data.u8[0] != ACAMERA_CONTROL_AF_MODE_OFF) {
            uint8_t afTrigger = ACAMERA_CONTROL_AF_TRIGGER_START;
            status = ACaptureRequest_setEntry_u8(request,
                ACAMERA_CONTROL_AF_TRIGGER, 1, &afTrigger);
            if (status == ACAMERA_OK) {
                af_state = PRECAPTURE_STATE_PENDING;
                camera->m_af_precapture_result_count = 0;
            } else {
                ALOGW("Failed to set AF trigger to START");
            }
        } else if (camera->m_cb.focus_cb) {
            ALOGI("skipping AF because it is disabled");
            camera->m_cb.focus_cb(camera->m_cb_data, 1);
        }
    }

    status = ACameraCaptureSession_capture(camera->m_session,
        &camera->m_capture_callbacks, 1, &request, NULL);

    ACaptureRequest_free(request);

    if (status == ACAMERA_OK) {
        camera->m_ae_precapture_state = ae_state;
        camera->m_af_precapture_state = af_state;
    } else {
        ALOGW("Failed to submit precapture trigger start request");
    }

    return status == ACAMERA_OK;
}

static bool cancel_precapture_trigger(DroidMediaCamera *camera)
{
    if (!camera || !camera->m_session || !camera->m_preview_request || !camera->m_preview_enabled) {
        return false;
    }

    camera_status_t status;

    ACaptureRequest *request = ACaptureRequest_copy(camera->m_preview_request);
    if (!request) {
        return false;
    }

    if (camera->m_ae_precapture_state != PRECAPTURE_STATE_IDLE) {
        uint8_t aeTrigger = ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL;
        status = ACaptureRequest_setEntry_u8(request,
            ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &aeTrigger);
        if (status != ACAMERA_OK) {
            ALOGW("Failed to set AE precapture trigger to CANCEL");
        }
    }

    if (camera->m_af_precapture_state != PRECAPTURE_STATE_IDLE) {
        uint8_t afTrigger = ACAMERA_CONTROL_AF_TRIGGER_CANCEL;
        status = ACaptureRequest_setEntry_u8(request,
            ACAMERA_CONTROL_AF_TRIGGER, 1, &afTrigger);
        if (status != ACAMERA_OK) {
            ALOGW("Failed to set AF trigger to CANCEL");
        }
    }

    status = ACameraCaptureSession_capture(camera->m_session,
        &camera->m_capture_callbacks, 1, &request, NULL);

    ACaptureRequest_free(request);

    if (status == ACAMERA_OK) {
        camera->m_ae_precapture_state = PRECAPTURE_STATE_IDLE;
        camera->m_af_precapture_state = PRECAPTURE_STATE_IDLE;
    } else {
        ALOGW("Failed to cancel precapture trigger cancel request");
    }

    return status == ACAMERA_OK;
}

static void clear_still_capture_state(DroidMediaCamera *camera)
{
    if (!camera) {
        return;
    }

    camera->m_still_capture_state = STILL_CAPTURE_STATE_IDLE;
    camera->m_still_capture_sequence_id = -1;
    camera->m_ae_precapture_state = PRECAPTURE_STATE_IDLE;
    camera->m_af_precapture_state = PRECAPTURE_STATE_IDLE;
}

static bool submit_still_capture_request(DroidMediaCamera *camera)
{
    if (!camera || !camera->m_session || !camera->m_image_request) {
        return false;
    }

    int32_t seq_id = -1;
    camera_status_t status = ACameraCaptureSession_capture(camera->m_session,
        &camera->m_capture_callbacks, 1, &camera->m_image_request, &seq_id);
    if (status != ACAMERA_OK) {
        ALOGE("Submitting still capture after precapture failed");
        clear_still_capture_state(camera);
        return false;
    }

    camera->m_still_capture_state = STILL_CAPTURE_STATE_CAPTURING;
    camera->m_still_capture_sequence_id = seq_id;
    camera->m_ae_precapture_state = PRECAPTURE_STATE_IDLE;
    camera->m_af_precapture_state = PRECAPTURE_STATE_IDLE;
    return true;
}

static bool continue_still_capture(DroidMediaCamera *camera)
{
    if (!camera || !camera->m_session || !camera->m_preview_request || !camera->m_preview_enabled) {
        return submit_still_capture_request(camera);
    }

    if (camera->m_ae_precapture_state == PRECAPTURE_STATE_PENDING ||
            camera->m_ae_precapture_state == PRECAPTURE_STATE_BUSY ||
            camera->m_af_precapture_state == PRECAPTURE_STATE_PENDING ||
            camera->m_af_precapture_state == PRECAPTURE_STATE_BUSY) {
        ALOGD("Waiting for precapture to complete");
        return true;
    }

    ALOGD("All precapture sequences done, starting capture");
    return submit_still_capture_request(camera);
}

static void finish_auto_focus(DroidMediaCamera *camera, int result)
{
    if (!camera) {
        return;
    }

    ALOGD("AF done, result: %i", result);

    camera->m_af_precapture_state = PRECAPTURE_STATE_LOCKED;

    if (camera->m_cb.focus_cb) {
        camera->m_cb.focus_cb(camera->m_cb_data, result);
    }

    if (camera->m_still_capture_state == STILL_CAPTURE_STATE_WAITING_PRECAPTURE_DONE) {
        continue_still_capture(camera);
    }
}

static void check_auto_focus_timeout(DroidMediaCamera *camera)
{
    if (!camera) {
        return;
    }

    static const int kMaxAutoFocusResults = 60;
    camera->m_af_precapture_result_count++;
    if (camera->m_af_precapture_result_count >= kMaxAutoFocusResults) {
        ALOGW("AF trigger timed out after %d results, continuing",
            camera->m_af_precapture_result_count);
        finish_auto_focus(camera, 0);
    }
}

static void process_auto_focus_result(DroidMediaCamera *camera, const ACameraMetadata *result)
{
    if (!camera || !result) {
        return;
    }

    ACameraMetadata_const_entry entry;
    camera_status_t status;

    if (camera->m_af_precapture_state == PRECAPTURE_STATE_PENDING) {
        status = ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AF_TRIGGER, &entry);
        if (status != ACAMERA_OK || entry.count <= 0) {
            check_auto_focus_timeout(camera);
            return;
        }

        if (entry.data.u8[0] != ACAMERA_CONTROL_AF_TRIGGER_START) {
            check_auto_focus_timeout(camera);
            return;
        }

        ALOGD("got first result for AF precapture");
        camera->m_af_precapture_state = PRECAPTURE_STATE_BUSY;
    }

    if (camera->m_af_precapture_state == PRECAPTURE_STATE_BUSY) {
        status = ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AF_STATE, &entry);
        if (status != ACAMERA_OK || entry.count <= 0) {
            check_auto_focus_timeout(camera);
            return;
        }

        uint8_t af_state = entry.data.u8[0];
        ALOGD("precapture AF state: %i", af_state);

        if (af_state == ACAMERA_CONTROL_AF_STATE_FOCUSED_LOCKED) {
            finish_auto_focus(camera, 1);
            return;
        }

        if (af_state == ACAMERA_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED) {
            finish_auto_focus(camera, 0);
            return;
        }

        check_auto_focus_timeout(camera);
    }
}

static void finish_ae_precapture(DroidMediaCamera *camera)
{
    if (!camera) {
        return;
    }

    ALOGD("AE done");

    camera->m_ae_precapture_state = PRECAPTURE_STATE_LOCKED;

    // TODO add a callback similar to focus_cb

    if (camera->m_still_capture_state == STILL_CAPTURE_STATE_WAITING_PRECAPTURE_DONE) {
        continue_still_capture(camera);
    }
}

static void check_ae_precapture_timeout(DroidMediaCamera *camera)
{
    if (!camera) {
        return;
    }

    static const int kMaxPrecaptureResults = 60;
    camera->m_ae_precapture_result_count++;
    if (camera->m_ae_precapture_result_count >= kMaxPrecaptureResults) {
        ALOGW("AE precapture trigger timed out after %d results, continuing",
            camera->m_ae_precapture_result_count);
        finish_ae_precapture(camera);
    }
}

static void process_ae_precapture_result(DroidMediaCamera *camera, const ACameraMetadata *result)
{
    if (!camera || !result) {
        return;
    }

    ACameraMetadata_const_entry entry;
    camera_status_t status;

    if (camera->m_ae_precapture_state == PRECAPTURE_STATE_PENDING) {
        status = ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, &entry);
        if (status != ACAMERA_OK && entry.count <= 0) {
            check_ae_precapture_timeout(camera);
            return;
        }

        if (entry.data.u8[0] != ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START) {
            check_ae_precapture_timeout(camera);
            return;
        }

        ALOGD("got first result for AE precapture");
        camera->m_ae_precapture_state = PRECAPTURE_STATE_BUSY;
    }

    if (camera->m_ae_precapture_state == PRECAPTURE_STATE_BUSY) {
        status = ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AE_STATE, &entry);
        if (status != ACAMERA_OK && entry.count <= 0) {
            check_ae_precapture_timeout(camera);
            return;
        }

        uint8_t ae_state = entry.data.u8[0];
        ALOGD("precapture AE state: %i", ae_state);

        if (ae_state == ACAMERA_CONTROL_AE_STATE_CONVERGED ||
                ae_state == ACAMERA_CONTROL_AE_STATE_FLASH_REQUIRED ||
                ae_state == ACAMERA_CONTROL_AE_STATE_LOCKED) {
            finish_ae_precapture(camera);
            return;
        }

        check_ae_precapture_timeout(camera);
    }
}

static void abort_still_capture_flow(DroidMediaCamera *camera, const char *reason)
{
    if (!camera) {
        return;
    }

    ALOGW("Aborting still capture flow: %s", reason ? reason : "unknown");
    cancel_precapture_trigger(camera);
    clear_still_capture_state(camera);
}

static void still_image_available(void *context, AImageReader *reader)
{
    ALOGI("Still image available");
// TODO send image data
    AImage *image = NULL;
    media_status_t status;

    status = AImageReader_acquireNextImage(reader, &image);

    if (status == AMEDIA_OK) {
        DroidMediaCamera *camera = (DroidMediaCamera *)context;
        DroidMediaData mem;
        int32_t format;

        status = AImage_getFormat(image, &format);

        switch (format) {
        case AIMAGE_FORMAT_JPEG: {
            int32_t num_planes = 0;

            status = AImage_getNumberOfPlanes(image, &num_planes);
            if (status != AMEDIA_OK || num_planes != 1) {
                break;
            }

            int plane_len = 0;
            uint8_t *plane_ptr = NULL;
            status = AImage_getPlaneData(image, 0, &plane_ptr, &plane_len);
            if (status == AMEDIA_OK && camera->m_cb.compressed_image_cb) {
                mem.data = plane_ptr;
                mem.size = (size_t)plane_len;
                camera->m_cb.compressed_image_cb(camera->m_cb_data, &mem);
            }
            break;
        }
/*
// TODO raw image
            if (m_cam->m_cb.raw_image_cb) {
                m_cam->m_cb.raw_image_cb(m_cam->m_cb_data, &mem);
            }
*/
        default:
            ALOGI("Unsupported image: %u", format);
            break;
        }
        AImage_delete(image);
    }
}

static void device_on_disconnected(void *context, ACameraDevice *device)
{
    ALOGI("Camera '%s' is diconnected.", ACameraDevice_getId(device));
}

static void device_on_error(void *context, ACameraDevice *device, int error)
{
    ALOGE("Error (%d) on Camera '%s'.", error, ACameraDevice_getId(device));
}

static void capture_session_on_active(void *context, ACameraCaptureSession *session)
{
    ALOGI("Session is activated. %p", session);
}

static void capture_session_on_closed(void *context, ACameraCaptureSession *session)
{
    ALOGI("Session is closed. %p", session);
}

static void capture_session_on_ready(void *context, ACameraCaptureSession *session)
{
    ALOGI("Session is ready. %p", session);
}

static void capture_session_on_capture_started(
    void *context, ACameraCaptureSession *session,
    const ACaptureRequest *request, int64_t timestamp)
{
    ACameraMetadata_const_entry entry;
    camera_status_t status = ACaptureRequest_getConstEntry(request, ACAMERA_CONTROL_CAPTURE_INTENT, &entry);

    if (status == ACAMERA_OK &&
            entry.data.u8[0] == ACAMERA_CONTROL_CAPTURE_INTENT_STILL_CAPTURE) {
        ALOGI("Calling shutter callback");
        DroidMediaCamera *camera = (DroidMediaCamera *)context;
        if (camera->m_cb.shutter_cb) {
            camera->m_cb.shutter_cb(camera->m_cb_data);
        }
    }
}

static void capture_session_on_capture_progressed(
    void *context, ACameraCaptureSession *session,
    ACaptureRequest *request, const ACameraMetadata *result)
{
    //TODO
}

static void capture_session_on_capture_completed(
    void *context, ACameraCaptureSession *session,
    ACaptureRequest *request, const ACameraMetadata *result)
{
    ALOGV("Capture completed: %p", context);
    (void)session;
    DroidMediaCamera *camera = (DroidMediaCamera *)context;

    process_auto_focus_result(camera, result);
    process_ae_precapture_result(camera, result);
}

static void capture_session_on_capture_failed(
    void *context, ACameraCaptureSession *session,
    ACaptureRequest *request, ACameraCaptureFailure *failure)
{
    (void)session;
    (void)failure;
    DroidMediaCamera *camera = (DroidMediaCamera *)context;
    ALOGW("Capture failed: %p", context);

    ACameraMetadata_const_entry entry;
    camera_status_t status;

    if (camera->m_af_precapture_state == PRECAPTURE_STATE_PENDING) {
        status = ACaptureRequest_getConstEntry(request, ACAMERA_CONTROL_AF_TRIGGER, &entry);
        if (status == ACAMERA_OK && entry.count > 0 &&
                entry.data.u8[0] == ACAMERA_CONTROL_AF_TRIGGER_START) {
            abort_still_capture_flow(camera, "AF trigger failed");
            return;
        }
    }

    if (camera->m_ae_precapture_state == PRECAPTURE_STATE_PENDING) {
        status = ACaptureRequest_getConstEntry(request, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, &entry);
        if (status == ACAMERA_OK && entry.count > 0 &&
                entry.data.u8[0] == ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START) {
            abort_still_capture_flow(camera, "AE precapture trigger failed");
            return;
        }
    }
}

static void capture_session_on_capture_sequence_completed(
    void *context, ACameraCaptureSession *session,
    int sequenceId, int64_t frameNumber)
{
    (void)session;
    (void)frameNumber;
    DroidMediaCamera *camera = (DroidMediaCamera *)context;
    ALOGI("Capture sequence completed: %p", context);

    if (camera->m_still_capture_state == STILL_CAPTURE_STATE_CAPTURING &&
            sequenceId == camera->m_still_capture_sequence_id) {
        clear_still_capture_state(camera);
    }
}

static void capture_session_on_capture_sequence_abort(
    void *context, ACameraCaptureSession *session, int sequenceId)
{
    (void)session;
    DroidMediaCamera *camera = (DroidMediaCamera *)context;
    ALOGI("Capture sequence aborted: %p", context);
    if (camera->m_still_capture_state == STILL_CAPTURE_STATE_CAPTURING &&
            sequenceId == camera->m_still_capture_sequence_id) {
        abort_still_capture_flow(camera, "still capture sequence aborted");
    }
}

static void capture_session_on_capture_buffer_lost(
    void *context, ACameraCaptureSession *session,
    ACaptureRequest *request, ACameraWindowType *window, int64_t frameNumber)
{
    ALOGI("Capture buffer lost: %p", context);
}

DroidMediaBufferQueue *droid_media_camera_get_buffer_queue (DroidMediaCamera *camera)
{
    ALOGD("get_buffer_queue");
    return camera->m_queue.get();
}

DroidMediaBufferQueue *droid_media_camera_get_recording_buffer_queue (DroidMediaCamera *camera)
{
    ALOGD("get_recording_buffer_queue");
    return camera->m_recording_queue.get();
}

int droid_media_camera_get_number_of_cameras()
{
    int num_cameras;
    camera_status_t status;
    ACameraIdList *camera_id_list = NULL;
    ACameraManager *camera_manager = ACameraManager_create();

    status = ACameraManager_getCameraIdList(camera_manager, &camera_id_list);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera id list (status: %d)", status);
        return 0;
    }

    num_cameras = camera_id_list->numCameras;

    ACameraManager_deleteCameraIdList(camera_id_list);
    ACameraManager_delete(camera_manager);

    return num_cameras;
}

bool droid_media_camera_get_info(DroidMediaCameraInfo *info, int camera_number)
{
    const char *selected_camera_id = NULL;
    camera_status_t status;
    ACameraIdList *camera_id_list = NULL;
    ACameraMetadata *camera_metadata = NULL;
    ACameraMetadata_const_entry entry;
    ACameraManager *camera_manager = ACameraManager_create();
    size_t num_physical_cameras = 0;
    const char *const *physical_camera_ids = NULL;

    status = ACameraManager_getCameraIdList(camera_manager, &camera_id_list);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera id list: %d", status);
        goto fail;
    }
    ALOGD("Get info from camera %i of %i", camera_number, camera_id_list->numCameras);

    if (camera_id_list->numCameras <= 0 ||
            camera_number < 0 ||
            camera_number >= camera_id_list->numCameras) {
        ALOGE("Invalid camera number");
        goto fail;
    }

    selected_camera_id = camera_id_list->cameraIds[camera_number];

    status = ACameraManager_getCameraCharacteristics(camera_manager,
                                                     selected_camera_id,
                                                     &camera_metadata);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera characteristics for camera '%s': %d",
            camera_id_list->cameraIds[camera_number], status);
        goto fail;
    }

#if ANDROID_MAJOR >= 10
    if (ACameraMetadata_isLogicalMultiCamera(camera_metadata,
                                             &num_physical_cameras,
                                             &physical_camera_ids)) {
        ALOGI("Multicamera with physical camera count %zu", num_physical_cameras);
    }
#endif

    status = ACameraMetadata_getConstEntry(camera_metadata,
                                           ACAMERA_LENS_FACING,
                                           &entry);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera lens facing: %d", status);
        goto fail;
    }

    if (entry.data.u8[0] == ACAMERA_LENS_FACING_FRONT) {
        info->facing = DROID_MEDIA_CAMERA_FACING_FRONT;
    } else {
        info->facing = DROID_MEDIA_CAMERA_FACING_BACK;
    }

    status = ACameraMetadata_getConstEntry(camera_metadata,
                                           ACAMERA_SENSOR_ORIENTATION,
                                           &entry);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera orientation: %d", status);
        goto fail;
    }

    info->orientation = entry.data.i32[0];
    ALOGI("Camera %i facing %i orientation %i", camera_number, info->facing, info->orientation);

fail:
    if (camera_metadata) {
        ACameraMetadata_free(camera_metadata);
    }
    if (camera_id_list) {
        ACameraManager_deleteCameraIdList(camera_id_list);
    }
    ACameraManager_delete(camera_manager);

    return status == ACAMERA_OK;
}

bool setup_image_reader(DroidMediaCamera *camera)
{
    media_status_t media_status;

    if (camera->m_image_reader) {
        AImageReader_delete(camera->m_image_reader);
        camera->m_image_reader = NULL;
    }

    media_status = AImageReader_new(
        camera->image_width, camera->image_height, camera->image_format,
        2, &camera->m_image_reader);
    if (media_status != AMEDIA_OK) {
        ALOGE("Create image reader failed with status %d", media_status);
        goto fail;
    }

    media_status = AImageReader_setImageListener(camera->m_image_reader, &camera->m_image_listener);
    if (media_status != AMEDIA_OK) {
        ALOGE("Set AImageReader listener failed with status %d", media_status);
        goto fail;
    }

    media_status = AImageReader_getWindow(camera->m_image_reader, &camera->m_image_reader_anw);
    if (media_status != AMEDIA_OK) {
        ALOGE("AImageReader_getWindow failed with status %d", media_status);
        goto fail;
    }

    return true;

fail:
    if (camera->m_image_reader) {
        AImageReader_delete(camera->m_image_reader);
        camera->m_image_reader = NULL;
    }

    return false;
}

void destroy_capture_session(DroidMediaCamera *camera)
{
    clear_still_capture_state(camera);

    if (camera->m_session) {
        ACameraCaptureSession_close(camera->m_session);
        camera->m_session = NULL;
    }

    if (camera->m_image_reader_output_target) {
        ACameraOutputTarget_free(camera->m_image_reader_output_target);
        camera->m_image_reader_output_target = NULL;
    }

    if (camera->m_image_reader_output) {
        ACaptureSessionOutput_free(camera->m_image_reader_output);
        camera->m_image_reader_output = NULL;
    }

    if (camera->m_preview_output_target) {
        ACameraOutputTarget_free(camera->m_preview_output_target);
        camera->m_preview_output_target = NULL;
    }

    if (camera->m_preview_output) {
        ACaptureSessionOutput_free(camera->m_preview_output);
        camera->m_preview_output = NULL;
    }

    if (camera->m_video_output_target) {
        ACameraOutputTarget_free(camera->m_video_output_target);
        camera->m_video_output_target = NULL;
    }

    if (camera->m_video_output) {
        ACaptureSessionOutput_free(camera->m_video_output);
        camera->m_video_output = NULL;
    }

    if (camera->m_capture_session_output_container) {
        ACaptureSessionOutputContainer_free(camera->m_capture_session_output_container);
        camera->m_capture_session_output_container = NULL;
    }

    if (camera->m_preview_request) {
        ACaptureRequest_free(camera->m_preview_request);
        camera->m_preview_request = NULL;
    }

    if (camera->m_image_request != NULL) {
        ACaptureRequest_free(camera->m_image_request);
        camera->m_image_request = NULL;
    }

    if (camera->m_video_request) {
        ACaptureRequest_free(camera->m_video_request);
        camera->m_video_request = NULL;
    }
    if (camera->m_preview_anw) {
        ANativeWindow_release(camera->m_preview_anw);
        camera->m_preview_anw = NULL;
    }

    if (camera->m_video_anw) {
        ANativeWindow_release(camera->m_video_anw);
        camera->m_video_anw = NULL;
    }
}

bool setup_capture_session(DroidMediaCamera *camera)
{
    camera_status_t status;

    ALOGD("setup_capture_session start");

    camera->m_queue->setBufferSizeFormat(camera->preview_width, camera->preview_height,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED);

    camera->m_preview_anw = camera->m_queue->window();
    ANativeWindow_acquire(camera->m_preview_anw);

    ALOGD("preview window format %i", ANativeWindow_getFormat(camera->m_preview_anw));

    status = ACaptureSessionOutputContainer_create(&camera->m_capture_session_output_container);
    if (status != ACAMERA_OK) {
        goto fail;
    }

    // Preview
    ALOGI("setup_capture_session preview");
    status = ACameraDevice_createCaptureRequest(camera->m_device,
        TEMPLATE_PREVIEW, &camera->m_preview_request);
    if (status != ACAMERA_OK) {
        goto fail;
    }

    status = ACameraOutputTarget_create(camera->m_preview_anw, &camera->m_preview_output_target);
    if (status != ACAMERA_OK) {
        goto fail;
    }

    status = ACaptureRequest_addTarget(camera->m_preview_request, camera->m_preview_output_target);
    if (status != ACAMERA_OK) {
        goto fail;
    }

    status = ACaptureSessionOutput_create(camera->m_preview_anw, &camera->m_preview_output);
    if (status != ACAMERA_OK) {
        goto fail;
    }

    status = ACaptureSessionOutputContainer_add(camera->m_capture_session_output_container,
        camera->m_preview_output);
    if (status != ACAMERA_OK) {
        goto fail;
    }
    ALOGD("preview window format %i", ANativeWindow_getFormat(camera->m_preview_anw));

    ALOGD("setup_capture_session preview done");

    // Video
    if (camera->video_width != -1 && camera->video_height != -1) {
        if (camera->m_recording_queue.get()) {
            camera->m_recording_queue->setBufferSizeFormat(camera->video_width, camera->video_height,
#if (ANDROID_MAJOR < 8)
                HAL_PIXEL_FORMAT_YCbCr_420_888);
#else
                HAL_PIXEL_FORMAT_YCBCR_420_888);
#endif

            camera->m_video_anw = camera->m_recording_queue->window();
            ANativeWindow_acquire(camera->m_video_anw);

            ALOGD("video window format %i", ANativeWindow_getFormat(camera->m_video_anw));

            ALOGI("setup_capture_session video");
            status = ACameraDevice_createCaptureRequest(camera->m_device,
                TEMPLATE_PREVIEW, &camera->m_video_request);
            if (status != ACAMERA_OK) {
                goto fail;
            }

            status = ACameraOutputTarget_create(camera->m_video_anw, &camera->m_video_output_target);
            if (status != ACAMERA_OK) {
                goto fail;
            }

            status = ACaptureRequest_addTarget(camera->m_video_request, camera->m_video_output_target);
            if (status != ACAMERA_OK) {
                goto fail;
            }

            ALOGD("camera->m_video_anw %p", camera->m_video_anw);

            status = ACaptureSessionOutput_create(camera->m_video_anw, &camera->m_video_output);
            if (status != ACAMERA_OK) {
                ALOGE("ACaptureSessionOutput_create failed %i", status);
                goto fail;
            }

            status = ACaptureSessionOutputContainer_add(camera->m_capture_session_output_container,
                camera->m_video_output);
            if (status != ACAMERA_OK) {
                goto fail;
            }

            ALOGD("video window format %i", ANativeWindow_getFormat(camera->m_video_anw));
        } else {
            ALOGW("No recording queue available, skipping video output setup");
        }
    }

    if (camera->image_height != -1 && camera->image_width != -1) {
        ALOGI("setup_capture_session image");
        status = ACameraDevice_createCaptureRequest(camera->m_device,
            TEMPLATE_STILL_CAPTURE, &camera->m_image_request);
        if (status != ACAMERA_OK) {
            goto fail;
        }

        status = ACameraOutputTarget_create(camera->m_image_reader_anw, &camera->m_image_reader_output_target);
        if (status != ACAMERA_OK) {
            goto fail;
        }

        status = ACaptureRequest_addTarget(camera->m_image_request, camera->m_image_reader_output_target);
        if (status != ACAMERA_OK) {
            goto fail;
        }

        // Keep preview running during still capture; this improves device-side 3A/flash behavior.
        if (camera->m_preview_output_target) {
            status = ACaptureRequest_addTarget(camera->m_image_request, camera->m_preview_output_target);
            if (status != ACAMERA_OK) {
                ALOGW("Failed to add preview target to still request, continuing without it (status=%d)", status);
            }
        }

        status = ACaptureSessionOutput_create(camera->m_image_reader_anw, &camera->m_image_reader_output);
        if (status != ACAMERA_OK) {
            goto fail;
        }

        status = ACaptureSessionOutputContainer_add(camera->m_capture_session_output_container,
            camera->m_image_reader_output);
        if (status != ACAMERA_OK) {
            goto fail;
        }
        ALOGD("setup_capture_session image done");
    }

    if (!camera->m_param_map.empty()) {
        if (camera->m_preview_request) {
            update_request(camera, camera->m_preview_request, camera->m_param_map);
        }
        if (camera->m_video_request) {
            update_request(camera, camera->m_video_request, camera->m_param_map);
        }
        if (camera->m_image_request) {
            update_request(camera, camera->m_image_request, camera->m_param_map);
        }
    }

    status = ACameraDevice_createCaptureSession(
        camera->m_device, camera->m_capture_session_output_container,
        &camera->m_capture_session_state_callbacks, &camera->m_session);
    if (status != ACAMERA_OK) {
        ALOGE("setup_capture_session failed (status: %i)", status);
        goto fail;
    }

    ALOGD("setup_capture_session done");
    return true;

fail:
    destroy_capture_session(camera);

    return false;
}

DroidMediaCamera *droid_media_camera_connect(int camera_number)
{
    camera_status_t status;
    ACameraDevice *camera_device = NULL;
    DroidMediaCamera *camera = new DroidMediaCamera;
    android::sp<DroidMediaBufferQueue> queue;
    android::sp<DroidMediaBufferQueue> recording_queue;
    const char *selected_camera_id = NULL;

    if (!camera) {
        ALOGE("Failed to allocate DroidMediaCamera");
        return NULL;
    }

    camera->m_manager = ACameraManager_create();

    ALOGI("Opening camera %i", camera_number);

    status = ACameraManager_getCameraIdList(camera->m_manager, &camera->m_camera_id_list);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera id list: %d", status);
        goto fail;
    }

    if (camera->m_camera_id_list->numCameras <= 0 ||
            camera_number < 0 ||
            camera_number >= camera->m_camera_id_list->numCameras) {
        ALOGE("Invalid camera number");
        goto fail;
    }

    selected_camera_id = camera->m_camera_id_list->cameraIds[camera_number];
    ALOGI("Selected camera %s", selected_camera_id);

    // Set device state callbacks
    camera->m_device_state_callbacks.context = camera;
    camera->m_device_state_callbacks.onDisconnected = device_on_disconnected;
    camera->m_device_state_callbacks.onError = device_on_error;

    status = ACameraManager_openCamera(camera->m_manager, selected_camera_id, &camera->m_device_state_callbacks, &camera_device);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to open camera %s with error: %i", selected_camera_id, status);
        goto fail;
    }

    ALOGD("Camera %s opened", selected_camera_id);
    camera->m_device = camera_device;

    status = ACameraManager_getCameraCharacteristics(camera->m_manager,
                                                     selected_camera_id,
                                                     &camera->m_metadata);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to get camera characteristics: %d", status);
        goto fail;
    }

    queue = new DroidMediaBufferQueue("DroidMediaCameraBufferQueue");
    if (!queue->connectListener()) {
        ALOGE("Failed to connect buffer queue listener");
        goto fail;
    }

    camera->m_queue = queue;

#if ANDROID_MAJOR >= 9
    recording_queue = new DroidMediaBufferQueue("DroidMediaCameraBufferRecordingQueue", false);
    if (!recording_queue->connectListener()) {
        ALOGE("Failed to connect video buffer queue listener");
    } else {
      camera->m_recording_queue = recording_queue;
    }
#endif

    // Set capture session callbacks
    camera->m_capture_session_state_callbacks.context = camera;
    camera->m_capture_session_state_callbacks.onReady = capture_session_on_ready;
    camera->m_capture_session_state_callbacks.onActive = capture_session_on_active;
    camera->m_capture_session_state_callbacks.onClosed = capture_session_on_closed;

    // Set capture callbacks
    camera->m_capture_callbacks.context = camera;
    camera->m_capture_callbacks.onCaptureStarted = capture_session_on_capture_started;
    camera->m_capture_callbacks.onCaptureProgressed = capture_session_on_capture_progressed;
    camera->m_capture_callbacks.onCaptureCompleted = capture_session_on_capture_completed;
    camera->m_capture_callbacks.onCaptureFailed = capture_session_on_capture_failed;
    camera->m_capture_callbacks.onCaptureSequenceCompleted = capture_session_on_capture_sequence_completed;
    camera->m_capture_callbacks.onCaptureSequenceAborted = capture_session_on_capture_sequence_abort;
    camera->m_capture_callbacks.onCaptureBufferLost = capture_session_on_capture_buffer_lost;

    // Still image reader
    camera->m_image_listener.context = camera;
    camera->m_image_listener.onImageAvailable = &still_image_available;

    return camera;

fail:
    if (camera->m_metadata) {
        ACameraMetadata_free(camera->m_metadata);
        camera->m_metadata = NULL;
    }

    if (camera->m_device != NULL) {
        status = ACameraDevice_close(camera->m_device);

        if (status != ACAMERA_OK) {
            ALOGE("Failed to close CameraDevice.");
        }
        camera->m_device = NULL;
    }

    if (camera->m_camera_id_list) {
        ACameraManager_deleteCameraIdList(camera->m_camera_id_list);
    }

    if (camera->m_manager) {
        ACameraManager_delete(camera->m_manager);
    }

    return NULL;
}

bool droid_media_camera_reconnect(DroidMediaCamera *camera)
{
    return false;
}

void droid_media_camera_disconnect(DroidMediaCamera *camera)
{
    ALOGI("disconnect");
    destroy_capture_session(camera);

    if (camera->m_image_reader) {
        AImageReader_delete(camera->m_image_reader);
        camera->m_image_reader = NULL;
    }

    if (camera->m_device != NULL) {
        ACameraDevice_close(camera->m_device);
        camera->m_device = NULL;
    }

    if (camera->m_metadata) {
        ACameraMetadata_free(camera->m_metadata);
        camera->m_metadata = NULL;
    }

    if (camera->m_camera_id_list) {
        ACameraManager_deleteCameraIdList(camera->m_camera_id_list);
        camera->m_camera_id_list = NULL;
    }

    camera->m_queue->setCallbacks(0, 0);

#if ANDROID_MAJOR >= 9
    if (camera->m_recording_queue.get()) {
        camera->m_recording_queue->setCallbacks(0, 0);
    }
#endif

    if (camera->m_manager) {
        ACameraManager_delete(camera->m_manager);
        camera->m_manager = NULL;
    }

    delete camera;
}

bool droid_media_camera_lock(DroidMediaCamera *camera)
{
    // TODO Is camera lock needed?
    return true;
}

bool droid_media_camera_unlock(DroidMediaCamera *camera)
{
    // TODO Is camera unlock needed?
    return true;
}

bool droid_media_camera_start_preview(DroidMediaCamera *camera)
{
    ALOGI("start_preview");
    if (camera->m_preview_enabled) {
        return true;
    }

    if (!setup_capture_session(camera)) {
        ALOGE("Failed to setup capture session");
        return false;
    }

    camera->m_preview_enabled = true;
    if (!restart_enabled_streams(camera)) {
        ALOGE("start_preview failed");
        camera->m_preview_enabled = false;
        return false;
    }

    ALOGD("start_preview success");

    return true;
}

void droid_media_camera_stop_preview(DroidMediaCamera *camera)
{
    ALOGI("stop_preview");
    if (camera->m_session) {
        ALOGD("Stopping preview");
        camera->m_preview_enabled = false;
        ACameraCaptureSession_stopRepeating(camera->m_session);
    }

    destroy_capture_session(camera);
}

bool droid_media_camera_is_preview_enabled(DroidMediaCamera *camera)
{
    ALOGD("is_preview_enabled");
    return camera->m_preview_enabled;
}

bool droid_media_camera_start_recording(DroidMediaCamera *camera)
{
    ALOGI("start_recording");
    if (camera->m_video_recording_enabled) {
        return true;
    }

    if (!camera->m_session) {
        ALOGE("start_recording failed, no active session");
        return false;
    }
    if (!camera->m_video_request) {
        ALOGE("start_recording failed, no request");
        return false;
    }

    camera->m_video_recording_enabled = true;
    if (!restart_enabled_streams(camera)) {
        ALOGE("start_recording failed");
        camera->m_video_recording_enabled = false;
        return false;
    }

    return true;
}

void droid_media_camera_stop_recording(DroidMediaCamera *camera)
{
    ALOGI("stop_recording");
    if (!camera->m_session) {
        ALOGE("stop_recording failed, no active session");
        camera->m_video_recording_enabled = false;
        return;
    }

    if (!camera->m_video_request) {
        ALOGE("stop_recording failed, no request");
        camera->m_video_recording_enabled = false;
        return;
    }
    ACameraCaptureSession_stopRepeating(camera->m_session);

    camera->m_video_recording_enabled = false;

    restart_enabled_streams(camera);
}

bool droid_media_camera_is_recording_enabled(DroidMediaCamera *camera)
{
    ALOGD("is_recording_enabled");
    return camera->m_video_recording_enabled;
}

bool droid_media_camera_start_auto_focus(DroidMediaCamera *camera)
{
    ALOGI("start_auto_focus");
    return start_precapture_trigger(camera);
}

bool droid_media_camera_cancel_auto_focus(DroidMediaCamera *camera)
{
    ALOGI("cancel_auto_focus");
    return cancel_precapture_trigger(camera);
}

void droid_media_camera_set_callbacks(DroidMediaCamera *camera, DroidMediaCameraCallbacks *cb, void *data)
{
    ALOGD("set_callbacks");
    memcpy(&camera->m_cb, cb, sizeof(camera->m_cb));
    camera->m_cb_data = data;
}

bool droid_media_camera_send_command(DroidMediaCamera *camera, int32_t cmd, int32_t arg1, int32_t arg2)
{
    ALOGD("send_command");
    // TODO Is send command needed?
    return false;
}

bool droid_media_camera_store_meta_data_in_buffers(DroidMediaCamera *camera, bool enabled)
{
    (void)camera;
    if (!enabled) {
        ALOGW("camera2 does not support callback YUV recording frames");
        return false;
    }
    return true;
}

void droid_media_camera_set_preview_callback_flags(DroidMediaCamera *camera, int preview_callback_flag)
{
    camera->m_preview_callback_flag = preview_callback_flag;
    if (preview_callback_flag) {
        ALOGW("Preview callback flags are ignored with camera2 backend");
    }
}

int wb_mode_string_to_enum(const char *wb_mode)
{
    return
        !wb_mode ?
            ACAMERA_CONTROL_AWB_MODE_AUTO :
        !strcmp(wb_mode, android::CameraParameters::WHITE_BALANCE_AUTO) ?
            ACAMERA_CONTROL_AWB_MODE_AUTO :
        !strcmp(wb_mode, android::CameraParameters::WHITE_BALANCE_INCANDESCENT) ?
            ACAMERA_CONTROL_AWB_MODE_INCANDESCENT :
        !strcmp(wb_mode, android::CameraParameters::WHITE_BALANCE_FLUORESCENT) ?
            ACAMERA_CONTROL_AWB_MODE_FLUORESCENT :
        !strcmp(wb_mode, android::CameraParameters::WHITE_BALANCE_WARM_FLUORESCENT) ?
            ACAMERA_CONTROL_AWB_MODE_WARM_FLUORESCENT :
        !strcmp(wb_mode, android::CameraParameters::WHITE_BALANCE_DAYLIGHT) ?
            ACAMERA_CONTROL_AWB_MODE_DAYLIGHT :
        !strcmp(wb_mode, android::CameraParameters::WHITE_BALANCE_CLOUDY_DAYLIGHT) ?
            ACAMERA_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT :
        !strcmp(wb_mode, android::CameraParameters::WHITE_BALANCE_TWILIGHT) ?
            ACAMERA_CONTROL_AWB_MODE_TWILIGHT :
        !strcmp(wb_mode, android::CameraParameters::WHITE_BALANCE_SHADE) ?
            ACAMERA_CONTROL_AWB_MODE_SHADE :
        -1;
}

const char *wb_mode_enum_to_string(uint8_t wb_mode, bool &found)
{
    found = true;
    switch (wb_mode) {
        case ACAMERA_CONTROL_AWB_MODE_OFF:
            found = false;
            return "";
        case ACAMERA_CONTROL_AWB_MODE_AUTO:
            return android::CameraParameters::WHITE_BALANCE_AUTO;
        case ACAMERA_CONTROL_AWB_MODE_INCANDESCENT:
            return android::CameraParameters::WHITE_BALANCE_INCANDESCENT;
        case ACAMERA_CONTROL_AWB_MODE_FLUORESCENT:
            return android::CameraParameters::WHITE_BALANCE_FLUORESCENT;
        case ACAMERA_CONTROL_AWB_MODE_WARM_FLUORESCENT:
            return android::CameraParameters::WHITE_BALANCE_WARM_FLUORESCENT;
        case ACAMERA_CONTROL_AWB_MODE_DAYLIGHT:
            return android::CameraParameters::WHITE_BALANCE_DAYLIGHT;
        case ACAMERA_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT:
            return android::CameraParameters::WHITE_BALANCE_CLOUDY_DAYLIGHT;
        case ACAMERA_CONTROL_AWB_MODE_TWILIGHT:
            return android::CameraParameters::WHITE_BALANCE_TWILIGHT;
        case ACAMERA_CONTROL_AWB_MODE_SHADE:
            return android::CameraParameters::WHITE_BALANCE_SHADE;
        default:
            found = false;
            ALOGE("%s: Unknown AWB mode enum: %d",
                    __FUNCTION__, wb_mode);
            return "";
    }
}

int effect_mode_string_to_enum(const char *effect_mode)
{
    return
        !effect_mode ?
            ACAMERA_CONTROL_EFFECT_MODE_OFF :
        !strcmp(effect_mode, android::CameraParameters::EFFECT_NONE) ?
            ACAMERA_CONTROL_EFFECT_MODE_OFF :
        !strcmp(effect_mode, android::CameraParameters::EFFECT_MONO) ?
            ACAMERA_CONTROL_EFFECT_MODE_MONO :
        !strcmp(effect_mode, android::CameraParameters::EFFECT_NEGATIVE) ?
            ACAMERA_CONTROL_EFFECT_MODE_NEGATIVE :
        !strcmp(effect_mode, android::CameraParameters::EFFECT_SOLARIZE) ?
            ACAMERA_CONTROL_EFFECT_MODE_SOLARIZE :
        !strcmp(effect_mode, android::CameraParameters::EFFECT_SEPIA) ?
            ACAMERA_CONTROL_EFFECT_MODE_SEPIA :
        !strcmp(effect_mode, android::CameraParameters::EFFECT_POSTERIZE) ?
            ACAMERA_CONTROL_EFFECT_MODE_POSTERIZE :
        !strcmp(effect_mode, android::CameraParameters::EFFECT_WHITEBOARD) ?
            ACAMERA_CONTROL_EFFECT_MODE_WHITEBOARD :
        !strcmp(effect_mode, android::CameraParameters::EFFECT_BLACKBOARD) ?
            ACAMERA_CONTROL_EFFECT_MODE_BLACKBOARD :
        !strcmp(effect_mode, android::CameraParameters::EFFECT_AQUA) ?
            ACAMERA_CONTROL_EFFECT_MODE_AQUA :
        -1;
}

const char *effect_mode_enum_to_string(uint8_t effect_mode, bool &found)
{
    found = true;
    switch (effect_mode) {
        case ACAMERA_CONTROL_EFFECT_MODE_OFF:
            return android::CameraParameters::EFFECT_NONE;
        case ACAMERA_CONTROL_EFFECT_MODE_MONO:
            return android::CameraParameters::EFFECT_MONO;
        case ACAMERA_CONTROL_EFFECT_MODE_NEGATIVE:
            return android::CameraParameters::EFFECT_NEGATIVE;
        case ACAMERA_CONTROL_EFFECT_MODE_SOLARIZE:
            return android::CameraParameters::EFFECT_SOLARIZE;
        case ACAMERA_CONTROL_EFFECT_MODE_SEPIA:
            return android::CameraParameters::EFFECT_SEPIA;
        case ACAMERA_CONTROL_EFFECT_MODE_POSTERIZE:
            return android::CameraParameters::EFFECT_POSTERIZE;
        case ACAMERA_CONTROL_EFFECT_MODE_WHITEBOARD:
            return android::CameraParameters::EFFECT_WHITEBOARD;
        case ACAMERA_CONTROL_EFFECT_MODE_BLACKBOARD:
            return android::CameraParameters::EFFECT_BLACKBOARD;
        case ACAMERA_CONTROL_EFFECT_MODE_AQUA:
            return android::CameraParameters::EFFECT_AQUA;
        default:
            found = false;
            ALOGE("%s: Unknown effect mode enum: %d",
                    __FUNCTION__, effect_mode);
            return "";
    }
}

int ab_mode_string_to_enum(const char *ab_mode)
{
    return
        !ab_mode ?
            ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO :
        !strcmp(ab_mode, android::CameraParameters::ANTIBANDING_AUTO) ?
            ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO :
        !strcmp(ab_mode, android::CameraParameters::ANTIBANDING_OFF) ?
            ACAMERA_CONTROL_AE_ANTIBANDING_MODE_OFF :
        !strcmp(ab_mode, android::CameraParameters::ANTIBANDING_50HZ) ?
            ACAMERA_CONTROL_AE_ANTIBANDING_MODE_50HZ :
        !strcmp(ab_mode, android::CameraParameters::ANTIBANDING_60HZ) ?
            ACAMERA_CONTROL_AE_ANTIBANDING_MODE_60HZ :
        -1;
}

const char *ab_mode_enum_to_string(uint8_t ab_mode, bool &found)
{
    found = true;
    switch (ab_mode) {
        case ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO:
            return android::CameraParameters::ANTIBANDING_AUTO;
        case ACAMERA_CONTROL_AE_ANTIBANDING_MODE_OFF:
            return android::CameraParameters::ANTIBANDING_OFF;
        case ACAMERA_CONTROL_AE_ANTIBANDING_MODE_50HZ:
            return android::CameraParameters::ANTIBANDING_50HZ;
        case ACAMERA_CONTROL_AE_ANTIBANDING_MODE_60HZ:
            return android::CameraParameters::ANTIBANDING_60HZ;
        default:
            found = false;
            ALOGE("%s: Unknown antibanding mode enum: %d",
                    __FUNCTION__, ab_mode);
            return "";
    }
}

int scene_mode_string_to_enum(const char *scene_mode, uint8_t default_scene_mode)
{
    return
        !scene_mode ?
            default_scene_mode :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_AUTO) ?
            default_scene_mode :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_ACTION) ?
            ACAMERA_CONTROL_SCENE_MODE_ACTION :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_PORTRAIT) ?
            ACAMERA_CONTROL_SCENE_MODE_PORTRAIT :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_LANDSCAPE) ?
            ACAMERA_CONTROL_SCENE_MODE_LANDSCAPE :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_NIGHT) ?
            ACAMERA_CONTROL_SCENE_MODE_NIGHT :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_NIGHT_PORTRAIT) ?
            ACAMERA_CONTROL_SCENE_MODE_NIGHT_PORTRAIT :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_THEATRE) ?
            ACAMERA_CONTROL_SCENE_MODE_THEATRE :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_BEACH) ?
            ACAMERA_CONTROL_SCENE_MODE_BEACH :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_SNOW) ?
            ACAMERA_CONTROL_SCENE_MODE_SNOW :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_SUNSET) ?
            ACAMERA_CONTROL_SCENE_MODE_SUNSET :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_STEADYPHOTO) ?
            ACAMERA_CONTROL_SCENE_MODE_STEADYPHOTO :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_FIREWORKS) ?
            ACAMERA_CONTROL_SCENE_MODE_FIREWORKS :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_SPORTS) ?
            ACAMERA_CONTROL_SCENE_MODE_SPORTS :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_PARTY) ?
            ACAMERA_CONTROL_SCENE_MODE_PARTY :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_CANDLELIGHT) ?
            ACAMERA_CONTROL_SCENE_MODE_CANDLELIGHT :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_BARCODE) ?
            ACAMERA_CONTROL_SCENE_MODE_BARCODE :
        !strcmp(scene_mode, android::CameraParameters::SCENE_MODE_HDR) ?
            ACAMERA_CONTROL_SCENE_MODE_HDR :
        -1;
}

const char *scene_mode_enum_to_string(uint8_t scene_mode, bool &found)
{
    found = true;
    switch (scene_mode) {
        case ACAMERA_CONTROL_SCENE_MODE_DISABLED:
        case ACAMERA_CONTROL_SCENE_MODE_FACE_PRIORITY:
            return android::CameraParameters::SCENE_MODE_AUTO;
        case ACAMERA_CONTROL_SCENE_MODE_ACTION:
            return android::CameraParameters::SCENE_MODE_ACTION;
        case ACAMERA_CONTROL_SCENE_MODE_PORTRAIT:
            return android::CameraParameters::SCENE_MODE_PORTRAIT;
        case ACAMERA_CONTROL_SCENE_MODE_LANDSCAPE:
            return android::CameraParameters::SCENE_MODE_LANDSCAPE;
        case ACAMERA_CONTROL_SCENE_MODE_NIGHT:
            return android::CameraParameters::SCENE_MODE_NIGHT;
        case ACAMERA_CONTROL_SCENE_MODE_NIGHT_PORTRAIT:
            return android::CameraParameters::SCENE_MODE_NIGHT_PORTRAIT;
        case ACAMERA_CONTROL_SCENE_MODE_THEATRE:
            return android::CameraParameters::SCENE_MODE_THEATRE;
        case ACAMERA_CONTROL_SCENE_MODE_BEACH:
            return android::CameraParameters::SCENE_MODE_BEACH;
        case ACAMERA_CONTROL_SCENE_MODE_SNOW:
            return android::CameraParameters::SCENE_MODE_SNOW;
        case ACAMERA_CONTROL_SCENE_MODE_SUNSET:
            return android::CameraParameters::SCENE_MODE_SUNSET;
        case ACAMERA_CONTROL_SCENE_MODE_STEADYPHOTO:
            return android::CameraParameters::SCENE_MODE_STEADYPHOTO;
        case ACAMERA_CONTROL_SCENE_MODE_FIREWORKS:
            return android::CameraParameters::SCENE_MODE_FIREWORKS;
        case ACAMERA_CONTROL_SCENE_MODE_SPORTS:
            return android::CameraParameters::SCENE_MODE_SPORTS;
        case ACAMERA_CONTROL_SCENE_MODE_PARTY:
            return android::CameraParameters::SCENE_MODE_PARTY;
        case ACAMERA_CONTROL_SCENE_MODE_CANDLELIGHT:
            return android::CameraParameters::SCENE_MODE_CANDLELIGHT;
        case ACAMERA_CONTROL_SCENE_MODE_BARCODE:
            return android::CameraParameters::SCENE_MODE_BARCODE;
        case ACAMERA_CONTROL_SCENE_MODE_HDR:
            return android::CameraParameters::SCENE_MODE_HDR;
        default:
            found = false;
            ALOGE("%s: Unknown scene mode enum: %d",
                    __FUNCTION__, scene_mode);
            return "";
    }
}

int flash_mode_string_to_ae_mode_enum(const char *flash_mode)
{
    return
        !flash_mode ?
            ACAMERA_CONTROL_AE_MODE_ON :
        !strcmp(flash_mode, android::CameraParameters::FLASH_MODE_OFF) ?
            ACAMERA_CONTROL_AE_MODE_ON :
        !strcmp(flash_mode, android::CameraParameters::FLASH_MODE_AUTO) ?
            ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH :
        !strcmp(flash_mode, android::CameraParameters::FLASH_MODE_ON) ?
            ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH :
        !strcmp(flash_mode, android::CameraParameters::FLASH_MODE_RED_EYE) ?
            ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE :
        !strcmp(flash_mode, android::CameraParameters::FLASH_MODE_TORCH) ?
            ACAMERA_CONTROL_AE_MODE_ON :
        -1;
}

int focus_mode_string_to_enum(const char *focus_mode)
{
    return
        !focus_mode ?
            ACAMERA_CONTROL_AF_MODE_OFF :
        !strcmp(focus_mode, android::CameraParameters::FOCUS_MODE_AUTO) ?
            ACAMERA_CONTROL_AF_MODE_AUTO :
        !strcmp(focus_mode, android::CameraParameters::FOCUS_MODE_INFINITY) ?
            ACAMERA_CONTROL_AF_MODE_OFF :
        !strcmp(focus_mode, android::CameraParameters::FOCUS_MODE_MACRO) ?
            ACAMERA_CONTROL_AF_MODE_MACRO :
        !strcmp(focus_mode, android::CameraParameters::FOCUS_MODE_FIXED) ?
            ACAMERA_CONTROL_AF_MODE_OFF :
        !strcmp(focus_mode, android::CameraParameters::FOCUS_MODE_EDOF) ?
            ACAMERA_CONTROL_AF_MODE_EDOF :
        !strcmp(focus_mode, android::CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO) ?
            ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO :
        !strcmp(focus_mode, android::CameraParameters::FOCUS_MODE_CONTINUOUS_PICTURE) ?
            ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE :
        -1;
}

const char *focus_mode_enum_to_string(uint8_t focus_mode, bool fixed_lens, bool &found)
{
    found = true;
    switch (focus_mode) {
        case ACAMERA_CONTROL_AF_MODE_AUTO:
            return android::CameraParameters::FOCUS_MODE_AUTO;
        case ACAMERA_CONTROL_AF_MODE_MACRO:
            return android::CameraParameters::FOCUS_MODE_MACRO;
        case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
            return android::CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO;
        case ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
            return android::CameraParameters::FOCUS_MODE_CONTINUOUS_PICTURE;
        case ACAMERA_CONTROL_AF_MODE_EDOF:
            return android::CameraParameters::FOCUS_MODE_EDOF;
        case ANDROID_CONTROL_AF_MODE_OFF:
            if (fixed_lens) {
                return android::CameraParameters::FOCUS_MODE_FIXED;
            } else {
                return android::CameraParameters::FOCUS_MODE_INFINITY;
            }
        default:
            ALOGE("%s: Unknown focus mode enum: %d",
                    __FUNCTION__, focus_mode);
            found = false;
            return "";
    }
}

int param_key_string_to_enum(const char *key)
{
    return
        !strcmp(key, android::CameraParameters::KEY_PREVIEW_FPS_RANGE) ?
            ACAMERA_CONTROL_AE_TARGET_FPS_RANGE :
        !strcmp(key, android::CameraParameters::KEY_JPEG_QUALITY) ?
            ACAMERA_JPEG_QUALITY :
        !strcmp(key, android::CameraParameters::KEY_WHITE_BALANCE) ?
            ACAMERA_CONTROL_AWB_MODE :
        !strcmp(key, android::CameraParameters::KEY_EFFECT) ?
            ACAMERA_CONTROL_EFFECT_MODE :
        !strcmp(key, android::CameraParameters::KEY_ANTIBANDING) ?
            ACAMERA_CONTROL_AE_ANTIBANDING_MODE :
        !strcmp(key, android::CameraParameters::KEY_SCENE_MODE) ?
            ACAMERA_CONTROL_SCENE_MODE :
        !strcmp(key, android::CameraParameters::KEY_FLASH_MODE) ?
            ACAMERA_FLASH_MODE :
        !strcmp(key, android::CameraParameters::KEY_FOCUS_MODE) ?
            ACAMERA_CONTROL_AF_MODE :
        !strcmp(key, android::CameraParameters::KEY_FOCUS_AREAS) ?
            ACAMERA_CONTROL_AF_REGIONS :
        !strcmp(key, android::CameraParameters::KEY_EXPOSURE_COMPENSATION) ?
            ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION :
        !strcmp(key, android::CameraParameters::KEY_AUTO_EXPOSURE_LOCK) ?
            ACAMERA_CONTROL_AE_LOCK :
        !strcmp(key, android::CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK) ?
            ACAMERA_CONTROL_AWB_LOCK :
        !strcmp(key, android::CameraParameters::KEY_METERING_AREAS) ?
            ACAMERA_CONTROL_AE_REGIONS :
#if ANDROID_MAJOR >= 11
        !strcmp(key, android::CameraParameters::KEY_ZOOM) ?
            ACAMERA_CONTROL_ZOOM_RATIO :
#else
        !strcmp(key, android::CameraParameters::KEY_ZOOM) ?
            ACAMERA_SCALER_CROP_REGION :
#endif
        !strcmp(key, android::CameraParameters::KEY_VIDEO_STABILIZATION) ?
            ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE :
        -1;
}

bool parse_areas(std::string &str, std::vector<MeteringArea> *areas)
{
    static const size_t NUM_FIELDS = 5;
    areas->clear();
    if (str.empty()) {
        // If no key exists, use default (0,0,0,0,0)
        areas->emplace_back(MeteringArea{0, 0, 0, 0, 0});
        return true;
    }
    ssize_t start = str.find('(', 0) + 1;
    while (start != 0) {
        const char *area = str.c_str() + start;
        char *num_end;
        int values[NUM_FIELDS];
        for (size_t i = 0; i < NUM_FIELDS; i++) {
            errno = 0;
            values[i] = strtol(area, &num_end, 10);
            if (errno || num_end == area) {
                return false;
            }
            area = num_end + 1;
        }
        areas->emplace_back(MeteringArea{values[0], values[1], values[2], values[3], values[4]});
        start = str.find('(', start) + 1;
    }
    return true;
}


static bool camera_has_flash(DroidMediaCamera *camera)
{
    if (!camera || !camera->m_metadata) {
        return false;
    }

    ACameraMetadata_const_entry entry;
    camera_status_t status = ACameraMetadata_getConstEntry(camera->m_metadata,
        ACAMERA_FLASH_INFO_AVAILABLE, &entry);
    return status == ACAMERA_OK && entry.count > 0 &&
        entry.data.u8[0] != ACAMERA_FLASH_INFO_AVAILABLE_FALSE;
}

void parse_pair_string(std::string &str, char delim, std::string &first, std::string &second)
{
    std::istringstream iss(str);
    std::getline(iss, first, delim);
    std::getline(iss, second, delim);
}

bool parse_int32_value(const std::string &str, int32_t &out)
{
    errno = 0;
    char *end = nullptr;
    long value = strtol(str.c_str(), &end, 10);

    if (errno != 0 || end == str.c_str() || *end != '\0' ||
            value < std::numeric_limits<int32_t>::min() ||
            value > std::numeric_limits<int32_t>::max()) {
        return false;
    }

    out = static_cast<int32_t>(value);
    return true;
}

bool parse_float_value(const std::string &str, float &out)
{
    errno = 0;
    char *end = nullptr;
    float value = strtof(str.c_str(), &end);

    if (errno != 0 || end == str.c_str() || *end != '\0') {
        return false;
    }

    out = value;
    return true;
}

bool parse_pair_int32(std::string &str, char delim, int32_t &first, int32_t &second)
{
    std::string first_s;
    std::string second_s;
    parse_pair_string(str, delim, first_s, second_s);

    int32_t parsed_first = 0;
    int32_t parsed_second = 0;
    if (!parse_int32_value(first_s, parsed_first) ||
            !parse_int32_value(second_s, parsed_second)) {
        return false;
    }

    first = parsed_first;
    second = parsed_second;
    return true;
}

bool set_zoom_crop_region(DroidMediaCamera *camera, ACaptureRequest *request, const std::string &value_s,
                          const int32_t *active_array_size)
{
    if (!camera || !camera->m_metadata || !request) {
        ALOGW("Unable to apply zoom crop, camera/request not ready");
        return false;
    }

    float zoom_ratio = 0.0f;
    if (!parse_float_value(value_s, zoom_ratio)) {
        ALOGW("Ignoring invalid zoom value: %s", value_s.c_str());
        return false;
    }

    if (zoom_ratio < 1.0f) {
        zoom_ratio = 1.0f;
    }

    float max_zoom = 1.0f;
    ACameraMetadata_const_entry zoom_entry;
    camera_status_t status = ACameraMetadata_getConstEntry(camera->m_metadata,
        ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &zoom_entry);
    if (status == ACAMERA_OK && zoom_entry.count > 0 && zoom_entry.data.f[0] > 1.0f) {
        max_zoom = zoom_entry.data.f[0];
    }

    if (zoom_ratio > max_zoom) {
        zoom_ratio = max_zoom;
    }

    int32_t left = active_array_size[0];
    int32_t top = active_array_size[1];
    int32_t width = active_array_size[2];
    int32_t height = active_array_size[3];

    int32_t crop_width = static_cast<int32_t>(width / zoom_ratio);
    int32_t crop_height = static_cast<int32_t>(height / zoom_ratio);
    if (crop_width <= 0 || crop_height <= 0) {
        ALOGW("Invalid crop size computed for zoom");
        return false;
    }

    int32_t crop_left = left + (width - crop_width) / 2;
    int32_t crop_top = top + (height - crop_height) / 2;
    int32_t crop_region[4] = {
        crop_left,
        crop_top,
        crop_width,
        crop_height
    };

    status = ACaptureRequest_setEntry_i32(request,
        ACAMERA_SCALER_CROP_REGION, 4, crop_region);
    if (status != ACAMERA_OK) {
        ALOGW("Setting crop region for zoom failed");
        return false;
    }

    return true;
}

void convert_to_sensor_coordinates(int32_t *out, MeteringArea &area, const int32_t *active_array_size)
{
    int width = active_array_size[2];
    int height = active_array_size[3];
    out[0] = (area.xmin + 1000) * (width - 1) / 2000;
    out[1] = (area.ymin + 1000) * (height - 1) / 2000;
    out[2] = (area.xmax + 1000) * (width - 1) / 2000;
    out[3] = (area.ymax + 1000) * (height - 1) / 2000;
    out[4] = area.weight;
}

static void update_request(DroidMediaCamera *camera, ACaptureRequest *request, std::unordered_map<std::string, std::string> &param_map) {
    ALOGD("update_request");
    uint8_t controlMode = ACAMERA_CONTROL_MODE_AUTO;
    ACaptureRequest_setEntry_u8(request,
        ACAMERA_CONTROL_MODE, 1, &controlMode);

    const int32_t *active_array_size = NULL;
    ACameraMetadata_const_entry active_array_entry;
    camera_status_t status = ACameraMetadata_getConstEntry(camera->m_metadata,
        ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &active_array_entry);
    if (status == ACAMERA_OK && active_array_entry.count == 4) {
        active_array_size = active_array_entry.data.i32;
        if (active_array_size[2] <= 0 || active_array_size[3] <= 0) {
            ALOGW("Invalid active array size");
            active_array_size = NULL;
        }
    } else {
        ALOGW("Unable to read active array size");
    }


    // TODO check if something is missing
    for (auto& it: param_map) {
        std::string key_s = it.first;
        std::string value_s = it.second;
        ALOGV("update_request parameters %s=%s", key_s.c_str(), value_s.c_str());
        int32_t key;
        if ((key = param_key_string_to_enum(key_s.c_str())) >= 0) {
            switch (key) {
            case ACAMERA_CONTROL_AE_ANTIBANDING_MODE: {
                int mode_i = ab_mode_string_to_enum(value_s.c_str());
                if (mode_i >= 0) {
                    uint8_t mode = static_cast<uint8_t>(mode_i);
                    ACaptureRequest_setEntry_u8(request, key, 1, &mode);
                }
                break;
            }
            case ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION:
            {
                int32_t value = 0;
                if (parse_int32_value(value_s, value)) {
                    ACaptureRequest_setEntry_i32(request, key, 1, &value);
                } else {
                    ALOGW("Ignoring invalid exposure compensation: %s", value_s.c_str());
                }
                break;
            }
            case ACAMERA_CONTROL_AE_LOCK: {
                uint8_t value = ACAMERA_CONTROL_AE_LOCK_OFF;
                if (!strcmp(value_s.c_str(), android::CameraParameters::TRUE)) {
                    value = ACAMERA_CONTROL_AE_LOCK_ON;
                }
                ACaptureRequest_setEntry_u8(request, key, 1, &value);
                break;
            }
            case ACAMERA_CONTROL_AE_REGIONS: {
                if (active_array_size) {
                    std::vector<MeteringArea> areas;
                    if (parse_areas(value_s, &areas)) {
                        int32_t *values = new int32_t[areas.size() * 5];
                        for (int i = 0; i < areas.size(); i++) {
                            convert_to_sensor_coordinates(values + i * 5, areas[i], active_array_size);
                        }
                        ACaptureRequest_setEntry_i32(request, key, areas.size() * 5, values);
                        ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AWB_REGIONS, areas.size() * 5, values);
                        delete[] values;
                    }
                } else {
                    ALOGW("Cannot set metering areas without active array size");
                }
                break;
            }
            case ACAMERA_CONTROL_AE_TARGET_FPS_RANGE: {
                int32_t values[2];
                if (parse_pair_int32(value_s, ',', values[0], values[1])) {
                    values[0] /= 1000;
                    values[1] /= 1000;
                    ACaptureRequest_setEntry_i32(request, key, 2, values);
                } else {
                    ALOGW("Ignoring invalid fps range: %s", value_s.c_str());
                }
                break;
            }
            case ACAMERA_CONTROL_AF_MODE: {
                int mode_i = focus_mode_string_to_enum(value_s.c_str());
                if (mode_i >= 0) {
                    uint8_t mode = static_cast<uint8_t>(mode_i);
                    ACaptureRequest_setEntry_u8(request, key, 1, &mode);
                }
                break;
            }
            case ACAMERA_CONTROL_AF_REGIONS: {
                if (active_array_size) {
                    std::vector<MeteringArea> areas;
                    if (parse_areas(value_s, &areas)) {
                        int32_t *values = new int32_t[areas.size() * 5];
                        for (int i = 0; i < areas.size(); i ++) {
                            convert_to_sensor_coordinates(values + i * 5, areas[i], active_array_size);
                        }
                        ACaptureRequest_setEntry_i32(request, key, areas.size() * 5, values);
                        delete[] values;
                    }
                } else {
                    ALOGW("Cannot set AF areas without active array size");
                }
                break;
            }
            case ACAMERA_CONTROL_AWB_LOCK: {
                uint8_t value = ACAMERA_CONTROL_AWB_LOCK_OFF;
                if (!strcmp(value_s.c_str(), android::CameraParameters::TRUE)) {
                    value = ACAMERA_CONTROL_AWB_LOCK_ON;
                }
                ACaptureRequest_setEntry_u8(request, key, 1, &value);
                break;
            }
            case ACAMERA_CONTROL_AWB_MODE: {
                int mode_i = wb_mode_string_to_enum(value_s.c_str());
                if (mode_i >= 0) {
                    uint8_t mode = static_cast<uint8_t>(mode_i);
                    ACaptureRequest_setEntry_u8(request, key, 1, &mode);
                }
                break;
            }
            case ACAMERA_CONTROL_EFFECT_MODE: {
                int mode_i = effect_mode_string_to_enum(value_s.c_str());
                if (mode_i >= 0) {
                    uint8_t mode = static_cast<uint8_t>(mode_i);
                    ACaptureRequest_setEntry_u8(request, key, 1, &mode);
                }
                break;
            }
            case ACAMERA_CONTROL_SCENE_MODE: {
                int mode_i = scene_mode_string_to_enum(value_s.c_str(), ACAMERA_CONTROL_SCENE_MODE_DISABLED);
                if (mode_i >= 0) {
                    uint8_t mode = static_cast<uint8_t>(mode_i);
                    ACaptureRequest_setEntry_u8(request, key, 1, &mode);
                }
                break;
            }
            case ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE: {
               uint8_t value = ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
                if (camera->m_video_recording_enabled) {
                    if (!strcmp(value_s.c_str(), android::CameraParameters::TRUE)) {
                        if (request == camera->m_preview_request ||
                            request == camera->m_video_request ||
                            request == camera->m_ext_video_request) {
                            value = ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON;
                        }
                    }
                }
                ACaptureRequest_setEntry_u8(request, key, 1, &value);
                break;
            }
#if ANDROID_MAJOR >= 11
            case ACAMERA_CONTROL_ZOOM_RATIO:
            {
                float value = 0.0f;
                if (parse_float_value(value_s, value)) {
                    if (value <= 0.0f) {
                        value = 1.0f;
                    }
                    ACaptureRequest_setEntry_float(request, key, 1, &value);
                } else {
                    ALOGW("Ignoring invalid zoom ratio: %s", value_s.c_str());
                }
                break;
            }
#else
            case ACAMERA_SCALER_CROP_REGION:
                if (active_array_size) {
                    set_zoom_crop_region(camera, request, value_s, active_array_size);
                } else {
                    ALOGW("Cannot set zoom without active array size");
                }
                break;
#endif
            case ACAMERA_FLASH_MODE: {
                camera_status_t status;
                if (!camera_has_flash(camera) &&
                        strcmp(value_s.c_str(), android::CameraParameters::FLASH_MODE_OFF)) {
                    ALOGW("Ignoring flash mode on device without flash: %s", value_s.c_str());
                    value_s = android::CameraParameters::FLASH_MODE_OFF;
                }

                int ae_mode_i = flash_mode_string_to_ae_mode_enum(value_s.c_str());
                if (ae_mode_i >= 0) {
                    uint8_t ae_mode = static_cast<uint8_t>(ae_mode_i);
                    status = ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_MODE, 1, &ae_mode);
                    if (status != ACAMERA_OK) {
                        ALOGW("Failed to apply AE mode for flash");
                    }

                    // Set flash mode when AE doesn't override it.
                    if (ae_mode == ACAMERA_CONTROL_AE_MODE_OFF || ae_mode == ACAMERA_CONTROL_AE_MODE_ON) {
                        bool torch = !strcmp(value_s.c_str(), android::CameraParameters::FLASH_MODE_TORCH);
                        uint8_t flash_mode = torch ? ACAMERA_FLASH_MODE_TORCH : ACAMERA_FLASH_MODE_OFF;
                        status = ACaptureRequest_setEntry_u8(request, ACAMERA_FLASH_MODE, 1, &flash_mode);
                        if (status != ACAMERA_OK) {
                            ALOGW("Failed to apply flash mode");
                        }
                    }
                } else {
                    ALOGW("Ignoring invalid flash mode: %s", value_s.c_str());
                }
                break;
            }
            case ACAMERA_JPEG_QUALITY: {
                int32_t value = 0;
                if (parse_int32_value(value_s, value) && value >= 1 && value <= 100) {
                    uint8_t quality = static_cast<uint8_t>(value);
                    ACaptureRequest_setEntry_u8(request, key, 1, &quality);
                } else {
                    ALOGW("Ignoring invalid jpeg quality: %s", value_s.c_str());
                }
                break;
            }
            default:
                break;
            }
        }
    }

}

bool droid_media_camera_set_parameters(DroidMediaCamera *camera, const char *params)
{
    ALOGD("set_parameters");
    if (!camera->m_device || !params) {
        ALOGE("set_parameters failed");
        return false;
    }

    int32_t current_height = -1;
    int32_t current_width = -1;
    int32_t current_format = camera->image_format;

    if (camera->m_image_reader) {
        AImageReader_getHeight(camera->m_image_reader, &current_height);
        AImageReader_getWidth(camera->m_image_reader, &current_width);
    }

    std::istringstream iss(params);
    std::string param;

    while (std::getline(iss, param, ';')) {
        std::string key_s;
        std::string value_s;
        parse_pair_string(param, '=', key_s, value_s);

        if (!strcmp(key_s.c_str(), android::CameraParameters::KEY_FLASH_MODE) &&
                !camera_has_flash(camera) &&
                strcmp(value_s.c_str(), android::CameraParameters::FLASH_MODE_OFF)) {
            ALOGW("Forcing flash mode off on device without flash: %s", value_s.c_str());
            value_s = android::CameraParameters::FLASH_MODE_OFF;
        }

        camera->m_param_map[key_s] = value_s;

        ALOGD("set_parameters %s=%s", key_s.c_str(), value_s.c_str());

        if (param_key_string_to_enum(key_s.c_str()) == -1) {
            if (!strcmp(key_s.c_str(), "picture-format")) {
                //TODO is this needed?
            } else if (!strcmp(key_s.c_str(), "picture-size")) {
                if (!parse_pair_int32(value_s, 'x', camera->image_width, camera->image_height)) {
                    ALOGW("Invalid picture-size value: %s", value_s.c_str());
                    return false;
                }
            } else if (!strcmp(key_s.c_str(), "preview-size")) {
                if (!parse_pair_int32(value_s, 'x', camera->preview_width, camera->preview_height)) {
                    ALOGW("Invalid preview-size value: %s", value_s.c_str());
                    return false;
                }
            } else if (!strcmp(key_s.c_str(), "video-size")) {
                if (!parse_pair_int32(value_s, 'x', camera->video_width, camera->video_height)) {
                    ALOGW("Invalid video-size value: %s", value_s.c_str());
                    return false;
                }
            }
        }
    }

    if (camera->image_width != -1 && camera->image_height != -1 &&
        (!camera->m_image_reader || current_width != camera->image_width ||
        current_height != camera->image_height || current_format != camera->image_format)) {
        ALOGI("Updating image reader size %ix%i format %i", camera->image_width, camera->image_height, camera->image_format);
        setup_image_reader(camera);
    }

    if (camera->m_image_request) {
        ALOGI("update_request image mode");
        update_request(camera, camera->m_image_request, camera->m_param_map);
    }

    if (camera->m_preview_request) {
        ALOGI("update_request preview");
        update_request(camera, camera->m_preview_request, camera->m_param_map);
    }

    if (camera->m_video_request) {
        ALOGI("update_request video mode");
        update_request(camera, camera->m_video_request, camera->m_param_map);
    }

    if (camera->m_ext_video_request) {
        ALOGI("update_request external video");
        update_request(camera, camera->m_ext_video_request, camera->m_param_map);
    }

    restart_enabled_streams(camera);

    return true;
}

char *droid_media_camera_get_parameters(DroidMediaCamera *camera)
{
    ALOGD("get_parameters");
    camera_status_t status;
    std::string params;
    int32_t numEntries;
    const uint32_t *tags;

    if (!camera->m_device || !camera->m_metadata) {
        return NULL;
    }

    status = ACameraMetadata_getAllTags(camera->m_metadata, &numEntries, &tags);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to all tags from camera (status: %d)\n", status);
        return NULL;
    }

    if (camera->image_height != -1 && camera->image_width != -1) {
        params += "picture-size="+std::to_string(camera->image_width)+"x"+std::to_string(camera->image_height)+";";
    }

    if (camera->preview_height != -1 && camera->preview_width != -1) {
        params += "preview-size="+std::to_string(camera->preview_width)+"x"+std::to_string(camera->preview_height)+";";
    }

    if (camera->video_height != -1 && camera->video_width != -1) {
        params += "video-size="+std::to_string(camera->video_width)+"x"+std::to_string(camera->video_height)+";";
    } else {
        params += "video-size=-1x-1;";
    }

    params += "video-frame-format=android-opaque;";
    std::unordered_map<std::string, std::string>::const_iterator flash_it =
        camera->m_param_map.find(android::CameraParameters::KEY_FLASH_MODE);
    if (flash_it != camera->m_param_map.end() && !flash_it->second.empty()) {
        params += std::string("flash-mode=") + flash_it->second + ";";
    }

    for (int32_t i = 0; i < numEntries; i++) {
        ACameraMetadata_const_entry entry;
        status = ACameraMetadata_getConstEntry(camera->m_metadata, tags[i], &entry);
        if (status != ACAMERA_OK) {
            ALOGI("Failed to get entry from camera (status: %d)\n", status);
            continue;
        }

        switch (tags[i]) {
        case ACAMERA_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES:
            if (entry.count > 0) {
                bool found = false;
                std::string ab = "antibanding-values=";
                for (int32_t j = 0; j < entry.count; j++) {
                    if (found)
                        ab += ",";
                    ab += ab_mode_enum_to_string(entry.data.u8[j], found);
                }
                params += ab + ";";
            }
            break;
        case ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES: {
            if (entry.count > 0) {
                std::string fps = "preview-fps-range-values=";
                for (int32_t j = 0; j < entry.count; j = j + 2) {
                    if (j > 0)
                        fps += ",";
                    fps += "(" + std::to_string(entry.data.i32[j] * 1000);
                    fps += ",";
                    fps += std::to_string(entry.data.i32[j+1] * 1000) + ")";
                }
//                params += fps + ";";
                params += "preview-fps-range-values=(15000,30000);";
                params += "preview-frame-rate=30;";
            }
            break;
        }
        case ACAMERA_CONTROL_AE_COMPENSATION_RANGE:
            params += "min-exposure-compensation="+std::to_string(entry.data.i32[0])+";";
            params += "max-exposure-compensation="+std::to_string(entry.data.i32[1])+";";
            break;
        case ACAMERA_CONTROL_AE_COMPENSATION_STEP: {
            // convert to string using no locale
            std::ostringstream oss;
            oss.imbue(std::locale::classic());
            oss << "exposure-compensation-step=" << ((float)entry.data.r[0].numerator / (float)entry.data.r[0].denominator) << ";";
            params += oss.str();
            break;
        }
        case ACAMERA_CONTROL_AE_LOCK_AVAILABLE:
            if (entry.count > 0 && entry.data.u8[0] == ACAMERA_CONTROL_AE_LOCK_AVAILABLE_TRUE) {
                params += "auto-exposure-lock-supported=true;";
                params += "auto-exposure-lock=false;";
            } else {
                params += "auto-exposure-lock-supported=false;";
            }
            break;
        case ACAMERA_CONTROL_AF_AVAILABLE_MODES:
            if (entry.count > 0) {
                bool found = false;
                std::string af_modes = "focus-mode-values=";
                for (int32_t j = 0; j < entry.count; j++) {
                    if (found)
                        af_modes += ",";
                    bool fixed_lens = false;
                    ACameraMetadata_const_entry min_focus_distance_entry;
                    status = ACameraMetadata_getConstEntry(camera->m_metadata,
                        ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &min_focus_distance_entry);
                    if (status == ACAMERA_OK) {
                        fixed_lens = min_focus_distance_entry.count == 0 ||
                            min_focus_distance_entry.data.f[0] == 0;
                    }
                    af_modes += focus_mode_enum_to_string(entry.data.u8[j], fixed_lens, found);
                }
                params += af_modes + ";";
            }
            break;
        case ACAMERA_CONTROL_AVAILABLE_SCENE_MODES:
            if (entry.count > 0) {
                bool found = false;
                std::string scene_modes = "scene-mode-values=";
                for (int32_t j = 0; j < entry.count; j++) {
                    if (found)
                        scene_modes += ",";
                    scene_modes += scene_mode_enum_to_string(entry.data.u8[j], found);
                }
                params += scene_modes + ";";
            }
            break;
        case ACAMERA_CONTROL_AVAILABLE_EFFECTS:
            if (entry.count > 0) {
                bool found = false;
                std::string effects = "effect-values=";
                for (int32_t j = 0; j < entry.count; j++) {
                    if (found)
                        effects += ",";
                    effects += effect_mode_enum_to_string(entry.data.u8[j], found);
                }
                params += effects + ";";
            }
            break;
        case ACAMERA_CONTROL_AWB_AVAILABLE_MODES:
            if (entry.count > 0) {
                bool found = false;
                std::string wb = "whitebalance-values=";
                for (int32_t j = 0; j < entry.count; j++) {
                    if (found)
                        wb += ",";
                    wb += wb_mode_enum_to_string(entry.data.u8[j], found);
                }
                params += wb + ";";
            }
            break;
        case ACAMERA_CONTROL_AWB_LOCK_AVAILABLE:
            if (entry.count > 0 && entry.data.u8[0] == ACAMERA_CONTROL_AWB_LOCK_AVAILABLE_TRUE) {
                params += "auto-whitebalance-lock-supported=true;";
                params += "auto-whitebalance-lock=false;";
            } else {
                params += "auto-whitebalance-lock-supported=false;";
            }
            break;
        case ACAMERA_CONTROL_MAX_REGIONS:
            if (entry.count >= 3) {
                 params += "max-num-focus-areas="+std::to_string(entry.data.i32[2])+";";
                 params += "max-num-metering-areas="+std::to_string(entry.data.i32[0])+";";
                 camera->max_ae_regions = entry.data.i32[0];
                 camera->max_awb_regions = entry.data.i32[1];
                 camera->max_focus_regions = entry.data.i32[2];
            }
            break;
        case ACAMERA_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES:
            if (entry.count > 0) {
                bool supported = false;
                for (int32_t j = 0; j < entry.count; j++) {
                    if (entry.data.u8[j] == ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON) {
                        supported = true;
                        break;
                    }
                }
                if (supported) {
                 ALOGI("video stabilization supported");
                 params += "video-stabilization-supported=true;";
                }
            }
            break;
#if ANDROID_MAJOR >= 11
        case ACAMERA_CONTROL_ZOOM_RATIO_RANGE:
            if (entry.count >= 2) {
                params += "max-zoom="+std::to_string(entry.data.f[1])+";";
                params += "zoom-supported=true;";
            }
            break;
#else
        case ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM:
            if (entry.count > 0) {
                params += "max-zoom="+std::to_string(entry.data.f[0])+";";
                params += "zoom-supported=true;";
            }
            break;
#endif
        case ACAMERA_FLASH_INFO_AVAILABLE:
            if (!camera_has_flash(camera)) {
                params += "flash-mode-values=off;";
            } else {
                params += "flash-mode-values=off,auto,on,torch;";
            }
            break;
        case ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS: {
            std::string image = "picture-size-values=";
            std::string preview = "preview-size-values=";
            std::string video = "video-size-values=";

            //TODO Verify correct stream configurations for each case
            std::set<int32_t> formats;
            for (int32_t j = 0; j < entry.count; j = j + 4) {
                formats.insert(entry.data.i32[j]);

                switch (entry.data.i32[j]) {
                case AIMAGE_FORMAT_JPEG:
                    if (image.length() > 20)
                        image += ",";
                    image += std::to_string(entry.data.i32[j+1]);
                    image += "x";
                    image += std::to_string(entry.data.i32[j+2]);
                    break;
/*
                case AIMAGE_FORMAT_YUV_420_888:
                    if (preview.length() > 20)
                        preview += ",";
                    preview += std::to_string(entry.data.i32[j+1]);
                    preview += "x";
                    preview += std::to_string(entry.data.i32[j+2]);
                    break;
*/
                case AIMAGE_FORMAT_PRIVATE:
                    if (video.length() > 18)
                        video += ",";
                    video += std::to_string(entry.data.i32[j+1]);
                    video += "x";
                    video += std::to_string(entry.data.i32[j+2]);
                    if (preview.length() > 20)
                        preview += ",";
                    preview += std::to_string(entry.data.i32[j+1]);
                    preview += "x";
                    preview += std::to_string(entry.data.i32[j+2]);
                    break;
                default:
                    break;
                }
            }
            params += image + ";";
            params += preview + ";";
            params += video + ";";

            std::set<int32_t>::iterator itr;
            for (itr = formats.begin(); itr != formats.end(); itr++) {
                ALOGI("format %i %x", *itr, *itr);
            }
            break;
        }
        default:
            break;
        }
    }

    ALOGD("get_parameters result: %s", params.c_str());
    size_t len = params.length();

    char *c_params = (char *)malloc(len + 1);
    if (!c_params) {
        ALOGE("Failed to allocate enough memory for camera parameters");
        return NULL;
    }

    memcpy(c_params, params.c_str(), len);
    c_params[len] = '\0';

    return c_params;
}

bool droid_media_camera_take_picture(DroidMediaCamera *camera, int msgType)
{
    (void)msgType;
    ALOGI("take_picture");

    if (!camera->m_session || !camera->m_image_request) {
        ALOGE("take_picture failed, session or image request missing");
        return false;
    }

    if (camera->m_still_capture_state != STILL_CAPTURE_STATE_IDLE) {
        ALOGW("take_picture ignored, previous still capture is still in progress");
        return false;
    }

    if (!start_precapture_trigger(camera)) {
        ALOGW("Failed to start precapture trigger, capturing immediately");
        return submit_still_capture_request(camera);
    }

    camera->m_still_capture_state = STILL_CAPTURE_STATE_WAITING_PRECAPTURE_DONE;
    camera->m_still_capture_sequence_id = -1;

    return continue_still_capture(camera);
}

void droid_media_camera_release_recording_frame(DroidMediaCamera *camera, DroidMediaCameraRecordingData *data)
{
    (void)camera;
    delete data;
}

nsecs_t droid_media_camera_recording_frame_get_timestamp(DroidMediaCameraRecordingData *data)
{
    if (!data) {
        return 0;
    }
    return data->ts;
}

size_t droid_media_camera_recording_frame_get_size(DroidMediaCameraRecordingData *data)
{
    if (!data || !data->mem.get()) {
        return 0;
    }
    return data->mem->size();
}

void *droid_media_camera_recording_frame_get_data(DroidMediaCameraRecordingData *data)
{
    if (!data || !data->mem.get()) {
        return NULL;
    }
#if ANDROID_MAJOR >= 11
    return data->mem->unsecurePointer();
#else
    return data->mem->pointer();
#endif
}

bool droid_media_camera_enable_face_detection(DroidMediaCamera *camera,
                                              DroidMediaCameraFaceDetectionType type,
                                              bool enable)
{
    ALOGI("enable_face_detection enable: %i", enable);
    camera_status_t status;
    uint8_t faceDetectMode = enable ? ACAMERA_STATISTICS_FACE_DETECT_MODE_SIMPLE
                                    : ACAMERA_STATISTICS_FACE_DETECT_MODE_OFF;
    status = ACaptureRequest_setEntry_u8(camera->m_preview_request,
        ACAMERA_STATISTICS_FACE_DETECT_MODE, 1, &faceDetectMode);

    return status == ACAMERA_OK;
}

int32_t droid_media_camera_get_video_color_format(DroidMediaCamera *camera)
{
    // TODO get video color format
    ALOGD("get_video_color_format");
    //return AIMAGE_FORMAT_YUV_420_888;
    //return AIMAGE_FORMAT_PRIVATE;
    return OMX_COLOR_FormatAndroidOpaque;
}

};

android::sp<android::Camera> droid_media_camera_get_camera(DroidMediaCamera *camera)
{
    // TODO Is this needed with camera2?
    return NULL;
}

bool droid_media_camera_start_external_recording(DroidMediaCamera *camera)
{
    ALOGI("start_external_recording");
    camera_status_t status;
    bool removed_image_output = false;
    bool added_ext_output = false;

    if (!camera->m_session || !camera->m_capture_session_output_container ||
            !camera->m_ext_video_output || !camera->m_ext_video_request) {
        ALOGE("start_external_recording failed, missing session/output/request");
        return false;
    }

    ACameraCaptureSession *old_session = camera->m_session;
    ACameraCaptureSession *new_session = NULL;

    ACameraCaptureSession_stopRepeating(old_session);

    if (camera->m_image_reader_output) {
        status = ACaptureSessionOutputContainer_remove(camera->m_capture_session_output_container,
            camera->m_image_reader_output);
        if (status != ACAMERA_OK) {
            ALOGE("Removing image reader output failed");
            goto fail;
        }
        removed_image_output = true;
    }

    status = ACaptureSessionOutputContainer_add(camera->m_capture_session_output_container,
        camera->m_ext_video_output);
    if (status != ACAMERA_OK) {
        ALOGE("Adding external output failed");
        goto fail;
    }
    added_ext_output = true;

    status = ACameraDevice_createCaptureSession(
        camera->m_device, camera->m_capture_session_output_container,
        &camera->m_capture_session_state_callbacks, &new_session);
    if (status != ACAMERA_OK) {
        ALOGE("setup_capture_session failed (status: %i)", status);
        goto fail;
    }

    camera->m_session = new_session;

    camera->m_video_recording_enabled = true;
    if (!restart_enabled_streams(camera)) {
        goto fail;
    }

    ACameraCaptureSession_close(old_session);

    ALOGD("start_external_recording done");
    return true;

fail:
    if (camera->m_session == new_session && new_session) {
        ACameraCaptureSession_close(new_session);
        camera->m_session = old_session;
    }
    if (added_ext_output) {
        status = ACaptureSessionOutputContainer_remove(camera->m_capture_session_output_container,
            camera->m_ext_video_output);
        if (status != ACAMERA_OK) {
            ALOGE("Rollback removing external output failed");
        }
    }
    if (removed_image_output) {
        status = ACaptureSessionOutputContainer_add(camera->m_capture_session_output_container,
            camera->m_image_reader_output);
        if (status != ACAMERA_OK) {
            ALOGE("Rollback restoring image reader output failed");
        }
    }

    camera->m_video_recording_enabled = false;
    restart_enabled_streams(camera);

    ALOGE("Starting external recording failed");
    return false;
}


void droid_media_camera_stop_external_recording(DroidMediaCamera *camera)
{
    ALOGI("stop_external_recording");
    camera_status_t status;
    ACameraCaptureSession *old_session = camera->m_session;
    ACameraCaptureSession *new_session = NULL;

    camera->m_video_recording_enabled = false;

    if (!camera->m_session || !camera->m_capture_session_output_container) {
        ALOGE("stop_external_recording: missing session/container");
        return;
    }

    ACameraCaptureSession_stopRepeating(old_session);

    if (camera->m_ext_video_output) {
        status = ACaptureSessionOutputContainer_remove(camera->m_capture_session_output_container,
            camera->m_ext_video_output);
        if (status != ACAMERA_OK) {
            ALOGE("Removing external output failed");
        }
    }

    if (camera->m_image_reader_output) {
        status = ACaptureSessionOutputContainer_add(camera->m_capture_session_output_container,
            camera->m_image_reader_output);
        if (status != ACAMERA_OK) {
            ALOGE("Adding image reader output failed");
        }
    }

    // Session outputs changed; create a new session for the updated output set.
    status = ACameraDevice_createCaptureSession(
        camera->m_device, camera->m_capture_session_output_container,
        &camera->m_capture_session_state_callbacks, &new_session);
    if (status != ACAMERA_OK || !new_session) {
        ALOGE("Recreating capture session failed");
        camera->m_session = old_session;
        status = ACameraCaptureSession_setRepeatingRequest(camera->m_session,
            &camera->m_capture_callbacks, 1, &camera->m_preview_request, NULL);
        if (status != ACAMERA_OK) {
            ALOGE("Fallback preview restart failed");
        }
        return;
    }

    camera->m_session = new_session;
    ACameraCaptureSession_close(old_session);

    restart_enabled_streams(camera);
    ALOGI("stop_external_recording done");
}

bool droid_media_camera_set_external_video_window(DroidMediaCamera *camera, ANativeWindow *window)
{
    ALOGI("set_external_video_window start");

    camera_status_t status;

    status = ACameraDevice_createCaptureRequest(camera->m_device,
        TEMPLATE_RECORD, &camera->m_ext_video_request);
    if (status != ACAMERA_OK) {
        goto fail;
    }

    status = ACameraOutputTarget_create(window, &camera->m_ext_video_output_target);
    if (status != ACAMERA_OK) {
        goto fail;
    }

    status = ACaptureRequest_addTarget(camera->m_ext_video_request, camera->m_preview_output_target);
    if (status != ACAMERA_OK) {
        goto fail;
    }

    status = ACaptureRequest_addTarget(camera->m_ext_video_request, camera->m_ext_video_output_target);
    if (status != ACAMERA_OK) {
        goto fail;
    }

    status = ACaptureSessionOutput_create(window, &camera->m_ext_video_output);
    if (status != ACAMERA_OK) {
        ALOGE("ACaptureSessionOutput_create failed %i", status);
        goto fail;
    }

    update_request(camera, camera->m_ext_video_request, camera->m_param_map);

    ALOGD("set_external_video_window done");

    return true;

fail:
    ALOGE("set_external_video_window failed");
    return false;
}

bool droid_media_camera_remove_external_video_window(DroidMediaCamera *camera, ANativeWindow *window)
{
    if (camera->m_ext_video_output_target) {
        ACameraOutputTarget_free(camera->m_ext_video_output_target);
        camera->m_ext_video_output_target = NULL;
    }

    if (camera->m_ext_video_output) {
        ACaptureSessionOutput_free(camera->m_ext_video_output);
        camera->m_ext_video_output = NULL;
    }

    if (camera->m_ext_video_request) {
        ACaptureRequest_free(camera->m_ext_video_request);
        camera->m_ext_video_request = NULL;
    }

    return true;
}

#endif
