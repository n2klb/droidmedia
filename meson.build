project(
  'droidmedia',
  ['c', 'cpp'],
  default_options : ['warning_level=1', 'buildtype=debugoptimized'],
version : '0.20260508.2+android15.cameraproxy.20260805140735.6b18ef1'
)

droidmedia_version = meson.project_version()
droidmedia = meson.project_name()

cc = meson.get_compiler('c')
dl_dep = cc.find_library('dl', required : true)

root_dir = include_directories('.')

egl_dep = dependency('egl', required: true)
photo_api_dep = dependency('photo-api-0.1', required: true)

droidmedia_source = [
  'hybris.c',
]

libdroidmedia_a = static_library('droidmedia',
                                 droidmedia_source,
                                 install: true,
                                 dependencies: [dl_dep, photo_api_dep])

droidmedia_headers = [
    'droidmediacamera.h',
    'droidmediacodec.h',
    'droidmediaconstants.h',
    'droidmediaconvert.h',
    'droidmedia.h',
    'droidmediarecorder.h'
]

install_headers(droidmedia_headers, subdir : meson.project_name())
install_data('hybris.c', install_dir : get_option('datadir') / meson.project_name())

qt5 = import('qt5')

flashlight_qt_deps = [
  dependency('Qt5Core'),
  dependency('Qt5DBus'),
]

flashlight_deps = flashlight_qt_deps + [
  dependency('libresourceqt5', version : '>=1.29'),
  dl_dep,
]

flashlight_moc = qt5.preprocess(
  moc_headers : ['flashlight/flashlightservice.h'],
  dependencies : flashlight_qt_deps,
)

executable(
  'droidmedia-flashlight',
  [
    'flashlight/main.cpp',
    'flashlight/flashlightservice.cpp',
    flashlight_moc,
  ],
  include_directories : root_dir,
  cpp_args : ['-fPIC'],
  dependencies : flashlight_deps,
  link_with : libdroidmedia_a,
  install : true,
  install_dir : get_option('bindir'),
)

install_data(
  'flashlight/org.sailfish.flashlight.provider.service',
  install_dir : get_option('datadir') / 'dbus-1' / 'services',
)

pkg = import('pkgconfig')
pkg.generate(libdroidmedia_a,
             libraries: ['-ldl'],
             subdirs: ['droidmedia'])

photoplugin_dir = photo_api_dep.get_variable('plugin_dir')

photoplugin_source = [
  'hybris-photoplugin.c',
]

shared_module(
  'photoplugin-droid2',
  photoplugin_source,
  install : true,
  link_with : libdroidmedia_a,
  dependencies : [dl_dep, egl_dep, photo_api_dep],
  install_dir : photoplugin_dir
)
