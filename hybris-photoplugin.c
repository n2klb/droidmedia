// SPDX-FileCopyrightText: 2026 Jolla Mobile Ltd
//
// SPDX-License-Identifier: LGPL-2.1

#include <stdio.h>

#include "droidmediaphotoplugin.h"

static DroidMediaPhotoInterface droid_media_photo_interface = {
    .register_camera = photo_backend_register_camera,
    .init_stream = photo_backend_init_stream,
    .begin_state_change = photo_backend_begin_state_change,
    .end_state_change = photo_backend_end_state_change,
    .notify_shutter = photo_backend_notify_shutter,
    .notify_capture = photo_backend_notify_capture,
    .notify_disconnected = photo_backend_notify_disconnected,
    .init_buffer = photo_backend_init_buffer,
    .frame_available = photo_backend_frame_available,
    .unbind_buffer = photo_backend_unbind_buffer,
};

bool photo_plugin_init(void)
{
    if (!droid_media_init()) {
        return false;
    }

    // This has to be the libhybris function, not the Android one
    droid_media_photo_interface.egl_create_image = eglGetProcAddress("eglCreateImageKHR");

    return droid_media_photo_plugin_init(&droid_media_photo_interface);
}
