/* sloppy-synth: Android host for the Vital engine.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 *
 * Plays the engine through Oboe (AAudio on Android 8.1+, OpenSL ES below),
 * takes MIDI bytes from Android's MIDI API, and runs the same control server
 * as the Linux player so the web UI can be shown in a WebView. One synth per
 * process; the Kotlin side reaches it through NativeSynth.
 */
#include "JuceHeader.h"
#include "control_server.h"
#include "midi_parser.h"
#include "patch_library.h"
#include "sloppy_engine.h"

#include <android/log.h>
#include <jni.h>
#include <oboe/Oboe.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#define LOG_TAG "sloppy-synth"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

  constexpr int kChannels = 2;
  // Oboe may ask for more frames than this; bigger requests are split.
  constexpr int kMaxFrames = 2048;
  constexpr int kFirstPort = 8080;
  constexpr int kPortsToTry = 10;

  class AndroidSynth : public sloppy::ControlHost,
                       public oboe::AudioStreamDataCallback,
                       public oboe::AudioStreamErrorCallback {
    public:
      AndroidSynth(const File& library_root, const File& web_root) :
          library_(library_root), web_root_(web_root) {
        library_root.createDirectory();
        // Device settings (macro MIDI CCs) sit next to the library.
        engine_.setSettingsFile(library_root.getSiblingFile("settings.json"));
        rescan();
        for (int channel = 0; channel < kChannels; ++channel)
          planar_[channel].resize(kMaxFrames);
        midi_buffer_.ensureSize(4096);
      }

      ~AndroidSynth() override {
        stopServer();
        stopAudio();
      }

      // sloppy::ControlHost. Called from server threads.
      sloppy::Engine& getEngine() override { return engine_; }

      std::vector<sloppy::PatchEntry> getPatches() override {
        const ScopedLock lock(patches_lock_);
        return patches_;
      }

      bool loadPatchIndex(int index, std::string& error) override {
        File file;
        {
          const ScopedLock lock(patches_lock_);
          if (index < 0 || index >= static_cast<int>(patches_.size())) {
            error = "No patch number " + std::to_string(index);
            return false;
          }
          file = patches_[index].file;
        }
        return loadPatchFile(file, index, error);
      }

      int getCurrentPatchIndex() override { return current_index_; }
      int getPatchGeneration() override { return generation_; }

      void rescan() {
        std::vector<sloppy::PatchEntry> patches = library_.listPatches();
        const ScopedLock lock(patches_lock_);
        // Keep pointing at the same patch if it moved in the sorted list.
        if (current_index_ >= 0 && current_index_ < static_cast<int>(patches_.size())) {
          File current = patches_[current_index_].file;
          current_index_ = -1;
          for (size_t i = 0; i < patches.size(); ++i) {
            if (patches[i].file == current)
              current_index_ = static_cast<int>(i);
          }
        }
        patches_ = std::move(patches);
      }

      // Imports a .vitalbank, .vital or .zip file. `name` is the file's display
      // name, since files shared from other apps often arrive without one.
      bool importFile(const File& file, const String& name, std::string& message) {
        std::string error;
        bool ok = false;
        if (name.endsWithIgnoreCase(".vitalbank")) {
          std::string bank_name;
          ok = library_.importBank(file, bank_name, error);
          message = ok ? "Imported bank " + bank_name : error;
        }
        else if (name.endsWithIgnoreCase(".vital")) {
          // importPatch keeps the source's file name, so copy to the real
          // name first.
          File named = file.getSiblingFile(File::createLegalFileName(name));
          if (named != file && !file.copyFileTo(named)) {
            message = "Couldn't read the patch.";
            return false;
          }
          File imported;
          ok = library_.importPatch(named, imported, error);
          if (named != file)
            named.deleteFile();
          message = ok ? "Imported " + imported.getFileNameWithoutExtension().toStdString() : error;
          if (ok) {
            rescan();
            loadPatchFile(imported, indexOf(imported), error);
            return true;
          }
        }
        else if (name.endsWithIgnoreCase(".zip")) {
          ok = library_.importZip(file, name, message, error);
          if (!ok)
            message = error;
        }
        else
          message = "Not a Vital patch (.vital), bank (.vitalbank) or a zip of them.";

        if (ok)
          rescan();
        return ok;
      }

      bool startServer(bool allow_network, std::string& error) {
        stopServer();
        server_ = std::make_unique<sloppy::ControlServer>(*this, web_root_);
        // Local only unless asked: a phone on a public network shouldn't
        // offer its synth to everyone on it.
        String bind = allow_network ? String() : String("127.0.0.1");
        for (int port = kFirstPort; port < kFirstPort + kPortsToTry; ++port) {
          if (server_->start(port, bind, error)) {
            LOGI("Web UI on port %d", server_->getPort());
            return true;
          }
        }
        server_.reset();
        return false;
      }

      void stopServer() {
        if (server_) {
          server_->stop();
          server_.reset();
        }
      }

      int getPort() const { return server_ ? server_->getPort() : -1; }

      bool startAudio(std::string& error) {
        std::lock_guard<std::mutex> lock(stream_lock_);
        return openStream(error);
      }

      void stopAudio() {
        std::lock_guard<std::mutex> lock(stream_lock_);
        closeStream();
      }

      std::string describeAudio() {
        std::lock_guard<std::mutex> lock(stream_lock_);
        if (!stream_)
          return "Audio stopped";
        const char* api = stream_->getAudioApi() == oboe::AudioApi::AAudio ? "AAudio" : "OpenSL ES";
        int rate = stream_->getSampleRate();
        int frames = stream_->getBufferSizeInFrames();
        return std::string(api) + ", " + std::to_string(rate) + " Hz, " +
               std::to_string(frames) + " frames (" +
               std::to_string(rate > 0 ? 1000 * frames / rate : 0) + " ms), " +
               std::to_string(underruns_.load()) + " underruns";
      }

      // Called from the MIDI receiver threads.
      void midiBytes(int port, const uint8_t* data, size_t size) {
        std::lock_guard<std::mutex> lock(midi_lock_);
        parsers_[port].parse(data, size, [this](const uint8_t* message, int length) {
          handleMidi(MidiMessage(message, length));
        });
      }

      void allNotesOff() { engine_.allNotesOff(); }

      // oboe::AudioStreamDataCallback, on the audio thread.
      oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audio_data,
                                            int32_t num_frames) override {
        if (latency_tuner_)
          latency_tuner_->tune();

        int xruns = stream->getXRunCount().value();
        if (xruns > 0)
          underruns_ = xruns;

        float* output = static_cast<float*>(audio_data);
        float* channels[kChannels] = { planar_[0].data(), planar_[1].data() };
        for (int done = 0; done < num_frames;) {
          int frames = std::min(num_frames - done, kMaxFrames);
          AudioSampleBuffer buffer(channels, kChannels, frames);
          midi_buffer_.clear();
          engine_.process(buffer, midi_buffer_);

          float* out = output + done * kChannels;
          for (int i = 0; i < frames; ++i) {
            out[2 * i] = channels[0][i];
            out[2 * i + 1] = channels[1][i];
          }
          done += frames;
        }
        return oboe::DataCallbackResult::Continue;
      }

      // oboe::AudioStreamErrorCallback. Headphones plugged or unplugged, a
      // USB audio device appearing, and so on close the stream; open a new
      // one on the new default device. Oboe calls this on its own thread.
      void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result result) override {
        LOGI("Audio stream closed (%s), reopening", oboe::convertToText(result));
        // If the lock is taken, the app is stopping or restarting audio
        // itself, and waiting here could deadlock with its close().
        std::unique_lock<std::mutex> lock(stream_lock_, std::try_to_lock);
        if (!lock.owns_lock() || stream_.get() != stream)
          return;
        latency_tuner_.reset();
        stream_.reset();
        std::string error;
        if (!openStream(error))
          LOGE("Couldn't reopen audio: %s", error.c_str());
      }

    private:
      bool openStream(std::string& error) {
        if (stream_)
          return true;

        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(kChannels)
            ->setUsage(oboe::Usage::Media)
            ->setContentType(oboe::ContentType::Music)
            ->setDataCallback(this)
            ->setErrorCallback(this);

        oboe::Result result = builder.openStream(stream_);
        if (result != oboe::Result::OK) {
          error = std::string("Couldn't open audio output: ") + oboe::convertToText(result);
          stream_.reset();
          return false;
        }

        engine_.prepare(stream_->getSampleRate(), kMaxFrames);
        underruns_ = 0;

        // Start with two bursts of buffering and let the tuner add more
        // whenever the engine can't keep up. Old phones will need it.
        stream_->setBufferSizeInFrames(2 * stream_->getFramesPerBurst());
        latency_tuner_ = std::make_unique<oboe::LatencyTuner>(*stream_);

        result = stream_->requestStart();
        if (result != oboe::Result::OK) {
          error = std::string("Couldn't start audio output: ") + oboe::convertToText(result);
          closeStream();
          return false;
        }
        LOGI("Audio: %s, %d Hz, burst %d frames",
             stream_->getAudioApi() == oboe::AudioApi::AAudio ? "AAudio" : "OpenSL ES",
             stream_->getSampleRate(), stream_->getFramesPerBurst());
        return true;
      }

      void closeStream() {
        if (stream_) {
          stream_->stop();
          stream_->close();
          stream_.reset();
        }
        latency_tuner_.reset();
      }

      int indexOf(const File& file) {
        const ScopedLock lock(patches_lock_);
        for (size_t i = 0; i < patches_.size(); ++i) {
          if (patches_[i].file == file)
            return static_cast<int>(i);
        }
        return -1;
      }

      void handleMidi(const MidiMessage& message) {
        // Program changes step through the library, as on the Linux player.
        if (message.isController() && message.getControllerNumber() == 0) {
          bank_msb_ = message.getControllerValue();
          return;
        }
        if (message.isProgramChange()) {
          int index = 128 * bank_msb_ + message.getProgramChangeNumber();
          // Loading takes a while; keep it off the MIDI thread.
          Thread::launch([this, index] {
            std::string error;
            if (!loadPatchIndex(index, error))
              LOGE("%s", error.c_str());
          });
          return;
        }
        engine_.addMidiMessage(message);
      }

      bool loadPatchFile(const File& file, int index, std::string& error) {
        const ScopedLock lock(load_lock_);
        if (!engine_.loadPatch(file, error)) {
          LOGE("Couldn't load %s: %s", file.getFullPathName().toRawUTF8(), error.c_str());
          return false;
        }
        current_index_ = index;
        ++generation_;
        return true;
      }

      sloppy::PatchLibrary library_;
      File web_root_;
      CriticalSection patches_lock_;
      std::vector<sloppy::PatchEntry> patches_;
      CriticalSection load_lock_;
      std::atomic<int> current_index_ { -1 };
      std::atomic<int> generation_ { 0 };

      sloppy::Engine engine_;
      std::unique_ptr<sloppy::ControlServer> server_;

      std::mutex stream_lock_;
      std::shared_ptr<oboe::AudioStream> stream_;
      std::unique_ptr<oboe::LatencyTuner> latency_tuner_;
      std::atomic<int> underruns_ { 0 };
      std::vector<float> planar_[kChannels];
      MidiBuffer midi_buffer_;

      std::mutex midi_lock_;
      std::map<int, sloppy::MidiParser> parsers_;
      int bank_msb_ = 0;
  };

  std::mutex synth_lock;
  std::unique_ptr<AndroidSynth> synth;

  String toString(JNIEnv* env, jstring value) {
    if (value == nullptr)
      return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    String result = String::fromUTF8(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
  }

  jstring toJava(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
  }

  AndroidSynth* getSynth() {
    std::lock_guard<std::mutex> lock(synth_lock);
    return synth.get();
  }

} // namespace

#define NATIVE(name) Java_io_github_nfsp_1ta_sloppysynth_NativeSynth_##name

extern "C" {

  // Creates the synth. Returns an empty string, or what went wrong.
  // Safe to call again; later calls do nothing.
  JNIEXPORT jstring JNICALL NATIVE(nativeCreate)(JNIEnv* env, jobject, jobject context,
                                                  jstring library_dir, jstring web_dir,
                                                  jint default_sample_rate, jint default_frames_per_burst) {
    std::lock_guard<std::mutex> lock(synth_lock);
    if (synth)
      return toJava(env, "");

    Thread::initialiseJUCE(env, context);

    // Below Android 8.1, OpenSL ES only gets a fast path at the device's
    // own rate and burst size, which only Java can look up.
    if (default_sample_rate > 0)
      oboe::DefaultStreamValues::SampleRate = default_sample_rate;
    if (default_frames_per_burst > 0)
      oboe::DefaultStreamValues::FramesPerBurst = default_frames_per_burst;

    synth = std::make_unique<AndroidSynth>(File(toString(env, library_dir)), File(toString(env, web_dir)));
    return toJava(env, "");
  }

  // Returns the port the web UI is served on, or -1.
  JNIEXPORT jint JNICALL NATIVE(nativeStartServer)(JNIEnv*, jobject, jboolean allow_network) {
    AndroidSynth* instance = getSynth();
    if (instance == nullptr)
      return -1;
    std::string error;
    if (!instance->startServer(allow_network, error)) {
      LOGE("Web UI not started: %s", error.c_str());
      return -1;
    }
    return instance->getPort();
  }

  // Returns an empty string, or what went wrong.
  JNIEXPORT jstring JNICALL NATIVE(nativeStartAudio)(JNIEnv* env, jobject) {
    AndroidSynth* instance = getSynth();
    if (instance == nullptr)
      return toJava(env, "Synth not created");
    std::string error;
    instance->startAudio(error);
    return toJava(env, error);
  }

  JNIEXPORT void JNICALL NATIVE(nativeStopAudio)(JNIEnv*, jobject) {
    if (AndroidSynth* instance = getSynth())
      instance->stopAudio();
  }

  JNIEXPORT jstring JNICALL NATIVE(nativeDescribeAudio)(JNIEnv* env, jobject) {
    AndroidSynth* instance = getSynth();
    return toJava(env, instance ? instance->describeAudio() : "Synth not created");
  }

  JNIEXPORT void JNICALL NATIVE(nativeMidi)(JNIEnv* env, jobject, jint port, jbyteArray data,
                                             jint offset, jint count) {
    AndroidSynth* instance = getSynth();
    if (instance == nullptr || count <= 0)
      return;
    uint8_t bytes[256];
    while (count > 0) {
      jint chunk = std::min<jint>(count, sizeof(bytes));
      env->GetByteArrayRegion(data, offset, chunk, reinterpret_cast<jbyte*>(bytes));
      instance->midiBytes(port, bytes, static_cast<size_t>(chunk));
      offset += chunk;
      count -= chunk;
    }
  }

  JNIEXPORT void JNICALL NATIVE(nativeAllNotesOff)(JNIEnv*, jobject) {
    if (AndroidSynth* instance = getSynth())
      instance->allNotesOff();
  }

  // Imports a patch or bank. Returns the message to show; it starts with
  // "ok:" when the import worked.
  JNIEXPORT jstring JNICALL NATIVE(nativeImport)(JNIEnv* env, jobject, jstring path, jstring display_name) {
    AndroidSynth* instance = getSynth();
    if (instance == nullptr)
      return toJava(env, "Synth not created");
    std::string message;
    bool ok = instance->importFile(File(toString(env, path)), toString(env, display_name), message);
    return toJava(env, (ok ? "ok:" : "") + message);
  }

}
