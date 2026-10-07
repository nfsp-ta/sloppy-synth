/* sloppy-synth: a headless front end for the Vital synthesis engine.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 *
 * Engine is the one object a UI or audio host talks to. It owns Vital's
 * SynthBase (sound engine, modulation matrix, MIDI handling, patch state)
 * and exposes what a host needs: prepare, process a block, load a patch,
 * read and set parameters. Nothing here depends on a GUI toolkit.
 */
#pragma once

#include "JuceHeader.h"
#include "synth_base.h"

#include <string>
#include <vector>

namespace sloppy {

  struct ParameterInfo {
    std::string name;          // Vital's internal id, e.g. "filter_1_cutoff"
    std::string display_name;  // Human-readable name, e.g. "Filter 1 Cutoff"
    float min = 0.0f;
    float max = 1.0f;
    float default_value = 0.0f;
    float value = 0.0f;
  };

  class Engine : public HeadlessSynth {
    public:
      Engine();
      ~Engine() override;

      // Call before processing, and again when the sample rate changes.
      void prepare(double sample_rate, int max_block_size);

      // Renders one block into a stereo buffer, consuming the MIDI events in
      // `midi` (sample positions are relative to the block start). Safe to
      // call from a real-time audio thread: if a patch is being loaded on
      // another thread the block is rendered as silence rather than waiting.
      void process(AudioSampleBuffer& buffer, MidiBuffer& midi);

      // Loads a .vital patch. Vital patches embed their wavetables, samples
      // and LFO shapes, so a single file is all that's needed.
      bool loadPatch(const File& file, std::string& error);
      bool loadPatchFromString(const std::string& patch_json, std::string& error);
      void loadInitPatch();

      bool savePatch(const File& file) { return saveToFile(file); }

      // Parameters, by Vital's internal names (the keys used in .vital files).
      std::vector<std::string> getParameterNames() const;
      bool getParameterInfo(const std::string& name, ParameterInfo& info);
      bool hasParameter(const std::string& name) const;
      float getParameter(const std::string& name);
      // Thread safe; the change is picked up by the audio thread.
      void setParameter(const std::string& name, float value);

      // MIDI input from a device thread lands here; it's merged into the
      // next processed block. Prefer passing events to process() directly
      // when the host already has them in sample-accurate form.
      void addMidiMessage(const MidiMessage& message);

      void allNotesOff();
      void setBpm(float bpm);

      double getSampleRateHz() const { return sample_rate_; }

    private:
      double sample_rate_ = 44100.0;
      double seconds_time_ = 0.0;
      SpinLock midi_lock_;
      MidiBuffer incoming_midi_;
      MidiBuffer pending_midi_;

      JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Engine)
  };

} // namespace sloppy
