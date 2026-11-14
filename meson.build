# chimera-core-gearboy: Gearboy (Game Boy, Game Boy Color, Super Game Boy)
# in miniBox's sandbox, as a Chimera core.
#
# Native reference build (run-native + run-wbx under the build directory):
#     meson setup build/meson-native -Dminibox_dir=<miniBox> && ninja -C build/meson-native
# Guest (core.wbx) build via the cross file waterbox/setup-guest.sh writes
# (needs miniBox's C++ guest toolchain, build/meson-cpp):
#     sh waterbox/setup-guest.sh -m <miniBox> && ninja -C build/meson-guest core.wbx
#
# The files are the ones upstream's libretro core builds
# (platforms/libretro/Makefile.common), without its libretro frontend
# (libretro*.cpp): waterbox/gearboy.cpp is this core's frontend. The desktop
# program, its debugger, its MCP server and everything else under
# platforms/ are not built. Both flavours compile the SAME sources; only
# emulibc.h differs (waterbox/native-shim for the reference).
project('chimera-core-gearboy', ['c', 'cpp'],
  default_options: ['c_std=gnu99', 'cpp_std=gnu++11', 'optimization=2', 'debug=false', 'warning_level=0'])

gb = 'extern/gearboy'

# the submodule pin is upstream as it is; the patches are laid over it
# (waterbox/apply-patches.sh does nothing to a tree that has them)
run_command(files('waterbox/apply-patches.sh'), check: true)

mb = get_option('minibox_dir')
if mb == ''
  mb_candidate = meson.current_source_dir() / '../chimera/extern/chimera-common-minibox'
  if import('fs').is_dir(mb_candidate)
    mb = mb_candidate
  else
    error('pass -Dminibox_dir=<miniBox checkout> (or keep one at ../chimera/extern/chimera-common-minibox)')
  endif
endif

# upstream's libretro build's own defines (platforms/libretro/Makefile): no
# disassembler, no VGM recorder, no file dates in zip handling
defs = ['-DGEARBOY_DISABLE_DISASSEMBLER', '-DGEARBOY_DISABLE_VGMRECORDER', '-DMINIZ_NO_TIME', '-DNDEBUG',
  '-DEMULATOR_BUILD="3.8.20"']
# one answer from a floating-point expression on every machine (no fused
# multiply-adds: the sound's band-limited synthesis is set up in floating point)
flags = ['-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off']
add_project_arguments(defs + flags, language: ['c', 'cpp'])

inc = include_directories('waterbox', gb / 'src', gb / 'src/audio', gb / 'platforms/shared/dependencies/miniz')

core_sources = files(
  'waterbox/gearboy.cpp',
  gb / 'platforms/shared/dependencies/miniz/miniz.c',
  gb / 'src/GearboyCore.cpp',
  gb / 'src/Memory.cpp',
  gb / 'src/Processor.cpp',
  gb / 'src/Video.cpp',
  gb / 'src/Audio.cpp',
  gb / 'src/Input.cpp',
  gb / 'src/Cartridge.cpp',
  gb / 'src/BarcodeBoy.cpp',
  gb / 'src/MemoryRule.cpp',
  gb / 'src/CommonMemoryRule.cpp',
  gb / 'src/IORegistersMemoryRule.cpp',
  gb / 'src/RomOnlyMemoryRule.cpp',
  gb / 'src/MBC1MemoryRule.cpp',
  gb / 'src/MBC2MemoryRule.cpp',
  gb / 'src/MBC3MemoryRule.cpp',
  gb / 'src/MBC5MemoryRule.cpp',
  gb / 'src/MBC5LogoMemoryRule.cpp',
  gb / 'src/MBC6MemoryRule.cpp',
  gb / 'src/MultiMBC1MemoryRule.cpp',
  gb / 'src/HuC1MemoryRule.cpp',
  gb / 'src/HuC3MemoryRule.cpp',
  gb / 'src/MMM01MemoryRule.cpp',
  gb / 'src/CameraMemoryRule.cpp',
  gb / 'src/MBC7MemoryRule.cpp',
  gb / 'src/TAMA5MemoryRule.cpp',
  gb / 'src/WisdomTreeMemoryRule.cpp',
  gb / 'src/M161MemoryRule.cpp',
  gb / 'src/SachenMMC1MemoryRule.cpp',
  gb / 'src/SachenMMC2MemoryRule.cpp',
  gb / 'src/RocketMemoryRule.cpp',
  gb / 'src/FlashcartMemoryRule.cpp',
  gb / 'src/BHGOSMemoryRule.cpp',
  gb / 'src/LiChengMemoryRule.cpp',
  gb / 'src/NTNewMemoryRule.cpp',
  gb / 'src/GGB81MemoryRule.cpp',
  gb / 'src/HitekMemoryRule.cpp',
  gb / 'src/VF001MemoryRule.cpp',
  gb / 'src/SintaxMemoryRule.cpp',
  gb / 'src/NTOldMemoryRule.cpp',
  gb / 'src/opcodes.cpp',
  gb / 'src/opcodes_cb.cpp',
  gb / 'src/VgmRecorder.cpp',
  gb / 'src/TraceLogger.cpp',
  gb / 'src/Profiler.cpp',
  gb / 'src/SGB.cpp',
  gb / 'src/audio/Blip_Buffer.cpp',
  gb / 'src/audio/Effects_Buffer.cpp',
  gb / 'src/audio/Gb_Apu.cpp',
  gb / 'src/audio/Gb_Apu_State.cpp',
  gb / 'src/audio/Gb_Oscs.cpp',
  gb / 'src/audio/Multi_Buffer.cpp',
)

if meson.is_cross_build()
  # ---- the guest: core.wbx --------------------------------------------------
  # miniBox's musl and libstdc++ arrive through the cross file. The -u lines
  # name the pthread functions libgcc's unwinder refers to weakly: a weak
  # reference is not enough to pull them out of a static libc, and it would
  # call address zero.
  mbdir = meson.get_external_property('minibox_dir')
  mbuild = meson.get_external_property('minibox_build')
  sr = meson.get_external_property('guest_sysroot')

  executable('core', core_sources,
    name_suffix: 'wbx',
    include_directories: inc,
    link_args: [
      '-static', '-no-pie',
      '-Wl,--eh-frame-hdr', '-Wl,-O2', '-Wl,--no-relax', '-Wl,-z,stack-size=8388608',
      '-T', mbdir / 'source/guest/linkscript.T',
      '-Wl,-u,pthread_once', '-Wl,-u,pthread_cond_wait',
      '-Wl,-u,pthread_cond_broadcast', '-Wl,-u,pthread_key_create',
      '-Wl,-u,pthread_mutexattr_init', '-Wl,-u,pthread_mutexattr_settype',
      '-Wl,-u,pthread_mutexattr_destroy',
      mbuild / 'source/guest/cxxglue.c.o',
      mbuild / 'source/guest/emulibc.c.o',
      '-L' + sr / 'lib', '-lstdc++', '-lgcc', '-lgcc_eh', '-lc'])
else
  # ---- the native reference -------------------------------------------------
  m = meson.get_compiler('c').find_library('m', required: false)
  executable('run-native', core_sources + files('waterbox/run-native.c'),
    include_directories: [include_directories('waterbox/native-shim'), inc],
    c_args: ['-I' + mb / 'source/guest/include', '-I' + mb / 'extern/jsmn'],
    cpp_args: ['-I' + mb / 'source/guest/include', '-I' + mb / 'extern/jsmn'],
    dependencies: [m])

  mbhost = mb / 'build/meson-linux/source/host'
  executable('run-wbx', files('waterbox/run-wbx.c'),
    c_args: ['-I' + mb / 'source/host'],
    link_args: [mbhost / 'libminiboxhost.so'],
    build_rpath: mbhost)
endif
