cmake_minimum_required(VERSION 3.22.1)
project(sloppy_android LANGUAGES C CXX)

# sloppy-synth for Android: the same engine and control server as the Linux
# player, with Oboe for audio. The UI is the web UI in a WebView, served by
# the control server on 127.0.0.1.

# Oboe needs C++17; the engine sets its own standard in its own folder.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SLOPPY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

# JUCE's audio device layer isn't used on Android (Oboe is), and the engine
# tests and command-line tools aren't needed in the app.
set(SLOPPY_WITH_AUDIO_DEVICES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${SLOPPY_ROOT} sloppy EXCLUDE_FROM_ALL)

find_package(oboe REQUIRED CONFIG)

# juce_core reads CPU details through the NDK's cpufeatures helper.
set(CPU_FEATURES_DIR ${ANDROID_NDK}/sources/android/cpufeatures)
add_library(cpufeatures STATIC ${CPU_FEATURES_DIR}/cpu-features.c)
target_include_directories(cpufeatures PUBLIC ${CPU_FEATURES_DIR})
target_link_libraries(cpufeatures PUBLIC dl)
target_link_libraries(sloppy_engine PUBLIC cpufeatures)

add_library(sloppy_android SHARED
  sloppy_android.cpp
  midi_parser.cpp
)

target_link_libraries(sloppy_android PRIVATE
  sloppy_engine
  oboe::oboe
  android
  log
)

# JNI_OnLoad comes from juce_core and must stay exported, so no
# --exclude-libs here.
target_link_options(sloppy_android PRIVATE -Wl,--gc-sections)
