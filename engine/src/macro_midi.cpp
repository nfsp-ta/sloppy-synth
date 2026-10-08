/* sloppy-synth: MIDI CC assignments for the macro knobs.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
#include "macro_midi.h"

#include "json/json.h"

namespace sloppy {

  using json = nlohmann::json;

  MacroMidiAssignment MacroMidiMap::defaultAssignment(int macro) {
    return { kFirstDefaultController + macro, 0 };
  }

  MacroMidiMap::MacroMidiMap() {
    for (int i = 0; i < kNumMacros; ++i)
      assignments_[i] = pack(defaultAssignment(i));
  }

  MacroMidiAssignment MacroMidiMap::get(int macro) const {
    if (macro < 0 || macro >= kNumMacros)
      return {};
    return unpack(assignments_[macro]);
  }

  bool MacroMidiMap::set(int macro, MacroMidiAssignment assignment, std::string& error) {
    if (macro < 0 || macro >= kNumMacros) {
      error = "No macro " + std::to_string(macro + 1) + ".";
      return false;
    }
    if (assignment.cc < -1 || assignment.cc > kMaxController) {
      error = "CC must be 0 to " + std::to_string(kMaxController) + ".";
      return false;
    }
    if (assignment.channel < 0 || assignment.channel > 16) {
      error = "Channel must be 1 to 16, or 0 for any.";
      return false;
    }
    assignments_[macro] = pack(assignment);
    ++generation_;
    return true;
  }

  void MacroMidiMap::resetToDefaults() {
    for (int i = 0; i < kNumMacros; ++i)
      assignments_[i] = pack(defaultAssignment(i));
    learning_ = -1;
    ++generation_;
  }

  void MacroMidiMap::learn(int macro) {
    learning_ = (macro >= 0 && macro < kNumMacros) ? macro : -1;
    ++generation_;
  }

  int MacroMidiMap::match(int channel, int cc, int start) {
    if (cc < 0 || cc > kMaxController)
      return -1;

    int learning = learning_.exchange(-1);
    if (learning >= 0) {
      MacroMidiAssignment assignment = unpack(assignments_[learning]);
      assignment.cc = cc;
      if (assignment.channel != 0)
        assignment.channel = channel;
      assignments_[learning] = pack(assignment);
      ++generation_;
    }

    for (int i = std::max(start, 0); i < kNumMacros; ++i) {
      MacroMidiAssignment assignment = unpack(assignments_[i]);
      if (assignment.cc == cc && (assignment.channel == 0 || assignment.channel == channel))
        return i;
    }
    return -1;
  }

  std::string MacroMidiMap::toJson() const {
    json list = json::array();
    for (int i = 0; i < kNumMacros; ++i) {
      MacroMidiAssignment assignment = get(i);
      list.push_back({ { "cc", assignment.cc }, { "channel", assignment.channel } });
    }
    return list.dump();
  }

  bool MacroMidiMap::fromJson(const std::string& text, std::string& error) {
    try {
      json list = json::parse(text);
      if (!list.is_array()) {
        error = "Expected a list of macro assignments.";
        return false;
      }
      // Validate everything before changing anything.
      MacroMidiAssignment parsed[kNumMacros];
      for (int i = 0; i < kNumMacros; ++i) {
        parsed[i] = i < static_cast<int>(list.size()) ? MacroMidiAssignment { list[i].value("cc", -1),
                                                                            list[i].value("channel", 0) }
                                                      : defaultAssignment(i);
        if (parsed[i].cc < -1 || parsed[i].cc > kMaxController || parsed[i].channel < 0 || parsed[i].channel > 16) {
          error = "Macro " + std::to_string(i + 1) + " has an out of range CC or channel.";
          return false;
        }
      }
      for (int i = 0; i < kNumMacros; ++i)
        assignments_[i] = pack(parsed[i]);
      ++generation_;
      return true;
    }
    catch (const json::exception& e) {
      error = std::string("Bad macro assignments: ") + e.what();
      return false;
    }
  }

} // namespace sloppy
