project(
  'droidmedia', 'c',
  default_options : ['warning_level=1', 'buildtype=debugoptimized'],
)

droidmedia_version = meson.project_version()
droidmedia = meson.project_name()

cc = meson.get_compiler('c')

root_dir = include_directories('.')

egl_dep = dependency('egl', required: true)
photo_api_dep = dependency('photo-api-0.1', required: true)

droidmedia_source = [
  'hybris.c',
]

droidmedia_deps = [
  photo_api_dep,
  cc.find_library('dl', required : true),
]

libdroidmedia_a = static_library('droidmedia',
                                 droidmedia_source,
                                 install: true,
                                 dependencies: droidmedia_deps)

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

pkg = import('pkgconfig')
pkg.generate(libdroidmedia_a,
             libraries: ['-ldl'],
             subdirs: ['droidmedia'])

photoplugin_dir = photo_api_dep.get_variable('plugin_dir')

photoplugin_source = [
  'hybris-photoplugin.c',
]

shared_module('photoplugin-droid2',
              photoplugin_source,
              install: true,
              link_with: [libdroidmedia_a],
              dependencies: [egl_dep, photo_api_dep],
              install_dir: photoplugin_dir)
