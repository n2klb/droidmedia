# droidmedia camera2-plugin branch

The `droidmediaphotoplugin2.cpp` and `hybris-photoplugin.c` files provide a photo-api backend using the Camera2 NDK API.

## Build instructions

Due to the dependency on photo-api, building this involves more steps compared to plain droidmedia.

1. Checkout the `camera2-plugin` branch in external/droidmedia.
2. Clone <https://github.com/n2klb/camera-api> into external/droidmedia/camera-api
3. Build Android side using `make droidmedia`
4. Build `droidmedia-localbuild` package as usual
5. Build `rpm/photo-api.spec` from `camera-api` directory.
6. Build `rpm/droidmedia-devel.spec` locally, this will now also produce the `photo-api-plugin-droid2` package.

Using `rpm/dhd/helpers/build_packages.sh`, the last three steps can be done with following commands:

```
alias bp2=rpm/dhd/helpers/build_packages.sh
bp2 -g
bp2 -b external/droidmedia/camera-api
bp2 -b external/droidmedia -s rpm/droidmedia-devel.spec
```

Finally, to test this with existing camera applications, build the `photo-api` branch of
<https://github.com/n2klb/qtmultimedia>. On the device, set the `QT_GSTREAMER_CAMERABIN_SRC`
environment variable to `photocamerasrc` in /var/lib/environment/nemo/60-multimedia.conf.
