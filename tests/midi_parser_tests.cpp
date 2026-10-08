/* sloppy-synth tests for the Android app's MIDI byte parser.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
#include "midi_parser.h"

#include <iostream>
#include <vector>

namespace {
  int failures = 0;

  using Messages = std::vector<std::vector<uint8_t>>;

  Messages parse(sloppy::MidiParser& parser, std::vector<uint8_t> bytes) {
    Messages messages;
    parser.parse(bytes.data(), bytes.size(), [&](const uint8_t* data, int size) {
      messages.emplace_back(data, data + size);
    });
    return messages;
  }

  void expect(const char* name, const Messages& got, const Messages& want) {
    if (got != want) {
      ++failures;
      std::cerr << "  FAILED " << name << ": got " << got.size() << " messages, wanted " << want.size() << "\n";
    }
  }
}

int main() {
  {
    sloppy::MidiParser parser;
    expect("whole messages", parse(parser, { 0x90, 60, 100, 0x80, 60, 0 }),
           { { 0x90, 60, 100 }, { 0x80, 60, 0 } });
  }
  {
    sloppy::MidiParser parser;
    expect("running status", parse(parser, { 0x90, 60, 100, 64, 100, 60, 0 }),
           { { 0x90, 60, 100 }, { 0x90, 64, 100 }, { 0x90, 60, 0 } });
  }
  {
    sloppy::MidiParser parser;
    expect("split first half", parse(parser, { 0xb0, 74 }), {});
    expect("split second half", parse(parser, { 20, 75, 30 }), { { 0xb0, 74, 20 }, { 0xb0, 75, 30 } });
  }
  {
    sloppy::MidiParser parser;
    expect("two byte messages", parse(parser, { 0xc0, 5, 7, 0xd0, 64 }),
           { { 0xc0, 5 }, { 0xc0, 7 }, { 0xd0, 64 } });
  }
  {
    sloppy::MidiParser parser;
    expect("clock inside a note", parse(parser, { 0x90, 0xf8, 60, 100 }),
           { { 0xf8 }, { 0x90, 60, 100 } });
  }
  {
    sloppy::MidiParser parser;
    expect("sysex dropped", parse(parser, { 0xf0, 0x7e, 1, 2, 0xf7, 0x90, 60, 1 }), { { 0x90, 60, 1 } });
  }
  {
    sloppy::MidiParser parser;
    expect("stray data ignored", parse(parser, { 60, 100, 0x80, 60, 0 }), { { 0x80, 60, 0 } });
  }
  {
    sloppy::MidiParser parser;
    expect("system common ends running status", parse(parser, { 0x90, 60, 1, 0xf3, 2, 61, 1 }),
           { { 0x90, 60, 1 }, { 0xf3, 2 } });
  }

  std::cout << (failures ? "midi parser tests failed\n" : "midi parser tests passed\n");
  return failures ? 1 : 0;
}
