/* sloppy-synth: turns raw MIDI bytes into whole messages.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 *
 * Android's MIDI API hands over raw bytes, which may hold several messages,
 * part of one, running status, or real-time bytes in the middle of another
 * message. One parser per input port keeps that state.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace sloppy {

  class MidiParser {
    public:
      using Callback = std::function<void(const uint8_t* data, int size)>;

      // Calls `callback` once per complete message. System exclusive
      // messages are dropped: the engine has no use for them.
      void parse(const uint8_t* data, size_t size, const Callback& callback);

      static int messageLength(uint8_t status);

    private:
      uint8_t running_status_ = 0;
      uint8_t message_[3] = {};
      int length_ = 0;
      int expected_ = 0;
      bool in_sysex_ = false;
  };

} // namespace sloppy
