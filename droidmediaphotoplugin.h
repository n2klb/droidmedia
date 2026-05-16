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

#ifndef DROID_MEDIA_PHOTO_PLUGIN_H
#define DROID_MEDIA_PHOTO_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <photo-backend.h>

#include "droidmedia.h"

typedef struct _DroidMediaPhotoInterface {
    bool (*register_camera)(PhotoBackendCamera *camera, const char *id,
                            const PhotoCameraImpl *impl);
    bool (*init_stream)(PhotoBackendCamera *camera, PhotoBackendStream *stream,
                        PhotoStreamUsage usage, const PhotoStreamImpl *impl);
    void (*begin_state_change)(PhotoBackendCamera *camera);
    void (*end_state_change)(PhotoBackendCamera *camera, bool changed);
    void (*notify_shutter)(PhotoBackendCamera *camera);
    void (*notify_capture)(PhotoBackendCamera *camera, bool successful);
    void (*notify_disconnected)(PhotoBackendCamera *camera);
    bool (*init_buffer)(PhotoBackendStream *stream, PhotoBackendBuffer *buffer,
                        unsigned int num_planes, PhotoBufferPlaneInfo *planes,
                        const PhotoBufferImpl *impl);
    void (*frame_available)(PhotoBackendStream *stream);
    void (*unbind_buffer)(PhotoBackendBuffer *buffer);
    PFNEGLCREATEIMAGEKHRPROC egl_create_image;
} DroidMediaPhotoInterface;

#ifdef __cplusplus
extern "C" {
#endif

bool droid_media_photo_plugin_init(const DroidMediaPhotoInterface *interface);

#ifdef __cplusplus
};
#endif

#endif
