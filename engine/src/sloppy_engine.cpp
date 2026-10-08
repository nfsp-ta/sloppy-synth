/* sloppy-synth: a headless front end for the Vital synthesis engine.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
#include "sloppy_engine.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "json/json.h"
#include "load_save.h"
#include "modulation_connection_processor.h"
#include "sound_engine.h"
#include "synth_constants.h"
#include "formant_filter.h"
#include "synth_oscillator.h"
#include "synth_parameters.h"

namespace sloppy {

  // Defined in common_unity.cpp, next to Vital's parameter table.
  size_t valueNameCount(const std::string* lookup);

  Engine::Engine() {
    loadInitPatch();
    prepare(sample_rate_, vital::kMaxBufferSize);
  }

  Engine::~Engine() = default;

  void Engine::prepare(double sample_rate, int max_block_size) {
    ScopedLock lock(getCriticalSection());
    sample_rate_ = sample_rate;
    engine_->setSampleRate(static_cast<int>(sample_rate));
    engine_->updateAllModulationSwitches();
    midi_manager_->setSampleRate(sample_rate);
    incoming_midi_.ensureSize(4096);
    pending_midi_.ensureSize(4096);
    ignoreUnused(max_block_size);
  }

  void Engine::process(AudioSampleBuffer& buffer, MidiBuffer& midi) {
    int total_samples = buffer.getNumSamples();
    int num_channels = std::min(buffer.getNumChannels(), 2);

    ScopedTryLock lock(getCriticalSection());
    if (!lock.isLocked() || total_samples == 0) {
      buffer.clear();
      return;
    }

    // Events from addMidiMessage() (device threads) join the host's events.
    {
      SpinLock::ScopedTryLockType midi_lock(midi_lock_);
      if (midi_lock.isLocked() && !incoming_midi_.isEmpty())
        pending_midi_.swapWith(incoming_midi_);
    }
    if (!pending_midi_.isEmpty()) {
      midi.addEvents(pending_midi_, 0, -1, 0);
      pending_midi_.clear();
    }

    processModulationChanges();
    processKeyboardEvents(midi, total_samples);

    double sample_time = 1.0 / sample_rate_;
    for (int sample_offset = 0; sample_offset < total_samples;) {
      int num_samples = std::min<int>(total_samples - sample_offset, vital::kMaxBufferSize);

      engine_->correctToTime(seconds_time_);
      processMidi(midi, sample_offset, sample_offset + num_samples);
      processAudio(&buffer, num_channels, num_samples, sample_offset);

      seconds_time_ += num_samples * sample_time;
      sample_offset += num_samples;
    }

    if (buffer.getNumChannels() > 2) {
      for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
        buffer.clear(channel, 0, total_samples);
    }
  }

  namespace {
    // Vital 1.5 added "Octave + 7" as the fifth unison stack style, moving
    // every later style up by one, and "Sub Harmonics" at the end. Patches
    // from 1.5 on use that numbering; this engine still uses 1.0's.
    constexpr const char* kStackRenumberedVersion = "1.5.0";
    constexpr int kOctavePlus7Stack = 4;
    constexpr int kSubHarmonicsStack = 12;

    std::string stackStyleName(int oscillator) {
      return "osc_" + std::to_string(oscillator) + "_stack_style";
    }

    // Converts stack styles from Vital 1.5's numbering to this engine's.
    void stackStylesFromNewer(json& settings, std::vector<std::string>& warnings) {
      for (int i = 1; i <= vital::kNumOscillators; ++i) {
        std::string name = stackStyleName(i);
        if (!settings.count(name) || !settings[name].is_number())
          continue;
        int style = static_cast<int>(std::round(settings[name].get<float>()));
        std::string label = "Oscillator " + std::to_string(i) + " Stack Style";
        if (style == kOctavePlus7Stack) {
          style = vital::SynthOscillator::kOctave;
          warnings.push_back(label + " is Octave + 7, which this engine doesn't have yet, so it plays as Octave.");
        }
        else if (style >= kSubHarmonicsStack) {
          style = vital::SynthOscillator::kNormal;
          warnings.push_back(label + " is Sub Harmonics, which this engine doesn't have yet, so it plays as Unison.");
        }
        else if (style > kOctavePlus7Stack) {
          style -= 1;
        }
        settings[name] = static_cast<float>(style);
      }
    }

    void stackStylesToNewer(json& settings) {
      for (int i = 1; i <= vital::kNumOscillators; ++i) {
        std::string name = stackStyleName(i);
        if (!settings.count(name) || !settings[name].is_number())
          continue;
        int style = static_cast<int>(std::round(settings[name].get<float>()));
        if (style >= kOctavePlus7Stack)
          settings[name] = static_cast<float>(style + 1);
      }
    }
  }

  bool Engine::loadPatch(const File& file, std::string& error) {
    if (!file.existsAsFile()) {
      error = "Patch file not found: " + file.getFullPathName().toStdString();
      return false;
    }
    json state;
    try {
      state = json::parse(file.loadFileAsString().toStdString(), nullptr);
    }
    catch (const json::exception&) {
      error = "Preset file is corrupted.";
      return false;
    }
    if (!loadJson(std::move(state), error))
      return false;

    active_file_ = file;
    setPresetName(file.getFileNameWithoutExtension());
    return true;
  }

  bool Engine::loadPatchFromString(const std::string& patch_json, std::string& error) {
    json state;
    try {
      state = json::parse(patch_json, nullptr);
    }
    catch (const json::exception& e) {
      error = std::string("Preset file is corrupted: ") + e.what();
      return false;
    }
    return loadJson(std::move(state), error);
  }

  bool Engine::loadJson(json state, std::string& error) {
    if (!state.is_object() || (state.count("synth_version") && !state["synth_version"].is_string())) {
      error = "Preset file is corrupted.";
      return false;
    }
    std::string version = state.value("synth_version", std::string("0.0.0"));
    // Vital refuses any patch whose major.minor is newer than its own. Newer
    // 1.x releases keep the same format and only add settings, so let those
    // through, posing as our own version so Vital doesn't try to upgrade them.
    String version_string(version);
    String major = version_string.upToFirstOccurrenceOf(".", false, true);
    String our_major = String(ProjectInfo::versionString).upToFirstOccurrenceOf(".", false, true);
    if (major.containsOnly("0123456789") && major.getIntValue() > our_major.getIntValue()) {
      error = "Preset was created with Vital " + version + ", which is too new to load.";
      return false;
    }
    std::vector<std::string> warnings;
    if (LoadSave::compareVersionStrings(version, kStackRenumberedVersion) >= 0 && state.count("settings"))
      stackStylesFromNewer(state["settings"], warnings);
    if (LoadSave::compareFeatureVersionStrings(version, ProjectInfo::versionString) > 0)
      state["synth_version"] = ProjectInfo::versionString;

    {
      ScopedLock lock(getCriticalSection());
      try {
        if (!loadFromJson(state)) {
          error = "Preset was created with a newer version.";
          return false;
        }
        checkUnsupported(state, version, warnings);
        if (!warnings.empty())
          warnings.insert(warnings.begin(), "Made with Vital " + version + ", so it may not sound the same here.");
      }
      catch (const json::exception& e) {
        error = std::string("Preset file is corrupted: ") + e.what();
        return false;
      }
    }

    {
      std::lock_guard<std::mutex> lock(load_info_lock_);
      load_warnings_ = warnings;
      patch_version_ = version;
    }
    prepare(sample_rate_, vital::kMaxBufferSize);
    ++modulation_generation_;
    return true;
  }

  namespace {
    // The Vital release this engine's code matches. ProjectInfo::versionString
    // says 1.5.5 so that Vital 1.5 patches pass the version check, but the
    // code underneath is the public 1.0.6 source.
    constexpr const char* kEngineVitalVersion = "1.0.6";
    constexpr size_t kMaxNamesListed = 6;

    // Collects the keys in `theirs` that `ours` doesn't have, walking objects
    // and arrays in step. Array positions are dropped from the paths, so the
    // same setting on three oscillators is reported once.
    void findUnknownKeys(const json& theirs, const json& ours, const std::string& path,
                         std::set<std::string>& unknown) {
      if (theirs.is_object() && ours.is_object()) {
        for (auto item = theirs.begin(); item != theirs.end(); ++item) {
          std::string child = path.empty() ? item.key() : path + "/" + item.key();
          auto found = ours.find(item.key());
          if (found == ours.end())
            unknown.insert(child);
          else
            findUnknownKeys(item.value(), *found, child, unknown);
        }
      }
      else if (theirs.is_array() && ours.is_array()) {
        size_t count = std::min(theirs.size(), ours.size());
        for (size_t i = 0; i < count; ++i)
          findUnknownKeys(theirs[i], ours[i], path, unknown);
      }
    }

    bool startsWith(const std::string& text, const std::string& prefix) {
      return text.compare(0, prefix.size(), prefix) == 0;
    }

    bool endsWith(const std::string& text, const std::string& suffix) {
      return text.size() >= suffix.size() &&
             text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // Settings that Vital 1.5 and 1.6 write into every patch. At these
    // values (their defaults in 1.6.4) they change nothing, so they aren't
    // worth a warning.
    bool newerSettingAtDefault(const std::string& name, const json& value) {
      // Curves for the spectral filter, flanger and phaser warps. Patches
      // that use those warps get a warning for the warp type instead.
      if (name == "custom_warps")
        return true;
      if (name == "random_values") {
        if (!value.is_array())
          return false;
        for (const json& oscillator : value) {
          if (!oscillator.is_object() || oscillator.value("seed", 4) != 4)
            return false;
        }
        return true;
      }
      if (!value.is_number())
        return false;
      float number = value.get<float>();
      if (startsWith(name, "modulation_") && (endsWith(name, "_ramp_up") || endsWith(name, "_ramp_down")))
        return number == -10.0f;
      if (startsWith(name, "osc_") && endsWith(name, "_spectral_morph_phase"))
        return number == 0.5f;
      return false;
    }

    template <class Container>
    std::string listNames(const Container& names) {
      std::string list;
      size_t listed = 0;
      for (const std::string& name : names) {
        if (listed == kMaxNamesListed) {
          list += " and " + std::to_string(names.size() - listed) + " more";
          break;
        }
        list += (listed ? ", " : "") + name;
        ++listed;
      }
      return list;
    }
  }

  // Called right after loading, with the patch as it was handed to Vital.
  // Resets choices this engine doesn't have and lists what was left out.
  void Engine::checkUnsupported(const json& original, const std::string& version,
                                std::vector<std::string>& warnings) {
    // A choice past the end of this engine's list (a newer filter model or
    // spectral warp, say) would index past the end of its tables.
    for (auto& control : controls_) {
      if (!vital::Parameters::isParameter(control.first))
        continue;
      const vital::ValueDetails& details = vital::Parameters::getDetails(control.first);
      float value = control.second->value();
      if (details.value_scale != vital::ValueDetails::kIndexed ||
          (value >= details.min - 0.5f && value <= details.max + 0.5f))
        continue;
      control.second->set(details.default_value);
      // Display-only choices, such as Vital 1.6's mono spectrum view.
      if (control.first.find("view_") != std::string::npos)
        continue;
      std::string warning = details.display_name + " uses an option this engine doesn't have";
      int default_index = static_cast<int>(details.default_value - details.min);
      if (details.string_lookup && default_index >= 0 &&
          static_cast<size_t>(default_index) < valueNameCount(details.string_lookup))
        warning += ", so it was set to " + details.string_lookup[default_index];
      warnings.push_back(warning + ".");
    }

    // The formant filter's third style (vocal tract) is an empty stub in the
    // public source and outputs silence; Vital 1.6.4 makes sound with it.
    for (const std::string& filter : { "filter_1", "filter_2", "filter_fx" }) {
      auto model = controls_.find(filter + "_model");
      auto style = controls_.find(filter + "_style");
      if (model == controls_.end() || style == controls_.end() ||
          std::round(model->second->value()) != vital::constants::kFormant ||
          std::round(style->second->value()) < vital::FormantFilter::kNumFormantStyles)
        continue;
      style->second->set(0.0f);
      auto on = controls_.find(filter + "_on");
      if (on != controls_.end() && on->second->value() > 0.5f) {
        std::string label = vital::Parameters::getDetails(filter + "_style").display_name;
        warnings.push_back(label + " uses a formant style this engine doesn't have, so it was set to AOIE.");
      }
    }

    // Older patches go through Vital's upgrade code, which renames settings,
    // and can't have anything this engine lacks.
    if (LoadSave::compareVersionStrings(version, kEngineVitalVersion) <= 0 || !original.count("settings"))
      return;

    const json& settings = original["settings"];
    json ours = saveToJson()["settings"];
    std::set<std::string> unknown;
    std::vector<std::string> dropped;
    for (auto item = settings.begin(); item != settings.end(); ++item) {
      if (item.key() == "modulations")
        continue;
      auto found = ours.find(item.key());
      if (found == ours.end()) {
        if (!newerSettingAtDefault(item.key(), item.value()))
          unknown.insert(item.key());
      }
      else
        findUnknownKeys(item.value(), *found, item.key(), unknown);
    }

    if (settings.count("modulations") && settings["modulations"].is_array()) {
      const json& modulations = settings["modulations"];
      const json& our_modulations = ours["modulations"];
      for (size_t i = 0; i < modulations.size(); ++i) {
        const json& modulation = modulations[i];
        std::string source = modulation.value("source", std::string());
        std::string destination = modulation.value("destination", std::string());
        if (source.empty() || destination.empty())
          continue;
        if (engine_->getModulationSource(source) == nullptr ||
            engine_->getMonoModulationDestination(destination) == nullptr) {
          dropped.push_back(source + " to " + destination);
        }
        else if (modulation.count("line_mapping") && i < our_modulations.size() &&
                 our_modulations[i].count("line_mapping")) {
          findUnknownKeys(modulation["line_mapping"], our_modulations[i]["line_mapping"],
                          "modulations/line_mapping", unknown);
        }
      }
    }

    if (!unknown.empty()) {
      warnings.push_back("Ignored " + std::to_string(unknown.size()) +
                         (unknown.size() == 1 ? " setting" : " settings") +
                         " this engine doesn't have: " + listNames(unknown) + ".");
    }
    if (!dropped.empty()) {
      warnings.push_back("Left out " + std::to_string(dropped.size()) +
                         (dropped.size() == 1 ? " modulation" : " modulations") +
                         " this engine can't route: " + listNames(dropped) + ".");
    }
  }

  bool Engine::savePatch(const File& file) {
    File preset = file.withFileExtension(String(vital::kPresetExtension));
    File parent = preset.getParentDirectory();
    if (!parent.exists() && !parent.createDirectory().wasOk())
      return false;

    setPresetName(preset.getFileNameWithoutExtension());
    json state = saveToJson();
    stackStylesToNewer(state["settings"]);
    if (!preset.replaceWithText(state.dump()))
      return false;
    active_file_ = preset;
    return true;
  }

  std::vector<std::string> Engine::getLoadWarnings() const {
    std::lock_guard<std::mutex> lock(load_info_lock_);
    return load_warnings_;
  }

  std::string Engine::getPatchVersion() const {
    std::lock_guard<std::mutex> lock(load_info_lock_);
    return patch_version_;
  }

  void Engine::loadInitPatch() {
    loadInitPreset();
    {
      std::lock_guard<std::mutex> lock(load_info_lock_);
      load_warnings_.clear();
      patch_version_ = ProjectInfo::versionString;
    }
    ++modulation_generation_;
  }

  std::vector<std::string> Engine::getParameterNames() const {
    std::vector<std::string> names;
    names.reserve(controls_.size());
    for (const auto& control : controls_)
      names.push_back(control.first);
    return names;
  }

  bool Engine::hasParameter(const std::string& name) const {
    return controls_.count(name) > 0;
  }

  bool Engine::getParameterInfo(const std::string& name, ParameterInfo& info) {
    if (!hasParameter(name) || !vital::Parameters::isParameter(name))
      return false;

    const vital::ValueDetails& details = vital::Parameters::getDetails(name);
    info.name = name;
    info.display_name = details.display_name;
    info.min = details.min;
    info.max = details.max;
    info.default_value = details.default_value;
    info.value = controls_[name]->value();
    info.value_scale = details.value_scale;
    info.post_offset = details.post_offset;
    info.display_multiply = details.display_multiply;
    info.display_invert = details.display_invert;
    info.units = details.display_units;
    info.options.clear();
    if (details.string_lookup && details.value_scale == vital::ValueDetails::kIndexed) {
      size_t count = std::min<size_t>(static_cast<size_t>(details.max - details.min) + 1,
                                      valueNameCount(details.string_lookup));
      for (size_t i = 0; i < count; ++i)
        info.options.push_back(details.string_lookup[i]);
    }
    return true;
  }

  float Engine::getParameter(const std::string& name) {
    auto control = controls_.find(name);
    if (control == controls_.end())
      return 0.0f;
    return control->second->value();
  }

  void Engine::setParameter(const std::string& name, float value) {
    if (!hasParameter(name))
      return;

    valueChanged(name, value);
    if (name == "mod_wheel")
      engine_->setModWheelAllChannels(value);
    else if (name == "pitch_wheel")
      engine_->setZonedPitchWheel(value, 0, vital::kNumMidiChannels - 1);
  }

  void Engine::addMidiMessage(const MidiMessage& message) {
    SpinLock::ScopedLockType lock(midi_lock_);
    incoming_midi_.addEvent(message, 0);
  }

  std::vector<std::string> Engine::getModulationSources() {
    std::vector<std::string> names;
    for (const auto& source : engine_->getModulationSources())
      names.push_back(source.first);
    std::sort(names.begin(), names.end());
    return names;
  }

  std::vector<std::string> Engine::getModulationDestinations() {
    std::vector<std::string> names;
    for (const auto& destination : engine_->getMonoModulations())
      names.push_back(destination.first);
    std::sort(names.begin(), names.end());
    return names;
  }

  std::vector<Modulation> Engine::getModulations() {
    ScopedLock lock(getCriticalSection());
    std::vector<Modulation> modulations;
    vital::ModulationConnectionBank& bank = getModulationBank();
    for (int i = 0; i < vital::kMaxModulationConnections; ++i) {
      vital::ModulationConnection* connection = bank.atIndex(i);
      if (!connection->source_name.empty() && !connection->destination_name.empty())
        modulations.push_back({ i + 1, connection->source_name, connection->destination_name });
    }
    return modulations;
  }

  bool Engine::addModulation(const std::string& source, const std::string& destination, float amount,
                             std::string& error) {
    if (engine_->getModulationSources().count(source) == 0) {
      error = "Unknown modulation source: " + source;
      return false;
    }
    if (engine_->getMonoModulations().count(destination) == 0) {
      error = "Can't modulate " + destination;
      return false;
    }

    ScopedLock lock(getCriticalSection());
    bool created = connectModulation(source, destination);
    int index = getConnectionIndex(source, destination);
    if (index < 0) {
      if (static_cast<int>(getModulations().size()) >= vital::kMaxModulationConnections)
        error = "All " + std::to_string(vital::kMaxModulationConnections) + " modulation slots are in use.";
      else
        error = "Can't route " + source + " to " + destination + ".";
      return false;
    }
    std::string prefix = "modulation_" + std::to_string(index + 1) + "_";
    if (created) {
      // Same starting point as Vital's editor: a straight line map, no curve.
      getModulationBank().atIndex(index)->modulation_processor->lineMapGenerator()->initLinear();
      valueChanged(prefix + "power", 0.0f);
      valueChanged(prefix + "stereo", 0.0f);
      valueChanged(prefix + "bypass", 0.0f);
    }
    valueChanged(prefix + "amount", jlimit(-1.0f, 1.0f, amount));
    ++modulation_generation_;
    return true;
  }

  bool Engine::removeModulation(const std::string& source, const std::string& destination) {
    ScopedLock lock(getCriticalSection());
    int index = getConnectionIndex(source, destination);
    if (index < 0)
      return false;
    disconnectModulation(source, destination);
    valueChanged("modulation_" + std::to_string(index + 1) + "_amount", 0.0f);
    ++modulation_generation_;
    return true;
  }

  void Engine::allNotesOff() {
    ScopedLock lock(getCriticalSection());
    engine_->allSoundsOff();
  }

  void Engine::setBpm(float bpm) {
    engine_->setBpm(bpm);
  }

} // namespace sloppy
