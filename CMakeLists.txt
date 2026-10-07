cmake_minimum_required(VERSION 3.16)
project(sloppy_synth VERSION 0.1.0 LANGUAGES C CXX)

# sloppy-synth: headless build of the Vital synthesis engine.
#
# Builds Vital's DSP engine (vital/src/synthesis) and its patch/state layer
# (vital/src/common) against the non-GUI JUCE modules only, plus small
# command-line front ends. No X11, OpenGL, curl or display is needed, so the
# same build works on a desktop, a Raspberry Pi and (later) Android.

option(SLOPPY_WITH_AUDIO_DEVICES "Build the real-time player (needs ALSA headers on Linux)" ON)
option(SLOPPY_WITH_JACK "Let the real-time player use JACK as well as ALSA" OFF)
set(SLOPPY_ARM_CPU "cortex-a53" CACHE STRING "CPU to tune for on ARM (Raspberry Pi 3 is cortex-a53)")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(VITAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vital)
set(JUCE_MODULES_DIR ${VITAL_DIR}/third_party/JUCE/modules)
set(JUCE_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine/juce_config)

# ---------------------------------------------------------------------------
# Target CPU flags. Vital's DSP is written against SSE2 or NEON; pick the
# right one for the target so the hot loops stay vectorised.
# ---------------------------------------------------------------------------
set(SLOPPY_ARCH_FLAGS "")
set(SLOPPY_ARCH_DEFINES "")
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _proc)
if(ANDROID)
  # The Android NDK picks the ABI flags itself.
  if(ANDROID_ABI STREQUAL "armeabi-v7a")
    list(APPEND SLOPPY_ARCH_DEFINES NEON_ARM32=1 _FILE_OFFSET_BITS=64)
  endif()
elseif(_proc MATCHES "^(aarch64|arm64)")
  set(SLOPPY_ARCH_FLAGS -march=armv8-a -mtune=${SLOPPY_ARM_CPU})
elseif(_proc MATCHES "^arm")
  # 32-bit ARM (Raspberry Pi OS armhf on a Pi 2 v1.2 / 3 / 4). NEON on
  # AArch32 has no vector divide, so Vital needs NEON_ARM32 to use the
  # reciprocal-estimate path instead.
  set(SLOPPY_ARCH_FLAGS -march=armv8-a+crc -mtune=${SLOPPY_ARM_CPU} -mfpu=neon-fp-armv8 -mfloat-abi=hard)
  list(APPEND SLOPPY_ARCH_DEFINES NEON_ARM32=1)
  # 64-bit file offsets and inodes. Without this, readdir() on a 32-bit
  # userland can fail with EOVERFLOW when the kernel or filesystem hands
  # back 64-bit values (64-bit Pi kernels, qemu-user), and patch library
  # scans silently come back short.
  list(APPEND SLOPPY_ARCH_DEFINES _FILE_OFFSET_BITS=64)
elseif(_proc MATCHES "^(x86_64|amd64|i.86)")
  set(SLOPPY_ARCH_FLAGS -msse2)
endif()

# ---------------------------------------------------------------------------
# JUCE (non-GUI modules only)
# ---------------------------------------------------------------------------
set(JUCE_MODULE_SOURCES
  ${JUCE_CONFIG_DIR}/include_juce_audio_basics.cpp
  ${JUCE_CONFIG_DIR}/include_juce_audio_formats.cpp
  ${JUCE_CONFIG_DIR}/include_juce_core.cpp
  ${JUCE_CONFIG_DIR}/include_juce_data_structures.cpp
  ${JUCE_CONFIG_DIR}/include_juce_dsp.cpp
  ${JUCE_CONFIG_DIR}/include_juce_events.cpp
)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# sloppy_engine: Vital's synthesis engine + patch loading, no UI.
# ---------------------------------------------------------------------------
add_library(sloppy_engine STATIC
  ${VITAL_DIR}/src/unity_build/synthesis.cpp
  engine/src/common_unity.cpp  # includes vital/src/unity_build/common.cpp
  engine/src/sloppy_engine.cpp
  engine/src/patch_library.cpp
  engine/src/control_server.cpp
  ${JUCE_MODULE_SOURCES}
)

target_include_directories(sloppy_engine PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/engine/src
  ${JUCE_CONFIG_DIR}
  ${JUCE_MODULES_DIR}
  ${VITAL_DIR}/src/common
  ${VITAL_DIR}/src/common/wavetable
  ${VITAL_DIR}/src/interface/look_and_feel  # only synth_strings.h (parameter value names)
  ${VITAL_DIR}/src/synthesis/synth_engine
  ${VITAL_DIR}/src/synthesis/effects
  ${VITAL_DIR}/src/synthesis/effects_engine
  ${VITAL_DIR}/src/synthesis/filters
  ${VITAL_DIR}/src/synthesis/framework
  ${VITAL_DIR}/src/synthesis/lookups
  ${VITAL_DIR}/src/synthesis/modulators
  ${VITAL_DIR}/src/synthesis/modules
  ${VITAL_DIR}/src/synthesis/producers
  ${VITAL_DIR}/src/synthesis/utilities
  ${VITAL_DIR}/third_party
)

target_compile_definitions(sloppy_engine PUBLIC
  HEADLESS=1
  NO_AUTH=1
  JUCE_STANDALONE_APPLICATION=1
  JUCE_USE_XRANDR=0
  $<$<CONFIG:Debug>:DEBUG=1 _DEBUG=1>
  $<$<NOT:$<CONFIG:Debug>>:NDEBUG=1>
  ${SLOPPY_ARCH_DEFINES}
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(sloppy_engine PUBLIC LINUX=1)
endif()

# Same optimisation recipe as Vital's own Linux builds.
target_compile_options(sloppy_engine PUBLIC
  ${SLOPPY_ARCH_FLAGS}
  -ffast-math
  $<$<NOT:$<CONFIG:Debug>>:-O3 -ftree-vectorize -ftree-slp-vectorize -funroll-loops>
  -Wno-deprecated-declarations
)

target_link_libraries(sloppy_engine PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# juce_audio_devices changes how Vital's MidiManager is declared (it stubs
# out MidiInput when the module is missing), so it has to be on or off for
# the whole engine, never per target.
if(SLOPPY_WITH_AUDIO_DEVICES)
  target_sources(sloppy_engine PRIVATE ${JUCE_CONFIG_DIR}/include_juce_audio_devices.cpp)
  target_compile_definitions(sloppy_engine PUBLIC SLOPPY_WITH_AUDIO_DEVICES=1)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(ALSA REQUIRED)
    target_link_libraries(sloppy_engine PUBLIC ALSA::ALSA)
    if(SLOPPY_WITH_JACK)
      # JUCE loads libjack at run time; only the headers are needed here.
      target_compile_definitions(sloppy_engine PUBLIC JUCE_JACK=1)
    else()
      target_compile_definitions(sloppy_engine PUBLIC JUCE_JACK=0)
    endif()
  endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(sloppy_engine PUBLIC rt)
endif()

# ---------------------------------------------------------------------------
# sloppy-render: load a .vital patch and render notes to a WAV file.
# Works everywhere, needs no audio hardware; used for tests and CI.
# ---------------------------------------------------------------------------
add_executable(sloppy-render apps/sloppy-render/main.cpp)
target_link_libraries(sloppy-render PRIVATE sloppy_engine)

# ---------------------------------------------------------------------------
# sloppy-synth: real-time player. Loads a patch or bank and plays it from
# MIDI through ALSA (or JACK). This is the Raspberry Pi target.
# ---------------------------------------------------------------------------
if(SLOPPY_WITH_AUDIO_DEVICES)
  add_executable(sloppy-synth apps/sloppy-synth/main.cpp)
  target_link_libraries(sloppy-synth PRIVATE sloppy_engine)
  # The web UI is served from a "web" folder next to the program.
  add_custom_command(TARGET sloppy-synth POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/web $<TARGET_FILE_DIR:sloppy-synth>/web)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(TARGETS sloppy-render RUNTIME DESTINATION bin)
if(SLOPPY_WITH_AUDIO_DEVICES)
  install(TARGETS sloppy-synth RUNTIME DESTINATION bin)
  install(DIRECTORY web/ DESTINATION share/sloppy-synth/web)
endif()
