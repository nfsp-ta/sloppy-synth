/* sloppy-synth: MIDI CC assignments for the macro knobs.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 *
 * Each macro listens to one MIDI CC, on one channel or on any channel.
 * The assignments belong to the device the synth runs on, not to a patch,
 * so a controller keeps working the same way whatever patch is loaded.
 * Everything here is lock free: the audio thread reads the assignments
 * (and finishes a MIDI learn) while UI threads change them.
 */
#pragma once

#include "JuceHeader.h"
#include "synth_constants.h"

#include <atomic>
#include <string>

namespace sloppy {

  struct MacroMidiAssignment {
    int cc = -1;       // 0-119, or -1 for none
    int channel = 0;   // 1-16, or 0 for any channel
  };

  class MacroMidiMap {
    public:
      static constexpr int kNumMacros = vital::kNumMacros;
      // CCs 120-127 are channel mode messages (all notes off and friends).
      static constexpr int kMaxController = 119;
      // Defaults: CC 21-28 on any channel. They're undefined in the MIDI
      // spec, clear of the CCs Vital reacts to (1 mod wheel, 64-67 pedals,
      // 74 MPE slide, 0/32 bank select...), and the knobs of many small
      // controllers send them out of the box.
      static constexpr int kFirstDefaultController = 21;

      static MacroMidiAssignment defaultAssignment(int macro);

      MacroMidiMap();

      MacroMidiAssignment get(int macro) const;
      // Fails if the macro, CC or channel is out of range.
      bool set(int macro, MacroMidiAssignment assignment, std::string& error);
      void resetToDefaults();

      // The next CC received (on any channel) is assigned to `macro`; -1
      // cancels. If the macro listens on a single channel, it moves to the
      // channel the CC arrived on.
      void learn(int macro);
      int getLearning() const { return learning_; }

      // Audio thread. Returns the macro that listens to this CC on this
      // channel (1-16), or -1. Finishes a pending learn first. When several
      // macros share a CC, `start` skips past earlier matches.
      int match(int channel, int cc, int start = 0);

      // Bumped whenever an assignment changes, learn included.
      int getGeneration() const { return generation_; }

      // As stored in the settings file and sent to UIs: a list of
      // { "cc", "channel" }, one per macro.
      std::string toJson() const;
      bool fromJson(const std::string& text, std::string& error);

    private:
      static int pack(MacroMidiAssignment assignment) { return (assignment.cc + 1) | (assignment.channel << 8); }
      static MacroMidiAssignment unpack(int packed) { return { (packed & 0xff) - 1, packed >> 8 }; }

      std::atomic<int> assignments_[kNumMacros];
      std::atomic<int> learning_ { -1 };
      std::atomic<int> generation_ { 0 };
  };

} // namespace sloppy
