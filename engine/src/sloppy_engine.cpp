/* sloppy-synth: a headless front end for the Vital synthesis engine.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
#include "sloppy_engine.h"

#include <algorithm>

#include "json/json.h"
#include "load_save.h"
#include "modulation_connection_processor.h"
#include "sound_engine.h"
#include "synth_parameters.h"

namespace sloppy {

  // Defined in common_unity.cpp, next to Vital's parameter table.
  size_t valueNameCount(const std::string* lookup);

  Engine::Engine() {
    for (int i = 0; i < MacroMidiMap::kNumMacros; ++i)
      macro_controls_[i] = controls_["macro_control_" + std::to_string(i + 1)];
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
    unmapped_midi_.ensureSize(4096);
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

    applyMacroMidi(midi);
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

  void Engine::applyMacroMidi(MidiBuffer& midi) {
    bool mapped = false;
    for (const MidiMessageMetadata event : midi) {
      const uint8* data = event.data;
      if (event.numBytes < 3 || (data[0] & 0xf0) != 0xb0)
        continue;
      int channel = (data[0] & 0x0f) + 1;
      int macro = macro_midi_.match(channel, data[1]);
      for (; macro >= 0; macro = macro_midi_.match(channel, data[1], macro + 1)) {
        macro_controls_[macro]->set(data[2] / 127.0f);
        mapped = true;
      }
    }
    if (!mapped)
      return;

    unmapped_midi_.clear();
    for (const MidiMessageMetadata event : midi) {
      const uint8* data = event.data;
      bool controller = event.numBytes >= 3 && (data[0] & 0xf0) == 0xb0;
      if (!controller || macro_midi_.match((data[0] & 0x0f) + 1, data[1]) < 0)
        unmapped_midi_.addEvent(data, event.numBytes, event.samplePosition);
    }
    midi.swapWith(unmapped_midi_);
  }

  void Engine::setSettingsFile(const File& file) {
    const ScopedLock lock(settings_lock_);
    settings_file_ = file;
    if (!file.existsAsFile())
      return;
    try {
      json settings = json::parse(file.loadFileAsString().toStdString());
      std::string error;
      if (settings.count("macro_midi") && !macro_midi_.fromJson(settings["macro_midi"].dump(), error))
        DBG("Ignoring settings: " + error);
    }
    catch (const json::exception&) {
      DBG("Settings file is corrupted, using defaults.");
    }
  }

  bool Engine::saveSettings() {
    const ScopedLock lock(settings_lock_);
    if (settings_file_ == File())
      return true;
    json settings = json::object();
    if (settings_file_.existsAsFile()) {
      // Keep whatever else is in there.
      try {
        settings = json::parse(settings_file_.loadFileAsString().toStdString());
        if (!settings.is_object())
          settings = json::object();
      }
      catch (const json::exception&) { }
    }
    settings["macro_midi"] = json::parse(macro_midi_.toJson());
    settings_file_.getParentDirectory().createDirectory();
    return settings_file_.replaceWithText(String(settings.dump(2)) + "\n");
  }

  bool Engine::loadPatch(const File& file, std::string& error) {
    if (!file.existsAsFile()) {
      error = "Patch file not found: " + file.getFullPathName().toStdString();
      return false;
    }
    if (!loadFromFile(file, error)) {
      if (error.empty())
        error = "Couldn't load patch.";
      return false;
    }
    prepare(sample_rate_, vital::kMaxBufferSize);
    ++modulation_generation_;
    return true;
  }

  bool Engine::loadPatchFromString(const std::string& patch_json, std::string& error) {
    try {
      json state = json::parse(patch_json, nullptr);
      if (!loadFromJson(state)) {
        error = "Preset was created with a newer version.";
        return false;
      }
    }
    catch (const json::exception& e) {
      error = std::string("Preset file is corrupted: ") + e.what();
      return false;
    }
    prepare(sample_rate_, vital::kMaxBufferSize);
    ++modulation_generation_;
    return true;
  }

  void Engine::loadInitPatch() {
    loadInitPreset();
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
