/* sloppy-synth: HTTP + WebSocket control server for the web UI.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
#include "control_server.h"

#include "json/json.h"
#include "sloppy_engine.h"
#include "synth_constants.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>

namespace sloppy {

  using json = nlohmann::json;

  namespace {
    constexpr int kSocketTimeoutMs = 200;
    constexpr size_t kMaxHeaderBytes = 16 * 1024;
    constexpr uint64_t kMaxMessageBytes = 1024 * 1024;
    constexpr int kBroadcastIntervalMs = 40;
    constexpr int kMaxClients = 16;

    // ---------------------------------------------------------------------
    // SHA-1, only for the WebSocket handshake (RFC 6455 requires it).
    // ---------------------------------------------------------------------
    std::string sha1(const std::string& input) {
      uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
      std::string data = input;
      uint64_t bit_length = static_cast<uint64_t>(input.size()) * 8;
      data.push_back(static_cast<char>(0x80));
      while (data.size() % 64 != 56)
        data.push_back(0);
      for (int i = 7; i >= 0; --i)
        data.push_back(static_cast<char>((bit_length >> (i * 8)) & 0xff));

      auto rotl = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
      for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
          const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data() + chunk + 4 * i);
          w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        }
        for (int i = 16; i < 80; ++i)
          w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
          uint32_t f, k;
          if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
          else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
          else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
          else { f = b ^ c ^ d; k = 0xCA62C1D6; }
          uint32_t temp = rotl(a, 5) + f + e + k + w[i];
          e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
      }

      std::string digest;
      for (uint32_t word : h) {
        for (int i = 3; i >= 0; --i)
          digest.push_back(static_cast<char>((word >> (i * 8)) & 0xff));
      }
      return digest;
    }

    String contentTypeFor(const File& file) {
      static const std::map<String, String> kTypes = {
        { ".html", "text/html; charset=utf-8" },
        { ".js", "text/javascript; charset=utf-8" },
        { ".mjs", "text/javascript; charset=utf-8" },
        { ".css", "text/css; charset=utf-8" },
        { ".json", "application/json" },
        { ".webmanifest", "application/manifest+json" },
        { ".svg", "image/svg+xml" },
        { ".png", "image/png" },
        { ".ico", "image/x-icon" },
        { ".woff2", "font/woff2" },
      };
      auto found = kTypes.find(file.getFileExtension().toLowerCase());
      return found == kTypes.end() ? String("application/octet-stream") : found->second;
    }

    bool readExactly(StreamingSocket& socket, void* buffer, int bytes, const std::atomic<bool>& running) {
      char* out = static_cast<char*>(buffer);
      int done = 0;
      while (done < bytes) {
        if (!running)
          return false;
        int ready = socket.waitUntilReady(true, kSocketTimeoutMs);
        if (ready < 0)
          return false;
        if (ready == 0)
          continue;
        int read = socket.read(out + done, bytes - done, false);
        if (read <= 0)
          return false;
        done += read;
      }
      return true;
    }

    bool writeAll(StreamingSocket& socket, const void* data, int bytes) {
      const char* in = static_cast<const char*>(data);
      int done = 0;
      while (done < bytes) {
        int written = socket.write(in + done, bytes - done);
        if (written <= 0)
          return false;
        done += written;
      }
      return true;
    }
  } // namespace

  std::string webSocketAcceptKey(const std::string& client_key) {
    std::string digest = sha1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return Base64::toBase64(digest.data(), digest.size()).toStdString();
  }

  // -----------------------------------------------------------------------
  // Impl
  // -----------------------------------------------------------------------
  class ControlServer::Impl {
    public:
      class Client;

      Impl(ControlHost& host, File web_root) : host_(host), web_root_(std::move(web_root)) { }

      ~Impl() { stop(); }

      bool start(int port, const String& bind_address, std::string& error) {
        stop();
        listener_ = std::make_unique<StreamingSocket>();
        if (!listener_->createListener(port, bind_address)) {
          error = "Couldn't listen on port " + std::to_string(port);
          listener_.reset();
          return false;
        }
        running_ = true;
        accept_thread_ = std::make_unique<FunctionThread>("sloppy http accept", [this] { acceptLoop(); });
        broadcast_thread_ = std::make_unique<FunctionThread>("sloppy ws broadcast", [this] { broadcastLoop(); });
        accept_thread_->startThread();
        broadcast_thread_->startThread();
        return true;
      }

      void stop() {
        if (!running_.exchange(false))
          return;
        // Threads first: JUCE sockets must not be closed while another
        // thread is using them. Both loops wake at least every 200 ms.
        if (accept_thread_)
          accept_thread_->stopThread(2000);
        if (broadcast_thread_)
          broadcast_thread_->stopThread(2000);
        if (listener_)
          listener_->close();

        std::vector<std::shared_ptr<Client>> clients;
        {
          std::lock_guard<std::mutex> lock(clients_mutex_);
          clients.swap(clients_);
        }
        for (auto& client : clients)
          client->shutdown();
        listener_.reset();
      }

      int getPort() const { return listener_ ? listener_->getBoundPort() : -1; }

      int getNumClients() {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        int count = 0;
        for (auto& client : clients_)
          count += client->isWebSocket() ? 1 : 0;
        return count;
      }

      std::vector<std::string> handleMessage(const std::string& text);

      // A thread that runs a lambda; JUCE's Thread wants a subclass.
      class FunctionThread : public Thread {
        public:
          FunctionThread(const String& name, std::function<void()> body) : Thread(name), body_(std::move(body)) { }
          void run() override { body_(); }
        private:
          std::function<void()> body_;
      };

      class Client {
        public:
          Client(Impl& owner, std::unique_ptr<StreamingSocket> socket) :
              owner_(owner), socket_(std::move(socket)), alive_(true) { }

          ~Client() { shutdown(); }

          // The server's client list owns this object and only drops it
          // after shutdown() has stopped the thread.
          void start() {
            thread_ = std::make_unique<FunctionThread>("sloppy http client", [this] { run(); });
            thread_->startThread();
          }

          // Called from other threads, never the client's own. The socket is
          // only closed once the client thread has finished with it.
          void shutdown() {
            alive_ = false;
            if (thread_)
              thread_->stopThread(2000);
            std::lock_guard<std::mutex> lock(write_mutex_);
            if (socket_)
              socket_->close();
          }

          bool isAlive() const { return alive_; }
          bool isWebSocket() const { return websocket_; }

          bool sendText(const std::string& text) {
            return sendFrame(0x1, text.data(), text.size());
          }

        private:
          void run();
          bool handleHttp(const std::string& request);
          void serveFile(const String& path, bool head_only);
          void sendHttp(int status, const String& reason, const String& type, const MemoryBlock& body,
                        bool head_only = false);
          void webSocketLoop();
          bool sendFrame(int opcode, const void* data, size_t size);

          Impl& owner_;
          std::unique_ptr<StreamingSocket> socket_;
          std::unique_ptr<FunctionThread> thread_;
          std::mutex write_mutex_;
          std::atomic<bool> alive_;
          std::atomic<bool> websocket_ { false };
      };

    private:
      void acceptLoop();
      void broadcastLoop();
      void broadcast(const std::string& text);
      void removeDeadClients();

      json stateMessage();
      json parameterInfoMessage();
      json patchesMessage();
      json modulationInfoMessage();
      json modulationsMessage();
      json macroMidiMessage();

      ControlHost& host_;
      File web_root_;
      std::unique_ptr<StreamingSocket> listener_;
      std::unique_ptr<FunctionThread> accept_thread_;
      std::unique_ptr<FunctionThread> broadcast_thread_;
      std::atomic<bool> running_ { false };

      std::mutex clients_mutex_;
      std::vector<std::shared_ptr<Client>> clients_;

      friend class Client;
  };

  void ControlServer::Impl::acceptLoop() {
    while (running_ && !Thread::currentThreadShouldExit()) {
      if (listener_->waitUntilReady(true, kSocketTimeoutMs) <= 0)
        continue;
      std::unique_ptr<StreamingSocket> socket(listener_->waitForNextConnection());
      if (socket == nullptr)
        continue;

      removeDeadClients();
      std::lock_guard<std::mutex> lock(clients_mutex_);
      if (static_cast<int>(clients_.size()) >= kMaxClients) {
        socket->close();
        continue;
      }
      auto client = std::make_shared<Client>(*this, std::move(socket));
      clients_.push_back(client);
      client->start();
    }
  }

  void ControlServer::Impl::removeDeadClients() {
    std::vector<std::shared_ptr<Client>> dead;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      for (auto it = clients_.begin(); it != clients_.end();) {
        if (!(*it)->isAlive()) {
          dead.push_back(*it);
          it = clients_.erase(it);
        }
        else
          ++it;
      }
    }
    for (auto& client : dead)
      client->shutdown();
  }

  void ControlServer::Impl::broadcast(const std::string& text) {
    std::vector<std::shared_ptr<Client>> clients;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      clients = clients_;
    }
    for (auto& client : clients) {
      if (client->isWebSocket() && client->isAlive())
        client->sendText(text);
    }
  }

  // Watches the engine and pushes changes to every connected UI: parameter
  // moves (from any UI, MIDI learn or MIDI CC) and patch changes.
  void ControlServer::Impl::broadcastLoop() {
    // Baseline taken before the first sleep, so changes made as soon as the
    // server starts are still noticed.
    std::map<std::string, float> last_values;
    int last_generation = host_.getPatchGeneration();
    int last_modulation_generation = host_.getEngine().getModulationGeneration();
    int last_macro_midi_generation = host_.getEngine().getMacroMidi().getGeneration();
    for (const std::string& name : host_.getEngine().getParameterNames())
      last_values[name] = host_.getEngine().getParameter(name);

    while (running_ && !Thread::currentThreadShouldExit()) {
      Thread::sleep(kBroadcastIntervalMs);
      removeDeadClients();

      // Keep tracking values even with nobody connected, so a change made
      // right after a client connects is never folded into the baseline.
      int generation = host_.getPatchGeneration();
      if (generation != last_generation) {
        last_generation = generation;
        last_values.clear();
        if (getNumClients() > 0)
          broadcast(stateMessage().dump());
      }

      int modulation_generation = host_.getEngine().getModulationGeneration();
      if (modulation_generation != last_modulation_generation) {
        last_modulation_generation = modulation_generation;
        if (getNumClients() > 0)
          broadcast(modulationsMessage().dump());
      }

      // CC assignments change from UIs and from MIDI learn on the audio
      // thread; either way, save them here, off the audio thread.
      int macro_midi_generation = host_.getEngine().getMacroMidi().getGeneration();
      if (macro_midi_generation != last_macro_midi_generation) {
        last_macro_midi_generation = macro_midi_generation;
        host_.getEngine().saveSettings();
        if (getNumClients() > 0)
          broadcast(macroMidiMessage().dump());
      }

      Engine& engine = host_.getEngine();
      json changed = json::object();
      for (const std::string& name : engine.getParameterNames()) {
        float value = engine.getParameter(name);
        auto found = last_values.find(name);
        if (found == last_values.end()) {
          last_values[name] = value;
          continue;
        }
        if (found->second != value) {
          found->second = value;
          changed[name] = value;
        }
      }
      if (!changed.empty() && getNumClients() > 0)
        broadcast(json({ { "type", "params" }, { "values", changed } }).dump());
    }
  }

  json ControlServer::Impl::stateMessage() {
    Engine& engine = host_.getEngine();
    json values = json::object();
    for (const std::string& name : engine.getParameterNames())
      values[name] = engine.getParameter(name);

    json macros = json::array();
    for (int i = 0; i < vital::kNumMacros; ++i)
      macros.push_back(engine.getMacroName(i).toStdString());

    return {
      { "type", "state" },
      { "patch", {
        { "index", host_.getCurrentPatchIndex() },
        { "name", engine.getPresetName().toStdString() },
        { "author", engine.getAuthor().toStdString() },
        { "style", engine.getStyle().toStdString() },
        { "comments", engine.getComments().toStdString() },
      } },
      { "macros", macros },
      { "values", values },
    };
  }

  json ControlServer::Impl::parameterInfoMessage() {
    Engine& engine = host_.getEngine();
    json params = json::array();
    for (const std::string& name : engine.getParameterNames()) {
      ParameterInfo info;
      if (!engine.getParameterInfo(name, info))
        continue;
      json param = {
        { "name", info.name },
        { "label", info.display_name },
        { "min", info.min },
        { "max", info.max },
        { "default", info.default_value },
        { "scale", info.value_scale },
        { "offset", info.post_offset },
        { "multiply", info.display_multiply },
        { "invert", info.display_invert },
        { "units", info.units },
      };
      if (!info.options.empty())
        param["options"] = info.options;
      params.push_back(param);
    }
    return { { "type", "param_info" }, { "params", params } };
  }

  json ControlServer::Impl::patchesMessage() {
    json patches = json::array();
    int index = 0;
    for (const PatchEntry& patch : host_.getPatches()) {
      patches.push_back({
        { "index", index++ },
        { "name", patch.name },
        { "bank", patch.bank },
        { "category", patch.category },
        { "folders", patch.folders },
      });
    }
    return { { "type", "patches" }, { "current", host_.getCurrentPatchIndex() }, { "patches", patches } };
  }

  json ControlServer::Impl::modulationInfoMessage() {
    Engine& engine = host_.getEngine();
    return {
      { "type", "mod_info" },
      { "sources", engine.getModulationSources() },
      { "destinations", engine.getModulationDestinations() },
    };
  }

  json ControlServer::Impl::modulationsMessage() {
    json modulations = json::array();
    for (const Modulation& modulation : host_.getEngine().getModulations()) {
      modulations.push_back({
        { "slot", modulation.slot },
        { "source", modulation.source },
        { "destination", modulation.destination },
      });
    }
    return {
      { "type", "modulations" },
      { "modulations", modulations },
      { "vital_warnings", host_.getEngine().getVitalIncompatibilities() },
    };
  }

  json ControlServer::Impl::macroMidiMessage() {
    MacroMidiMap& map = host_.getEngine().getMacroMidi();
    json defaults = json::array();
    for (int i = 0; i < MacroMidiMap::kNumMacros; ++i) {
      MacroMidiAssignment assignment = MacroMidiMap::defaultAssignment(i);
      defaults.push_back({ { "cc", assignment.cc }, { "channel", assignment.channel } });
    }
    return {
      { "type", "macro_midi" },
      { "assignments", json::parse(map.toJson()) },
      { "defaults", defaults },
      { "learning", map.getLearning() + 1 },
    };
  }

  // Protocol, one JSON object per WebSocket text message. See docs/PROTOCOL.md.
  std::vector<std::string> ControlServer::Impl::handleMessage(const std::string& text) {
    auto error = [](const std::string& message) {
      return std::vector<std::string> { json({ { "type", "error" }, { "message", message } }).dump() };
    };

    json message;
    try {
      message = json::parse(text);
    }
    catch (const json::exception&) {
      return error("Message isn't valid JSON.");
    }
    if (!message.is_object() || !message.count("type") || !message["type"].is_string())
      return error("Message needs a \"type\".");

    std::string type = message["type"];
    Engine& engine = host_.getEngine();

    try {
      if (type == "hello" || type == "get_state")
        return { stateMessage().dump() };

      if (type == "get_param_info")
        return { parameterInfoMessage().dump() };

      if (type == "list_patches")
        return { patchesMessage().dump() };

      if (type == "get_mod_info")
        return { modulationInfoMessage().dump() };

      if (type == "get_modulations")
        return { modulationsMessage().dump() };

      // Macro CC assignments. Every client, this one included, gets the new
      // assignments from the broadcast loop.
      if (type == "get_macro_midi")
        return { macroMidiMessage().dump() };

      if (type == "set_macro_midi") {
        MacroMidiMap& map = engine.getMacroMidi();
        int macro = message.at("macro").get<int>() - 1;
        MacroMidiAssignment assignment = map.get(macro);
        assignment.cc = message.value("cc", assignment.cc);
        assignment.channel = message.value("channel", assignment.channel);
        std::string set_error;
        if (!map.set(macro, assignment, set_error))
          return error(set_error);
        return {};
      }

      if (type == "learn_macro_midi") {
        int macro = message.value("macro", 0);
        if (macro < 0 || macro > MacroMidiMap::kNumMacros)
          return error("No macro " + std::to_string(macro) + ".");
        engine.getMacroMidi().learn(macro - 1);
        return {};
      }

      if (type == "reset_macro_midi") {
        engine.getMacroMidi().resetToDefaults();
        return {};
      }

      // Every client, this one included, gets the new list of routings from
      // the broadcast loop.
      if (type == "add_modulation") {
        std::string add_error;
        if (!engine.addModulation(message.at("source"), message.at("destination"),
                                  message.value("amount", 0.5f), add_error))
          return error(add_error);
        return {};
      }

      if (type == "remove_modulation") {
        if (!engine.removeModulation(message.at("source"), message.at("destination")))
          return error("No such modulation.");
        return {};
      }

      if (type == "set") {
        std::string name = message.at("name");
        float value = message.at("value");
        if (!engine.hasParameter(name))
          return error("Unknown parameter " + name);
        ParameterInfo info;
        if (engine.getParameterInfo(name, info))
          value = jlimit(info.min, info.max, value);
        engine.setParameter(name, value);
        return {};
      }

      if (type == "note") {
        int note = message.at("note");
        if (note < 0 || note > 127)
          return error("Note out of range.");
        float velocity = message.value("velocity", 0.8f);
        int channel = jlimit(1, 16, message.value("channel", 1));
        if (message.value("on", true))
          engine.addMidiMessage(MidiMessage::noteOn(channel, note, jlimit(0.0f, 1.0f, velocity)));
        else
          engine.addMidiMessage(MidiMessage::noteOff(channel, note, jlimit(0.0f, 1.0f, velocity)));
        return {};
      }

      if (type == "all_notes_off") {
        engine.allNotesOff();
        return {};
      }

      if (type == "load_patch") {
        int index = message.at("index");
        std::string load_error;
        if (!host_.loadPatchIndex(index, load_error))
          return error(load_error);
        // Every client, this one included, gets the new state from the
        // broadcast loop.
        return {};
      }
    }
    catch (const json::exception&) {
      return error("Bad or missing fields for \"" + type + "\".");
    }

    return error("Unknown message type \"" + type + "\".");
  }

  // -----------------------------------------------------------------------
  // Client: one thread per connection. Plain HTTP for files, upgraded to a
  // WebSocket for /ws.
  // -----------------------------------------------------------------------
  void ControlServer::Impl::Client::run() {
    std::string request;
    char byte = 0;
    while (alive_ && request.size() < kMaxHeaderBytes) {
      if (!readExactly(*socket_, &byte, 1, alive_))
        break;
      request.push_back(byte);
      if (request.size() >= 4 && request.compare(request.size() - 4, 4, "\r\n\r\n") == 0) {
        if (handleHttp(request))
          webSocketLoop();
        break;
      }
    }
    // The server notices within one broadcast tick and closes the socket.
    alive_ = false;
  }

  bool ControlServer::Impl::Client::handleHttp(const std::string& request) {
    StringArray lines;
    lines.addLines(String(request));
    StringArray request_line;
    request_line.addTokens(lines[0], " ", "");
    if (request_line.size() < 3) {
      sendHttp(400, "Bad Request", "text/plain", MemoryBlock("bad request", 11));
      return false;
    }

    String method = request_line[0];
    String path = URL::removeEscapeChars(request_line[1].upToFirstOccurrenceOf("?", false, false));

    std::map<String, String> headers;
    for (int i = 1; i < lines.size(); ++i) {
      String line = lines[i];
      if (line.containsChar(':'))
        headers[line.upToFirstOccurrenceOf(":", false, false).trim().toLowerCase()] =
            line.fromFirstOccurrenceOf(":", false, false).trim();
    }

    if (path == "/ws") {
      String key = headers["sec-websocket-key"];
      if (!headers["upgrade"].equalsIgnoreCase("websocket") || key.isEmpty()) {
        sendHttp(400, "Bad Request", "text/plain", MemoryBlock("expected a websocket upgrade", 28));
        return false;
      }

      String response = "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: " + String(webSocketAcceptKey(key.toStdString())) + "\r\n\r\n";
      std::lock_guard<std::mutex> lock(write_mutex_);
      if (!writeAll(*socket_, response.toRawUTF8(), static_cast<int>(response.getNumBytesAsUTF8())))
        return false;
      websocket_ = true;
      return true;
    }

    if (method != "GET" && method != "HEAD") {
      sendHttp(405, "Method Not Allowed", "text/plain", MemoryBlock("method not allowed", 18));
      return false;
    }
    serveFile(path, method == "HEAD");
    return false;
  }

  void ControlServer::Impl::Client::serveFile(const String& path, bool head_only) {
    String relative = path.trimCharactersAtStart("/");
    if (relative.isEmpty() || relative.endsWithChar('/'))
      relative += "index.html";

    StringArray parts;
    parts.addTokens(relative, "/", "");
    File root = owner_.web_root_;
    File file = root.getChildFile(relative);
    bool safe = !parts.contains("..") && !relative.containsChar('\\') && file.isAChildOf(root);

    if (!safe || !file.existsAsFile()) {
      sendHttp(404, "Not Found", "text/plain", MemoryBlock("not found", 9), head_only);
      return;
    }

    MemoryBlock body;
    if (!file.loadFileAsData(body)) {
      sendHttp(500, "Internal Server Error", "text/plain", MemoryBlock("read error", 10), head_only);
      return;
    }
    sendHttp(200, "OK", contentTypeFor(file), body, head_only);
  }

  void ControlServer::Impl::Client::sendHttp(int status, const String& reason, const String& type,
                                             const MemoryBlock& body, bool head_only) {
    String header = "HTTP/1.1 " + String(status) + " " + reason + "\r\n"
                    "Content-Type: " + type + "\r\n"
                    "Content-Length: " + String(static_cast<int64>(body.getSize())) + "\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: close\r\n\r\n";
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (writeAll(*socket_, header.toRawUTF8(), static_cast<int>(header.getNumBytesAsUTF8())) && !head_only)
      writeAll(*socket_, body.getData(), static_cast<int>(body.getSize()));
  }

  bool ControlServer::Impl::Client::sendFrame(int opcode, const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!alive_)
      return false;

    unsigned char header[10];
    int header_size = 2;
    header[0] = static_cast<unsigned char>(0x80 | opcode);
    if (size < 126)
      header[1] = static_cast<unsigned char>(size);
    else if (size <= 0xffff) {
      header[1] = 126;
      header[2] = static_cast<unsigned char>(size >> 8);
      header[3] = static_cast<unsigned char>(size & 0xff);
      header_size = 4;
    }
    else {
      header[1] = 127;
      for (int i = 0; i < 8; ++i)
        header[2 + i] = static_cast<unsigned char>((static_cast<uint64_t>(size) >> (8 * (7 - i))) & 0xff);
      header_size = 10;
    }

    if (!writeAll(*socket_, header, header_size) ||
        (size > 0 && !writeAll(*socket_, data, static_cast<int>(size)))) {
      alive_ = false;
      return false;
    }
    return true;
  }

  void ControlServer::Impl::Client::webSocketLoop() {
    std::string message;
    while (alive_ && owner_.running_) {
      unsigned char header[2];
      if (!readExactly(*socket_, header, 2, alive_))
        return;

      bool final_fragment = (header[0] & 0x80) != 0;
      int opcode = header[0] & 0x0f;
      bool masked = (header[1] & 0x80) != 0;
      uint64_t length = header[1] & 0x7f;

      if (length == 126) {
        unsigned char extended[2];
        if (!readExactly(*socket_, extended, 2, alive_))
          return;
        length = (uint64_t(extended[0]) << 8) | extended[1];
      }
      else if (length == 127) {
        unsigned char extended[8];
        if (!readExactly(*socket_, extended, 8, alive_))
          return;
        length = 0;
        for (unsigned char b : extended)
          length = (length << 8) | b;
      }

      // Clients must mask their frames (RFC 6455 5.1).
      if (!masked || length > kMaxMessageBytes || message.size() + length > kMaxMessageBytes)
        return;

      unsigned char mask[4];
      if (!readExactly(*socket_, mask, 4, alive_))
        return;

      std::string payload(static_cast<size_t>(length), '\0');
      if (length > 0 && !readExactly(*socket_, &payload[0], static_cast<int>(length), alive_))
        return;
      for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);

      if (opcode == 0x8) {  // close
        sendFrame(0x8, payload.data(), std::min<size_t>(payload.size(), 2));
        return;
      }
      if (opcode == 0x9) {  // ping
        sendFrame(0xA, payload.data(), payload.size());
        continue;
      }
      if (opcode == 0xA)  // pong
        continue;
      if (opcode == 0x2)  // binary: not part of the protocol
        return;

      message += payload;
      if (!final_fragment)
        continue;

      for (const std::string& reply : owner_.handleMessage(message))
        sendText(reply);
      message.clear();
    }
  }

  // -----------------------------------------------------------------------
  // ControlServer
  // -----------------------------------------------------------------------
  ControlServer::ControlServer(ControlHost& host, File web_root) :
      impl_(std::make_unique<Impl>(host, std::move(web_root))) { }

  ControlServer::~ControlServer() = default;

  bool ControlServer::start(int port, const String& bind_address, std::string& error) {
    return impl_->start(port, bind_address, error);
  }

  void ControlServer::stop() { impl_->stop(); }
  int ControlServer::getPort() const { return impl_->getPort(); }
  int ControlServer::getNumClients() const { return impl_->getNumClients(); }

  std::vector<std::string> ControlServer::handleMessage(const std::string& message) {
    return impl_->handleMessage(message);
  }

} // namespace sloppy
