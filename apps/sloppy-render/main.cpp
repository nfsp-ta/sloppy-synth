/* sloppy-render: load a Vital patch and render notes to a WAV file.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 *
 * Needs no audio hardware, so it doubles as a smoke test for new targets:
 * if a patch renders to sensible audio here, the engine works on that CPU.
 */
#include "JuceHeader.h"
#include "patch_library.h"
#include "sloppy_engine.h"
#include "tuning.h"

#include <chrono>
#include <cmath>
#include <iostream>

namespace {
  void printUsage() {
    std::cout <<
      "Usage: sloppy-render [patch.vital] [options]\n"
      "\n"
      "Renders notes through a Vital patch (or the init patch) to a WAV file.\n"
      "\n"
      "Options:\n"
      "  -o, --output FILE       WAV file to write (default: render.wav)\n"
      "  -n, --notes LIST        Notes to hold, MIDI numbers or names, e.g. C3,E3,G3 (default: C3)\n"
      "  -l, --length SECONDS    How long to hold the notes (default: 3)\n"
      "  -t, --tail SECONDS      Release time to render after note off (default: 1)\n"
      "  -r, --rate HZ           Sample rate (default: 44100)\n"
      "  -b, --block SAMPLES     Block size (default: 256)\n"
      "      --bpm BPM           Tempo for synced LFOs and effects (default: 120)\n"
      "      --set NAME=VALUE    Set a parameter after loading (repeatable)\n"
      "      --save FILE         Save the patch (after --set changes) as a .vital file, then exit\n"
      "      --list-params       Print every parameter and its current value, then exit\n"
      "      --import-bank FILE  Unpack a .vitalbank into the patch library, then exit\n"
      "      --list-patches      List patches in the patch library, then exit\n"
      "      --library DIR       Patch library folder (default: $SLOPPY_DATA_DIR or ~/.local/share/sloppy-synth)\n"
      "  -h, --help              Show this help\n";
  }

  struct Options {
    String patch;
    String output = "render.wav";
    String notes = "C3";
    double length = 3.0;
    double tail = 1.0;
    double sample_rate = 44100.0;
    int block_size = 256;
    float bpm = 120.0f;
    StringArray sets;
    bool list_params = false;
    bool list_patches = false;
    String import_bank;
    String library;
    String save;
  };

  bool parseArgs(int argc, const char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
      String arg = argv[i];
      auto next = [&](String& value) {
        if (i + 1 >= argc) {
          std::cerr << "Missing value for " << arg << "\n";
          return false;
        }
        value = argv[++i];
        return true;
      };

      String value;
      if (arg == "-h" || arg == "--help") { printUsage(); exit(0); }
      else if (arg == "-o" || arg == "--output") { if (!next(options.output)) return false; }
      else if (arg == "-n" || arg == "--notes") { if (!next(options.notes)) return false; }
      else if (arg == "-l" || arg == "--length") { if (!next(value)) return false; options.length = value.getDoubleValue(); }
      else if (arg == "-t" || arg == "--tail") { if (!next(value)) return false; options.tail = value.getDoubleValue(); }
      else if (arg == "-r" || arg == "--rate") { if (!next(value)) return false; options.sample_rate = value.getDoubleValue(); }
      else if (arg == "-b" || arg == "--block") { if (!next(value)) return false; options.block_size = value.getIntValue(); }
      else if (arg == "--bpm") { if (!next(value)) return false; options.bpm = value.getFloatValue(); }
      else if (arg == "--set") { if (!next(value)) return false; options.sets.add(value); }
      else if (arg == "--save") { if (!next(options.save)) return false; }
      else if (arg == "--list-params") options.list_params = true;
      else if (arg == "--list-patches") options.list_patches = true;
      else if (arg == "--import-bank") { if (!next(options.import_bank)) return false; }
      else if (arg == "--library") { if (!next(options.library)) return false; }
      else if (arg.startsWith("-")) { std::cerr << "Unknown option " << arg << "\n"; return false; }
      else options.patch = arg;
    }

    if (options.sample_rate < 8000.0 || options.sample_rate > 192000.0 ||
        options.block_size < 1 || options.length < 0.0 || options.tail < 0.0) {
      std::cerr << "Invalid sample rate, block size or length.\n";
      return false;
    }
    return true;
  }

  std::vector<int> parseNotes(const String& list) {
    std::vector<int> notes;
    StringArray tokens;
    tokens.addTokens(list, ",", "");
    for (String token : tokens) {
      token = token.trim();
      int note = token.containsOnly("0123456789") ? token.getIntValue() : Tuning::noteToMidiKey(token);
      if (note >= 0 && note < 128)
        notes.push_back(note);
      else
        std::cerr << "Ignoring note " << token << "\n";
    }
    return notes;
  }

  File resolve(const String& path) {
    return File::getCurrentWorkingDirectory().getChildFile(path);
  }
}

int main(int argc, const char* argv[]) {
  Options options;
  if (!parseArgs(argc, argv, options)) {
    printUsage();
    return 2;
  }

  sloppy::PatchLibrary library(options.library.isNotEmpty() ? resolve(options.library)
                                                            : sloppy::PatchLibrary::defaultRoot());

  if (options.import_bank.isNotEmpty()) {
    std::string bank_name, error;
    if (!library.importBank(resolve(options.import_bank), bank_name, error)) {
      std::cerr << "Import failed: " << error << "\n";
      return 1;
    }
    std::cout << "Imported bank \"" << bank_name << "\" into " << library.getRoot().getFullPathName() << "\n";
    return 0;
  }

  if (options.list_patches) {
    for (const sloppy::PatchEntry& patch : library.listPatches())
      std::cout << patch.bank << "\t" << patch.category << "\t" << patch.name << "\t"
                << patch.file.getFullPathName() << "\n";
    return 0;
  }

  sloppy::Engine engine;

  if (options.patch.isNotEmpty()) {
    std::string error;
    auto start = std::chrono::steady_clock::now();
    if (!engine.loadPatch(resolve(options.patch), error)) {
      std::cerr << "Couldn't load " << options.patch << ": " << error << "\n";
      return 1;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::cerr << "Loaded \"" << engine.getPresetName() << "\" in " << ms.count() << " ms\n";
  }

  for (const String& set : options.sets) {
    String name = set.upToFirstOccurrenceOf("=", false, false).trim();
    float value = set.fromFirstOccurrenceOf("=", false, false).getFloatValue();
    if (!engine.hasParameter(name.toStdString())) {
      std::cerr << "Unknown parameter " << name << "\n";
      return 1;
    }
    engine.setParameter(name.toStdString(), value);
  }

  if (options.save.isNotEmpty()) {
    File save_file = resolve(options.save);
    if (!engine.savePatch(save_file)) {
      std::cerr << "Couldn't save " << save_file.getFullPathName() << "\n";
      return 1;
    }
    std::cerr << "Saved " << save_file.getFullPathName() << "\n";
    return 0;
  }

  if (options.list_params) {
    for (const std::string& name : engine.getParameterNames()) {
      sloppy::ParameterInfo info;
      if (engine.getParameterInfo(name, info))
        std::cout << name << "\t" << info.value << "\t[" << info.min << ", " << info.max << "]\t"
                  << info.display_name << "\n";
    }
    return 0;
  }

  std::vector<int> notes = parseNotes(options.notes);

  engine.prepare(options.sample_rate, options.block_size);
  engine.setBpm(options.bpm);

  File output = resolve(options.output);
  output.deleteFile();
  std::unique_ptr<OutputStream> stream = output.createOutputStream();
  if (stream == nullptr) {
    std::cerr << "Can't write " << output.getFullPathName() << "\n";
    return 1;
  }

  WavAudioFormat wav;
  std::unique_ptr<AudioFormatWriter> writer(wav.createWriterFor(stream.get(), options.sample_rate, 2, 24, {}, 0));
  if (writer == nullptr) {
    std::cerr << "Couldn't create WAV writer\n";
    return 1;
  }
  stream.release();  // owned by the writer now

  const int64 note_off_sample = static_cast<int64>(options.length * options.sample_rate);
  const int64 total_samples = note_off_sample + static_cast<int64>(options.tail * options.sample_rate);

  AudioSampleBuffer buffer(2, options.block_size);
  MidiBuffer midi;
  float peak = 0.0f;
  double sum_squares = 0.0;
  bool finite = true;

  auto start = std::chrono::steady_clock::now();
  for (int64 position = 0; position < total_samples; position += options.block_size) {
    int samples = static_cast<int>(std::min<int64>(options.block_size, total_samples - position));
    buffer.setSize(2, samples, false, false, true);
    midi.clear();

    if (position == 0) {
      for (int note : notes)
        midi.addEvent(MidiMessage::noteOn(1, note, 0.8f), 0);
    }
    if (note_off_sample >= position && note_off_sample < position + samples) {
      for (int note : notes)
        midi.addEvent(MidiMessage::noteOff(1, note, 0.5f), static_cast<int>(note_off_sample - position));
    }

    engine.process(buffer, midi);

    for (int channel = 0; channel < 2; ++channel) {
      const float* data = buffer.getReadPointer(channel);
      for (int i = 0; i < samples; ++i) {
        finite = finite && std::isfinite(data[i]);
        peak = std::max(peak, std::abs(data[i]));
        sum_squares += data[i] * data[i];
      }
    }
    writer->writeFromAudioSampleBuffer(buffer, 0, samples);
  }
  auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  writer.reset();

  double rms = total_samples > 0 ? std::sqrt(sum_squares / (2.0 * total_samples)) : 0.0;
  double audio_seconds = total_samples / options.sample_rate;
  std::cerr << "Wrote " << output.getFullPathName() << ": " << audio_seconds << " s, peak "
            << peak << ", rms " << rms << ", rendered at " << (elapsed > 0.0 ? audio_seconds / elapsed : 0.0)
            << "x real time\n";

  if (!finite) {
    std::cerr << "Error: output contains NaN or infinity\n";
    return 3;
  }
  return 0;
}
