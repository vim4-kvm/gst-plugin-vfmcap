project('gst-plugin-vfmcap', 'c',
  version : '0.1.0',
  license : 'LGPL-2.1-or-later',
  default_options : ['warning_level=2', 'c_std=gnu11'])

gst_dep = dependency('gstreamer-1.0', version : '>= 1.20')
deps = [
  gst_dep,
  dependency('gstreamer-base-1.0'),
  dependency('gstreamer-video-1.0'),
  dependency('gstreamer-allocators-1.0'),
]

add_project_arguments(
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-DPACKAGE="@0@"'.format(meson.project_name()),
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  language : 'c')

library('gstvfmcap', 'src/gstvfmcapsrc.c',
  dependencies : deps,
  install : true,
  install_dir : gst_dep.get_variable('pluginsdir'))

test('vuy24-repack', executable('test-vuy24', 'tests/test-vuy24.c',
  include_directories : include_directories('src')))
