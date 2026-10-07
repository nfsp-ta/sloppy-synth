// sloppy-synth: JUCE umbrella header for the headless engine.
// Vital's sources include "JuceHeader.h"; this version pulls in only the
// non-GUI modules configured in AppConfig.h.
#pragma once

#include "AppConfig.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>
#if SLOPPY_WITH_AUDIO_DEVICES
 #include <juce_audio_devices/juce_audio_devices.h>
#endif

#if ! DONT_SET_USING_JUCE_NAMESPACE
 using namespace juce;
#endif

#if ! JUCE_DONT_DECLARE_PROJECTINFO
namespace ProjectInfo {
  const char* const projectName = "sloppy-synth";
  const char* const companyName = "sloppy-synth";
  // Patches are tagged with this version, and Vital refuses to load patches
  // whose major.minor is newer than it. Track the newest Vital release so
  // presets made in current Vital import cleanly.
  const char* const versionString = "1.5.5";
  const int versionNumber = 0x10505;
}
#endif
