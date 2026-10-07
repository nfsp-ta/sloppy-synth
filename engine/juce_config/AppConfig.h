// sloppy-synth: JUCE configuration for the headless engine.
// Replaces the Projucer-generated AppConfig.h from vital/headless. Only
// non-GUI JUCE modules are used, so the engine builds without X11, OpenGL
// or a display, and stays portable to ARM Linux and Android.
#pragma once

#ifndef JUCE_DISPLAY_SPLASH_SCREEN
 #define JUCE_DISPLAY_SPLASH_SCREEN 0
#endif

#define JUCE_MODULE_AVAILABLE_juce_audio_basics  1
#define JUCE_MODULE_AVAILABLE_juce_audio_formats 1
#define JUCE_MODULE_AVAILABLE_juce_core          1
#define JUCE_MODULE_AVAILABLE_juce_data_structures 1
#define JUCE_MODULE_AVAILABLE_juce_dsp           1
#define JUCE_MODULE_AVAILABLE_juce_events        1
#if SLOPPY_WITH_AUDIO_DEVICES
 #define JUCE_MODULE_AVAILABLE_juce_audio_devices 1
#endif

#define JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED 1

// No network stack, no web browser: the engine never talks to the internet.
#define JUCE_USE_CURL 0
#define JUCE_WEB_BROWSER 0
#define JUCE_LOAD_CURL_SYMBOLS_LAZILY 0

#define JUCE_USE_FLAC 0
#define JUCE_USE_OGGVORBIS 0
#define JUCE_USE_MP3AUDIOFORMAT 0
#define JUCE_USE_LAME_AUDIO_FORMAT 0
#define JUCE_USE_WINDOWS_MEDIA_FORMAT 0

#ifndef JUCE_STANDALONE_APPLICATION
 #define JUCE_STANDALONE_APPLICATION 1
#endif
