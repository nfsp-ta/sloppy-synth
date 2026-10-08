/* sloppy-synth: headless real-time player for the Vital engine.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 *
 * Plays a patch from MIDI through the default audio device (ALSA on Linux).
 * Every MIDI input is opened, and a virtual "sloppy-synth" MIDI port is
 * created so sequencers can connect to it. MIDI program changes step
 * through the patch library, so a box with only a MIDI controller attached
 * can still switch sounds.
 */
#include "JuceHeader.h"
#include "control_server.h"
#include "patch_library.h"
#include "sloppy_engine.h"
#include "tuning.h"

#include <atomic>
#include <csignal>
#include <iostream>

namespace {
  std::atomic<bool> quit_requested { false };

  void handleSignal(int) {
    quit_requested = true;
  }

  void printUsage() {
    std::cout <<
      "Usage: sloppy-synth [patch.vital] [options]\n"
      "\n"
      "Plays a Vital patch from MIDI in real time.\n"
      "\n"
      "Options:\n"
      "  -d, --device NAME       Audio output device (default: system default)\n"
      "  -r, --rate HZ           Sample rate (default: 48000)\n"
      "  -b, --buffer SAMPLES    Audio buffer size (default: 256)\n"
      "  -p, --patch-index N     Start on patch N of the library (see --list-patches)\n"
      "      --library DIR       Patch library folder (default: $SLOPPY_DATA_DIR or ~/.local/share/sloppy-synth)\n"
      "      --import-bank FILE  Unpack a .vitalbank, or a .zip of presets or banks, into\n"
      "                          the library before starting\n"
      "      --list-patches      List library patches with their program numbers, then exit\n"
      "      --list-devices      List audio and MIDI devices, then exit\n"
      "      --audition NOTE     Play a note (MIDI number or name, e.g. C3) for a second\n"
      "                          after starting, to check audio without a controller\n"
      "      --http-port PORT    Port for the web UI (default: 8080, or the next free one)\n"
      "      --http-bind ADDR    Address to serve the web UI on (default: all, so phones\n"
      "                          on the same network can connect; 127.0.0.1 for local only)\n"
      "      --web-root DIR      Web UI files (default: the web folder next to the program)\n"
      "      --no-web            Don't serve the web UI\n"
      "  -h, --help              Show this help\n"
      "\n"
      "MIDI program change N loads library patch N (bank select MSB adds 128 * MSB).\n"
      "Press Ctrl+C to quit.\n";
  }

  File resolve(const String& path) {
    return File::getCurrentWorkingDirectory().getChildFile(path);
  }

  class Player : public AudioIODeviceCallback, public MidiInputCallback, public sloppy::ControlHost,
                 private Timer {
    public:
      Player(sloppy::PatchLibrary& library) : library_(library) {
        patches_ = library_.listPatches();
      }

      ~Player() override {
        stop();
      }

      const std::vector<sloppy::PatchEntry>& patches() const { return patches_; }

      bool loadPatchFile(const File& file) {
        std::string error;
        return loadPatchFile(file, -1, error);
      }

      void loadPatchIndex(int index) {
        std::string error;
        loadPatchIndex(index, error);
      }

      // sloppy::ControlHost, also used for MIDI program changes. Called from
      // the message thread and from web UI connections.
      sloppy::Engine& getEngine() override { return engine_; }
      std::vector<sloppy::PatchEntry> getPatches() override { return patches_; }
      int getCurrentPatchIndex() override { return current_index_; }
      int getPatchGeneration() override { return generation_; }

      bool loadPatchIndex(int index, std::string& error) override {
        if (index < 0 || index >= static_cast<int>(patches_.size())) {
          error = "No patch number " + std::to_string(index) + " (library has " +
                  std::to_string(patches_.size()) + ")";
          std::cerr << error << "\n";
          return false;
        }
        return loadPatchFile(patches_[index].file, index, error);
      }

      void audition(int note) {
        engine_.addMidiMessage(MidiMessage::noteOn(1, note, 0.8f));
        Timer::callAfterDelay(1000, [this, note] {
          engine_.addMidiMessage(MidiMessage::noteOff(1, note, 0.5f));
        });
      }

      bool start(const String& device_name, double sample_rate, int buffer_size) {
        String error = device_manager_.initialiseWithDefaultDevices(0, 2);
        if (error.isNotEmpty()) {
          std::cerr << "Audio device error: " << error << "\n";
          return false;
        }

        AudioDeviceManager::AudioDeviceSetup setup;
        device_manager_.getAudioDeviceSetup(setup);
        if (device_name.isNotEmpty())
          setup.outputDeviceName = device_name;
        setup.sampleRate = sample_rate;
        setup.bufferSize = buffer_size;
        setup.useDefaultOutputChannels = true;
        error = device_manager_.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty()) {
          std::cerr << "Audio device error: " << error << "\n";
          return false;
        }

        device_manager_.addAudioCallback(this);
        AudioIODevice* device = device_manager_.getCurrentAudioDevice();
        if (device == nullptr) {
          std::cerr << "No audio output device available.\n";
          return false;
        }
        std::cout << "Audio: " << device->getName() << " (" << device->getTypeName() << "), "
                  << device->getCurrentSampleRate() << " Hz, " << device->getCurrentBufferSizeSamples()
                  << " samples, " << roundToInt(1000.0 * device->getCurrentBufferSizeSamples() /
                                                device->getCurrentSampleRate()) << " ms\n";

        openMidiInputs();
        startTimer(1000);
        return true;
      }

      void stop() {
        stopTimer();
        midi_inputs_.clear();
        device_manager_.removeAudioCallback(this);
        device_manager_.closeAudioDevice();
      }

      // AudioIODeviceCallback
      void audioDeviceAboutToStart(AudioIODevice* device) override {
        int buffer_size = device->getCurrentBufferSizeSamples();
        engine_.prepare(device->getCurrentSampleRate(), buffer_size);
        midi_buffer_.ensureSize(4096);
      }

      void audioDeviceStopped() override { }

      void audioDeviceIOCallback(const float** input_channel_data, int num_input_channels,
                                 float** output_channel_data, int num_output_channels,
                                 int num_samples) override {
        ignoreUnused(input_channel_data, num_input_channels);
        AudioSampleBuffer buffer(output_channel_data, num_output_channels, num_samples);
        midi_buffer_.clear();
        engine_.process(buffer, midi_buffer_);
      }

      // MidiInputCallback: called on the MIDI thread.
      void handleIncomingMidiMessage(MidiInput* source, const MidiMessage& message) override {
        ignoreUnused(source);
        if (message.isController() && message.getControllerNumber() == 0) {
          bank_msb_ = message.getControllerValue();
          return;
        }
        if (message.isProgramChange()) {
          int index = 128 * bank_msb_.load() + message.getProgramChangeNumber();
          MessageManager::callAsync([this, index] { loadPatchIndex(index); });
          return;
        }
        engine_.addMidiMessage(message);
      }

    private:
      void openMidiInputs() {
        for (const MidiDeviceInfo& info : MidiInput::getAvailableDevices()) {
          if (opened_midi_ids_.contains(info.identifier))
            continue;
          if (auto input = MidiInput::openDevice(info.identifier, this)) {
            input->start();
            std::cout << "MIDI in: " << info.name << "\n";
            opened_midi_ids_.add(info.identifier);
            midi_inputs_.push_back(std::move(input));
          }
        }

        if (virtual_input_ == nullptr) {
          virtual_input_ = MidiInput::createNewDevice("sloppy-synth", this);
          if (virtual_input_ != nullptr) {
            virtual_input_->start();
            std::cout << "MIDI in: virtual port \"sloppy-synth\"\n";
          }
        }
      }

      // Picks up MIDI controllers plugged in after start, and handles quit.
      void timerCallback() override {
        if (quit_requested) {
          MessageManager::getInstance()->stopDispatchLoop();
          return;
        }
        openMidiInputs();
      }

      bool loadPatchFile(const File& file, int index, std::string& error) {
        const ScopedLock lock(load_lock_);
        auto start = Time::getMillisecondCounterHiRes();
        if (!engine_.loadPatch(file, error)) {
          std::cerr << "Couldn't load " << file.getFullPathName() << ": " << error << "\n";
          return false;
        }
        current_index_ = index;
        ++generation_;
        if (index >= 0)
          std::cout << "[" << index << "] ";
        std::cout << "Patch: " << engine_.getPresetName() << " ("
                  << roundToInt(Time::getMillisecondCounterHiRes() - start) << " ms)\n";
        for (const std::string& warning : engine_.getLoadWarnings())
          std::cout << "  " << warning << "\n";
        return true;
      }

      sloppy::PatchLibrary& library_;
      std::vector<sloppy::PatchEntry> patches_;
      CriticalSection load_lock_;
      std::atomic<int> current_index_ { -1 };
      std::atomic<int> generation_ { 0 };
      sloppy::Engine engine_;
      AudioDeviceManager device_manager_;
      MidiBuffer midi_buffer_;
      std::vector<std::unique_ptr<MidiInput>> midi_inputs_;
      std::unique_ptr<MidiInput> virtual_input_;
      StringArray opened_midi_ids_;
      std::atomic<int> bank_msb_ { 0 };
  };

  File findWebRoot() {
    File exe = File::getSpecialLocation(File::currentExecutableFile).getParentDirectory();
    const File candidates[] = {
      exe.getChildFile("web"),
      exe.getChildFile("../share/sloppy-synth/web"),
    };
    for (const File& candidate : candidates) {
      if (candidate.getChildFile("index.html").existsAsFile())
        return candidate;
    }
    return {};
  }

  void printWebAddresses(int port) {
    Array<IPAddress> addresses;
    IPAddress::findAllAddresses(addresses);
    for (const IPAddress& address : addresses) {
      if (address.isIPv6)
        continue;
      std::cout << "Web UI: http://" << address.toString() << ":" << port << "/\n";
    }
  }

  void listDevices() {
    AudioDeviceManager manager;
    manager.initialiseWithDefaultDevices(0, 2);
    for (AudioIODeviceType* type : manager.getAvailableDeviceTypes()) {
      type->scanForDevices();
      for (const String& name : type->getDeviceNames(false))
        std::cout << "audio out (" << type->getTypeName() << "): " << name << "\n";
    }
    for (const MidiDeviceInfo& info : MidiInput::getAvailableDevices())
      std::cout << "midi in: " << info.name << "\n";
  }
}

int main(int argc, const char* argv[]) {
  // Unbuffered, so logs show up promptly under systemd or a pipe.
  std::cout << std::unitbuf;

  String patch_path, device_name, library_path, import_bank, audition_note, http_bind, web_root_path;
  int http_port = 8080;
  bool http_port_given = false;
  bool serve_web = true;
  double sample_rate = 48000.0;
  int buffer_size = 256;
  int patch_index = -1;
  bool list_patches = false;
  bool list_devices = false;

  for (int i = 1; i < argc; ++i) {
    String arg = argv[i];
    auto next = [&]() -> String {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << arg << "\n";
        exit(2);
      }
      return argv[++i];
    };

    if (arg == "-h" || arg == "--help") { printUsage(); return 0; }
    else if (arg == "-d" || arg == "--device") device_name = next();
    else if (arg == "-r" || arg == "--rate") sample_rate = next().getDoubleValue();
    else if (arg == "-b" || arg == "--buffer") buffer_size = next().getIntValue();
    else if (arg == "-p" || arg == "--patch-index") patch_index = next().getIntValue();
    else if (arg == "--library") library_path = next();
    else if (arg == "--import-bank") import_bank = next();
    else if (arg == "--list-patches") list_patches = true;
    else if (arg == "--list-devices") list_devices = true;
    else if (arg == "--audition") audition_note = next();
    else if (arg == "--http-port") { http_port = next().getIntValue(); http_port_given = true; }
    else if (arg == "--http-bind") http_bind = next();
    else if (arg == "--web-root") web_root_path = next();
    else if (arg == "--no-web") serve_web = false;
    else if (arg.startsWith("-")) { std::cerr << "Unknown option " << arg << "\n"; printUsage(); return 2; }
    else patch_path = arg;
  }

  MessageManager::getInstance();

  sloppy::PatchLibrary library(library_path.isNotEmpty() ? resolve(library_path)
                                                         : sloppy::PatchLibrary::defaultRoot());
  if (import_bank.isNotEmpty()) {
    File file = resolve(import_bank);
    std::string bank_name, summary, error;
    bool imported = file.hasFileExtension("zip") ? library.importZip(file, file.getFileName(), summary, error)
                                                 : library.importBank(file, bank_name, error);
    if (!imported) {
      std::cerr << "Import failed: " << error << "\n";
      return 1;
    }
    if (summary.empty())
      summary = "Imported bank \"" + bank_name + "\"";
    std::cout << summary << "\n";
  }

  if (list_devices) {
    listDevices();
    MessageManager::deleteInstance();
    return 0;
  }

  int exit_code = 0;
  {
    Player player(library);

    if (list_patches) {
      int index = 0;
      for (const sloppy::PatchEntry& patch : player.patches())
        std::cout << index++ << "\t" << patch.bank << "\t" << patch.category << "\t" << patch.name << "\n";
    }
    else {
      if (patch_path.isNotEmpty()) {
        if (!player.loadPatchFile(resolve(patch_path)))
          return 1;
      }
      else if (patch_index >= 0)
        player.loadPatchIndex(patch_index);

      std::signal(SIGINT, handleSignal);
      std::signal(SIGTERM, handleSignal);

      std::unique_ptr<sloppy::ControlServer> server;
      if (serve_web) {
        File web_root = web_root_path.isNotEmpty() ? resolve(web_root_path) : findWebRoot();
        if (!web_root.getChildFile("index.html").existsAsFile())
          std::cerr << "Web UI files not found; use --web-root. Serving the control socket only.\n";
        server = std::make_unique<sloppy::ControlServer>(player, web_root);
        std::string error;
        bool started = server->start(http_port, http_bind, error);
        // Without --http-port, step past ports other programs already use.
        for (int port = http_port + 1; !started && !http_port_given && port < http_port + 10; ++port) {
          if (server->start(port, http_bind, error))
            started = true;
        }
        if (started) {
          if (http_port != 0 && server->getPort() != http_port)
            std::cout << "Port " << http_port << " is in use, so the web UI is on port " << server->getPort() << ".\n";
          if (http_bind.isEmpty())
            printWebAddresses(server->getPort());
          else
            std::cout << "Web UI: http://" << http_bind << ":" << server->getPort() << "/\n";
        }
        else {
          std::cerr << "Web UI disabled: " << error << ". Another program may be using it; "
                       "pick a free one with --http-port.\n";
          server.reset();
        }
      }

      if (player.start(device_name, sample_rate, buffer_size)) {
        std::cout << "Playing. Ctrl+C to quit.\n";
        if (audition_note.isNotEmpty()) {
          int note = audition_note.containsOnly("0123456789") ? audition_note.getIntValue()
                                                              : Tuning::noteToMidiKey(audition_note);
          if (note >= 0 && note < 128)
            player.audition(note);
        }
        MessageManager::getInstance()->runDispatchLoop();
        if (server)
          server->stop();
        player.stop();
      }
      else
        exit_code = 1;
    }
  }

  MessageManager::deleteInstance();
  return exit_code;
}
