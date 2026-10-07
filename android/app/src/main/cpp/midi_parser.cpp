/* sloppy-synth: turns raw MIDI bytes into whole messages.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
#include "midi_parser.h"

namespace sloppy {

  int MidiParser::messageLength(uint8_t status) {
    if (status < 0xf0) {
      switch (status & 0xf0) {
        case 0xc0:  // program change
        case 0xd0:  // channel pressure
          return 2;
        default:
          return 3;
      }
    }
    switch (status) {
      case 0xf1:  // time code quarter frame
      case 0xf3:  // song select
        return 2;
      case 0xf2:  // song position
        return 3;
      default:
        return 1;
    }
  }

  void MidiParser::parse(const uint8_t* data, size_t size, const Callback& callback) {
    for (size_t i = 0; i < size; ++i) {
      uint8_t byte = data[i];

      // Real-time messages can appear anywhere, even inside other messages.
      if (byte >= 0xf8) {
        callback(&byte, 1);
        continue;
      }

      if (byte & 0x80) {
        if (byte == 0xf0) {
          in_sysex_ = true;
          running_status_ = 0;
          length_ = 0;
          continue;
        }
        if (byte == 0xf7) {
          in_sysex_ = false;
          continue;
        }
        in_sysex_ = false;

        // System common messages cancel running status.
        running_status_ = byte < 0xf0 ? byte : 0;
        message_[0] = byte;
        length_ = 1;
        expected_ = messageLength(byte);
        if (expected_ == 1) {
          callback(message_, 1);
          length_ = 0;
        }
        continue;
      }

      if (in_sysex_)
        continue;

      // A data byte with no status in progress: reuse the running status.
      if (length_ == 0) {
        if (running_status_ == 0)
          continue;
        message_[0] = running_status_;
        length_ = 1;
        expected_ = messageLength(running_status_);
      }

      message_[length_++] = byte;
      if (length_ == expected_) {
        callback(message_, length_);
        length_ = 0;
      }
    }
  }

} // namespace sloppy
