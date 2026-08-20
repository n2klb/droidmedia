## Local build instructions for photo-api integration

Building droidmedia with the new Camera2 plugin requires a few additional steps.

A new package package needs to be built from rpm/droidmedia-devel.spec, the name is photo-api-plugin-droid2. Nothing depends on it yet, so don't forget to install it.

There is a new requirement for the Android-side build: The photo-api headers need to be present in the droidmedia directory, including generated code.

It is easiest to start with an installation of photo-api-devel and take a copy of /usr/include/photo-api-0.1. Alternatively, you can extract the files directly from an RPM using the following commands:

```
mkdir -p tmp-extract
rpm2cpio photo-api-devel-xxxx.rpm | (cd tmp-extract; cpio -i -d)
mv tmp-extract/usr/include/photo-api-0.1 .
rm -r tmp-extract
```

After this, build droidmedia as usual.
