project('gst-plugin-vfmcap', 'c',
  version : '0.1.0',
  license : 'LGPL-2.1-or-later',
  default_options : ['warning_level=2', 'c_std=gnu11', 'buildtype=release'])

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

sources = ['src/gstvfmcapsrc.c', 'src/gstvim4vdin1src.c', 'src/codecmm-pool.c']
gpu_dep = dependency('vulkan', required : false)
if gpu_dep.found()
  glslang = find_program('glslangValidator')
  python = import('python').find_installation('python3')
  spirv = custom_target('amly-spirv', input : 'src/amly-to-p010.comp',
    output : 'amly-to-p010.spv', command : [glslang, '-V', '-Os', '@INPUT@', '-o', '@OUTPUT@'])
  shader_header = custom_target('amly-shader-header', input : spirv,
    output : 'amly-to-p010-spirv.h',
    command : [python, files('scripts/embed-spirv.py'), '@INPUT@', '@OUTPUT@'])
  scale_headers = []
  foreach name : ['amly-to-texture', 'texture-to-p010']
    scale_spirv = custom_target(name + '-spirv', input : 'src/' + name + '.comp',
      output : name + '.spv', command : [glslang, '-V', '-Os', '@INPUT@', '-o', '@OUTPUT@'])
    scale_headers += custom_target(name + '-header', input : scale_spirv,
      output : name + '-spirv.h',
      command : [python, files('scripts/embed-spirv.py'), '@INPUT@', '@OUTPUT@'])
  endforeach
  gpu_sources = ['src/amly-gpu.c', 'src/amly-gpu-scale.c', shader_header] + scale_headers
  sources += gpu_sources
  deps += [gpu_dep]
  add_project_arguments('-DHAVE_AMLY_GPU=1', language : 'c')
  # Uses contiguous DMA heaps and real Mali hardware; run only on the board.
  executable('amly-gpu-compare', ['tests/amly-gpu-compare.c'] + gpu_sources,
    dependencies : deps, include_directories : include_directories('src', '.'))
endif

library('gstvfmcap', sources,
  dependencies : deps,
  install : true,
  install_dir : gst_dep.get_variable('pluginsdir'))

test('codecmm-pool', executable('test-codecmm-pool', 'tests/test-codecmm-pool.c',
  dependencies : deps, include_directories : include_directories('src')))

test('vuy24-repack', executable('test-vuy24', 'tests/test-vuy24.c',
  include_directories : include_directories('src')))
test('amly-p010', executable('test-amly', 'tests/test-amly.c',
  include_directories : include_directories('src')))

test('vdin1-capture', executable('test-vdin1',
  ['tests/test-vdin1.c', 'src/gstvim4vdin1src.c'],
  dependencies : deps, include_directories : include_directories('src')))

# Requires an exclusively owned board and a physical HDMI test pattern;
# deliberately not registered as an automatic Meson test.
executable('capture-integrity', 'tests/capture-integrity.c',
  dependencies : [deps, dependency('gstreamer-app-1.0')])
