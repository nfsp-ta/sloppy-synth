/* sloppy-synth engine tests: patch import, bank import and rendering.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
#include "JuceHeader.h"
#include "patch_library.h"
#include "sloppy_engine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>

namespace {
  int failures = 0;
  int checks = 0;

  #define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
      ++failures; \
      std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << ": " #condition "\n"; \
    } \
  } while (false)

  File fixture(const String& name) {
    return File(SLOPPY_FIXTURES_DIR).getChildFile(name);
  }

  struct RenderStats {
    float peak = 0.0f;
    double rms = 0.0;
    bool finite = true;
  };

  // Plays `note` for `seconds`, through an optional extra callback that can
  // inject MIDI before each block.
  RenderStats render(sloppy::Engine& engine, int note, double seconds,
                     std::function<void(MidiBuffer&, int64)> add_midi = nullptr) {
    static constexpr int kBlock = 256;
    const double rate = engine.getSampleRateHz();
    const int64 total = static_cast<int64>(seconds * rate);

    AudioSampleBuffer buffer(2, kBlock);
    MidiBuffer midi;
    RenderStats stats;
    double sum = 0.0;

    for (int64 position = 0; position < total; position += kBlock) {
      midi.clear();
      if (note >= 0 && position == 0)
        midi.addEvent(MidiMessage::noteOn(1, note, 0.8f), 0);
      if (add_midi)
        add_midi(midi, position);

      engine.process(buffer, midi);
      for (int channel = 0; channel < 2; ++channel) {
        const float* data = buffer.getReadPointer(channel);
        for (int i = 0; i < kBlock; ++i) {
          stats.finite = stats.finite && std::isfinite(data[i]);
          stats.peak = std::max(stats.peak, std::abs(data[i]));
          sum += data[i] * data[i];
        }
      }
    }
    stats.rms = std::sqrt(sum / (2.0 * total));
    return stats;
  }

  void testInitPatchMakesSound() {
    sloppy::Engine engine;
    engine.prepare(48000.0, 256);
    RenderStats stats = render(engine, 60, 0.5);
    CHECK(stats.finite);
    CHECK(stats.peak > 0.05f);
    CHECK(stats.peak < 2.0f);
  }

  void testSilentWithoutNotes() {
    sloppy::Engine engine;
    engine.prepare(44100.0, 256);
    RenderStats stats = render(engine, -1, 0.3);
    CHECK(stats.finite);
    CHECK(stats.peak < 1e-4f);
  }

  void testLoadPatch() {
    sloppy::Engine engine;
    std::string error;
    CHECK(engine.loadPatch(fixture("test_bass.vital"), error));
    CHECK(error.empty());

    CHECK(engine.getParameter("filter_1_on") == 1.0f);
    CHECK(engine.getParameter("filter_1_model") == 1.0f);
    CHECK(std::abs(engine.getParameter("filter_1_cutoff") - 70.0f) < 1e-4f);
    CHECK(engine.getParameter("osc_1_unison_voices") == 4.0f);
    CHECK(engine.getParameter("reverb_on") == 1.0f);
    CHECK(engine.getNumModulations("filter_1_cutoff") == 1);
    CHECK(engine.getNumModulations("osc_1_wave_frame") == 1);
    CHECK(engine.getNumModulations("filter_1_resonance") == 1);
    CHECK(engine.getAuthor() == "sloppy-synth tests");
    CHECK(engine.getStyle() == "Bass");

    engine.prepare(44100.0, 256);
    RenderStats stats = render(engine, 36, 1.0);
    CHECK(stats.finite);
    CHECK(stats.rms > 0.01);
  }

  void testLoadOlderPatch() {
    sloppy::Engine engine;
    std::string error;
    CHECK(engine.loadPatch(fixture("test_bass_v1_0.vital"), error));
    CHECK(engine.getParameter("filter_1_on") == 1.0f);
    CHECK(engine.getNumModulations("filter_1_cutoff") == 1);
  }

  void testLoadNewerMinorPatch() {
    sloppy::Engine engine;
    std::string error;
    CHECK(engine.loadPatch(fixture("from_vital_1_6.vital"), error));
    CHECK(engine.getPatchVersion() == "1.6.4");
    CHECK(engine.getParameter("osc_1_spectral_morph_type") == 0.0f);
    CHECK(engine.getParameter("filter_1_on") == 1.0f);
    CHECK(engine.getNumModulations("filter_1_cutoff") == 1);

    std::string all;
    for (const std::string& warning : engine.getLoadWarnings())
      all += warning + "\n";
    std::cerr << all;
    CHECK(engine.getLoadWarnings().size() == 4);
    CHECK(all.find("Vital 1.6.4") != std::string::npos);
    CHECK(all.find("Morph Type") != std::string::npos);
    CHECK(all.find("osc_1_made_up_seed") != std::string::npos);
    CHECK(all.find("lfos/made_up_ramp") != std::string::npos);
    CHECK(all.find("made_up_source to filter_1_cutoff") != std::string::npos);

    engine.prepare(44100.0, 256);
    RenderStats stats = render(engine, 36, 0.5);
    CHECK(stats.finite);
    CHECK(stats.rms > 0.01);

    // Patches this engine fully understands load without warnings.
    CHECK(engine.loadPatch(fixture("test_bass_v1_0.vital"), error));
    CHECK(engine.getLoadWarnings().empty());
    CHECK(engine.loadPatch(fixture("test_bass.vital"), error));
    CHECK(engine.getLoadWarnings().empty());
  }

  void testRejectFuturePatch() {
    sloppy::Engine engine;
    std::string error;
    CHECK(!engine.loadPatch(fixture("from_the_future.vital"), error));
    CHECK(!error.empty());
  }

  void testRejectCorruptPatch() {
    sloppy::Engine engine;
    std::string error;
    CHECK(!engine.loadPatchFromString("{ not json", error));
    CHECK(!error.empty());
    CHECK(!engine.loadPatch(fixture("does_not_exist.vital"), error));
  }

  void testSaveRoundTrip() {
    TemporaryFile temp(".vital");
    sloppy::Engine original;
    std::string error;
    CHECK(original.loadPatch(fixture("test_bass.vital"), error));
    original.setParameter("filter_1_cutoff", 42.0f);
    CHECK(original.savePatch(temp.getFile()));

    sloppy::Engine reloaded;
    CHECK(reloaded.loadPatch(temp.getFile(), error));
    int mismatches = 0;
    for (const std::string& name : original.getParameterNames()) {
      if (std::abs(original.getParameter(name) - reloaded.getParameter(name)) > 1e-4f) {
        std::cerr << "  mismatch " << name << ": " << original.getParameter(name)
                  << " vs " << reloaded.getParameter(name) << "\n";
        ++mismatches;
      }
    }
    CHECK(mismatches == 0);
    CHECK(reloaded.getParameter("filter_1_cutoff") == 42.0f);
    CHECK(reloaded.getNumModulations("filter_1_cutoff") == 1);
  }

  void testMidiFromAnotherThreadPlays() {
    sloppy::Engine engine;
    engine.prepare(44100.0, 256);
    engine.addMidiMessage(MidiMessage::noteOn(1, 60, 0.9f));
    RenderStats stats = render(engine, -1, 0.3);
    CHECK(stats.peak > 0.05f);

    engine.allNotesOff();
    RenderStats after = render(engine, -1, 0.3);
    CHECK(after.peak < 1e-3f);
  }

  void testSetParameterChangesSound() {
    sloppy::Engine engine;
    engine.prepare(44100.0, 256);
    engine.setParameter("osc_1_on", 0.0f);
    RenderStats silent = render(engine, 60, 0.3);
    CHECK(silent.peak < 1e-3f);
    CHECK(engine.getParameter("osc_1_on") == 0.0f);
    CHECK(engine.hasParameter("osc_1_level"));
    CHECK(!engine.hasParameter("not_a_parameter"));
  }

  void testBankImport() {
    TemporaryFile temp_dir;
    File root = temp_dir.getFile();
    sloppy::PatchLibrary library(root);

    std::string bank_name, error;
    CHECK(library.importBank(fixture("Test Bank.vitalbank"), bank_name, error));
    CHECK(bank_name == "Test Bank");

    std::vector<sloppy::PatchEntry> patches = library.listPatches();
    CHECK(patches.size() == 3);
    int bass = 0, pads = 0;
    for (const sloppy::PatchEntry& patch : patches) {
      CHECK(patch.bank == "Test Bank");
      bass += patch.category == "Bass";
      pads += patch.category == "Pads";

      sloppy::Engine engine;
      std::string load_error;
      CHECK(engine.loadPatch(patch.file, load_error));
      RenderStats stats = render(engine, 48, 0.3);
      CHECK(stats.finite);
    }
    CHECK(bass == 1);
    CHECK(pads == 1);

    File copied;
    CHECK(library.importPatch(fixture("test_bass.vital"), copied, error));
    CHECK(copied.existsAsFile());
    CHECK(library.listPatches().size() == 4);

    root.deleteRecursively();
  }

  void testNestedFolders() {
    TemporaryFile temp_dir;
    File root = temp_dir.getFile();
    File nested = root.getChildFile("My Bank/Presets/Leads/Bright/lead.vital");
    File loose = root.getChildFile("Loose/one.vital");
    CHECK(nested.getParentDirectory().createDirectory());
    CHECK(loose.getParentDirectory().createDirectory());
    CHECK(fixture("test_bass.vital").copyFileTo(nested));
    CHECK(fixture("test_bass.vital").copyFileTo(loose));

    sloppy::PatchLibrary library(root);
    std::vector<sloppy::PatchEntry> patches = library.listPatches();
    CHECK(patches.size() == 2);
    for (const sloppy::PatchEntry& patch : patches) {
      if (patch.name == "lead") {
        CHECK(patch.bank == "My Bank");
        CHECK(patch.category == "Leads");
        CHECK((patch.folders == std::vector<std::string> { "Leads", "Bright" }));
      }
      else {
        CHECK(patch.bank == "Loose");
        CHECK(patch.folders.empty());
      }
    }
    root.deleteRecursively();
  }

  void testModulationMatrix() {
    sloppy::Engine engine;
    std::vector<std::string> sources = engine.getModulationSources();
    std::vector<std::string> destinations = engine.getModulationDestinations();
    auto contains = [](const std::vector<std::string>& list, const std::string& name) {
      return std::find(list.begin(), list.end(), name) != list.end();
    };
    CHECK(contains(sources, "lfo_1"));
    CHECK(contains(sources, "env_2"));
    CHECK(contains(sources, "macro_control_1"));
    CHECK(contains(destinations, "filter_1_cutoff"));
    CHECK(contains(destinations, "osc_1_level"));
    CHECK(engine.getModulations().empty());

    RenderStats plain = render(engine, 60, 0.3);

    // Macro 1 fully up, routed to pull oscillator 1's level all the way down.
    int generation = engine.getModulationGeneration();
    std::string error;
    CHECK(engine.addModulation("macro_control_1", "osc_1_level", -1.0f, error));
    CHECK(engine.getModulationGeneration() != generation);
    std::vector<sloppy::Modulation> modulations = engine.getModulations();
    CHECK(modulations.size() == 1);
    CHECK(modulations[0].source == "macro_control_1");
    CHECK(modulations[0].destination == "osc_1_level");
    std::string amount = "modulation_" + std::to_string(modulations[0].slot) + "_amount";
    CHECK(engine.getParameter(amount) == -1.0f);
    engine.setParameter("macro_control_1", 1.0f);
    RenderStats modulated = render(engine, 60, 0.3);
    CHECK(modulated.rms < plain.rms * 0.5);

    // Adding it again only changes the amount.
    CHECK(engine.addModulation("macro_control_1", "osc_1_level", 0.25f, error));
    CHECK(engine.getModulations().size() == 1);
    CHECK(engine.getParameter(amount) == 0.25f);

    CHECK(!engine.addModulation("not_a_source", "osc_1_level", 0.5f, error));
    CHECK(!engine.addModulation("lfo_1", "not_a_parameter", 0.5f, error));

    CHECK(engine.removeModulation("macro_control_1", "osc_1_level"));
    CHECK(engine.getModulations().empty());
    CHECK(!engine.removeModulation("macro_control_1", "osc_1_level"));
  }

  // Builds a zip from (path in zip, file) pairs.
  File makeZip(const File& folder, const String& name, const std::vector<std::pair<String, File>>& entries) {
    ZipFile::Builder builder;
    for (const auto& entry : entries)
      builder.addFile(entry.second, 9, entry.first);
    File zip_file = folder.getChildFile(name);
    zip_file.getParentDirectory().createDirectory();
    FileOutputStream output(zip_file);
    builder.writeToStream(output, nullptr);
    return zip_file;
  }

  void testZipImport() {
    TemporaryFile temp_dir;
    File work = temp_dir.getFile();
    File readme = work.getChildFile("readme.txt");
    work.createDirectory();
    readme.replaceWithText("not a patch");
    std::string summary, error;

    // Loose presets in a wrapping folder, with macOS clutter and a text file.
    {
      sloppy::PatchLibrary library(work.getChildFile("lib1"));
      File zip = makeZip(work, "loose.zip", {
        { "My Stuff/test_bass.vital", fixture("test_bass.vital") },
        { "My Stuff/Leads/Old.vital", fixture("test_bass_v1_0.vital") },
        { "__MACOSX/My Stuff/._test_bass.vital", readme },
        { "My Stuff/readme.txt", readme },
      });
      CHECK(library.importZip(zip, "Cool Presets.zip", summary, error));
      CHECK(summary == "Imported 2 presets");
      std::vector<sloppy::PatchEntry> patches = library.listPatches();
      CHECK(patches.size() == 2);
      for (const sloppy::PatchEntry& patch : patches) {
        CHECK(patch.bank == "Cool Presets");
        sloppy::Engine engine;
        std::string load_error;
        CHECK(engine.loadPatch(patch.file, load_error));
      }
      CHECK(patches.size() == 2 && patches[1].folders == std::vector<std::string>({ "Leads" }));
    }

    // Banks inside a zip.
    {
      sloppy::PatchLibrary library(work.getChildFile("lib2"));
      File zip = makeZip(work, "banks.zip", { { "Downloads/Test Bank.vitalbank", fixture("Test Bank.vitalbank") } });
      CHECK(library.importZip(zip, "banks.zip", summary, error));
      CHECK(summary == "Imported 1 bank");
      std::vector<sloppy::PatchEntry> patches = library.listPatches();
      CHECK(patches.size() == 3);
      CHECK(!patches.empty() && patches[0].bank == "Test Bank");
    }

    // A zip laid out like a bank.
    {
      sloppy::PatchLibrary library(work.getChildFile("lib3"));
      File zip = makeZip(work, "laidout.zip", { { "Zipped Bank/Presets/Bass/Sub.vital", fixture("test_bass.vital") } });
      CHECK(library.importZip(zip, "laidout.zip", summary, error));
      CHECK(summary == "Imported 1 bank");
      std::vector<sloppy::PatchEntry> patches = library.listPatches();
      CHECK(patches.size() == 1);
      CHECK(!patches.empty() && patches[0].bank == "Zipped Bank" && patches[0].category == "Bass");
    }

    // Nothing Vital in it, and not a zip at all.
    {
      sloppy::PatchLibrary library(work.getChildFile("lib4"));
      File zip = makeZip(work, "other.zip", { { "readme.txt", readme } });
      CHECK(!library.importZip(zip, "other.zip", summary, error));
      CHECK(!error.empty());
      CHECK(!library.importZip(readme, "readme.zip", summary, error));
      CHECK(library.listPatches().empty());
    }

    // Paths escaping the library are refused, like in banks.
    {
      sloppy::PatchLibrary library(work.getChildFile("lib5"));
      File zip = makeZip(work, "slip.zip", { { "../escaped.vital", fixture("test_bass.vital") } });
      CHECK(!library.importZip(zip, "slip.zip", summary, error));
      CHECK(!work.getChildFile("escaped.vital").exists());
    }

    work.deleteRecursively();
  }

  void testBankImportRejectsZipSlip() {
    TemporaryFile temp_dir;
    File root = temp_dir.getFile().getChildFile("library");
    sloppy::PatchLibrary library(root);

    std::string bank_name, error;
    CHECK(!library.importBank(fixture("zip_slip.vitalbank"), bank_name, error));
    CHECK(!error.empty());
    CHECK(!root.getSiblingFile("escaped.txt").exists());
    CHECK(library.listPatches().empty());
    temp_dir.getFile().deleteRecursively();
  }
}

int main() {
  struct Test { const char* name; void (*run)(); };
  const Test tests[] = {
    { "init patch makes sound", testInitPatchMakesSound },
    { "silent without notes", testSilentWithoutNotes },
    { "load patch", testLoadPatch },
    { "load older patch", testLoadOlderPatch },
    { "load patch from a newer 1.x release", testLoadNewerMinorPatch },
    { "reject patch from a newer major version", testRejectFuturePatch },
    { "reject corrupt patch", testRejectCorruptPatch },
    { "save round trip", testSaveRoundTrip },
    { "MIDI from another thread plays", testMidiFromAnotherThreadPlays },
    { "setParameter changes sound", testSetParameterChangesSound },
    { "bank import", testBankImport },
    { "nested patch folders", testNestedFolders },
    { "modulation matrix", testModulationMatrix },
    { "bank import rejects zip slip", testBankImportRejectsZipSlip },
    { "zip import", testZipImport },
  };

  for (const Test& test : tests) {
    int before = failures;
    test.run();
    std::cout << (failures == before ? "ok   " : "FAIL ") << test.name << "\n";
  }

  std::cout << checks << " checks, " << failures << " failed\n";
  return failures == 0 ? 0 : 1;
}
