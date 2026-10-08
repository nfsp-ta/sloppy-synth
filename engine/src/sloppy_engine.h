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

#include <atomic>
#include <mutex>
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

    // How Vital displays the value: scale it (see vital::ValueDetails),
    // add post_offset, invert if asked, multiply, then append units.
    int value_scale = 1;  // vital::ValueDetails::ValueScale
    float post_offset = 0.0f;
    float display_multiply = 1.0f;
    bool display_invert = false;
    std::string units;
    // Names for indexed values (filter models, waveforms...), if any.
    std::vector<std::string> options;
  };

  // One routing in Vital's modulation matrix. Its depth and options are
  // ordinary parameters named after the slot: modulation_<slot>_amount
  // (-1 to 1), _bipolar, _stereo, _bypass and _power.
  struct Modulation {
    int slot = 0;              // 1 to 64
    std::string source;        // e.g. "lfo_1", "env_2", "macro_control_1"
    std::string destination;   // a parameter name, e.g. "filter_1_cutoff"
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
      //
      // Patches from newer Vital releases (1.x) load too. Whatever this
      // engine doesn't have is skipped or reset, and listed in
      // getLoadWarnings(). Patches from a newer major version are refused.
      bool loadPatch(const File& file, std::string& error);
      bool loadPatchFromString(const std::string& patch_json, std::string& error);
      void loadInitPatch();

      // What the last loaded patch uses that this engine can't play, one
      // readable line each. Empty when the patch loaded fully.
      std::vector<std::string> getLoadWarnings() const;
      // The Vital version that saved the last loaded patch, e.g. "1.6.4".
      std::string getPatchVersion() const;

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

      // Modulation matrix. Names are Vital's, as used in .vital files.
      std::vector<std::string> getModulationSources();
      std::vector<std::string> getModulationDestinations();
      std::vector<Modulation> getModulations();
      // Routes source to destination with the given amount (-1 to 1). If the
      // routing exists already, only its amount changes. Fails when a name is
      // unknown or all 64 slots are in use.
      bool addModulation(const std::string& source, const std::string& destination, float amount,
                         std::string& error);
      bool removeModulation(const std::string& source, const std::string& destination);
      // Changes whenever routings are added or removed, or a patch is loaded.
      int getModulationGeneration() const { return modulation_generation_; }

      void allNotesOff();
      void setBpm(float bpm);

      double getSampleRateHz() const { return sample_rate_; }

    private:
      bool loadJson(json state, std::string& error);
      void checkUnsupported(const json& original, const std::string& version,
                            std::vector<std::string>& warnings);

      double sample_rate_ = 44100.0;
      double seconds_time_ = 0.0;
      SpinLock midi_lock_;
      MidiBuffer incoming_midi_;
      MidiBuffer pending_midi_;
      std::atomic<int> modulation_generation_ { 0 };
      mutable std::mutex load_info_lock_;
      std::vector<std::string> load_warnings_;
      std::string patch_version_;

      JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Engine)
  };

} // namespace sloppy
