/* sloppy-synth control server tests: protocol, WebSocket and HTTP.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
#include "JuceHeader.h"
#include "control_server.h"
#include "crash_report.h"
#include "json/json.h"
#include "patch_library.h"
#include "sloppy_engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>

using json = nlohmann::json;

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

  class TestHost : public sloppy::ControlHost {
    public:
      TestHost() {
        patches_.push_back({ fixture("test_bass.vital"), "Test Bass", "Fixtures", "Bass" });
        patches_.push_back({ fixture("from_the_future.vital"), "Future", "Fixtures", "" });
      }
      sloppy::Engine& getEngine() override { return engine_; }
      std::vector<sloppy::PatchEntry> getPatches() override { return patches_; }
      bool loadPatchIndex(int index, std::string& error) override {
        if (index < 0 || index >= static_cast<int>(patches_.size())) {
          error = "No such patch";
          return false;
        }
        if (!engine_.loadPatch(patches_[index].file, error))
          return false;
        current_ = index;
        ++generation_;
        return true;
      }
      int getCurrentPatchIndex() override { return current_; }
      int getPatchGeneration() override { return generation_; }

    private:
      sloppy::Engine engine_;
      std::vector<sloppy::PatchEntry> patches_;
      std::atomic<int> current_ { -1 };
      std::atomic<int> generation_ { 0 };
  };

  json single(sloppy::ControlServer& server, const json& message) {
    std::vector<std::string> replies = server.handleMessage(message.dump());
    if (replies.size() != 1)
      return json();
    return json::parse(replies[0]);
  }

  void testAcceptKey() {
    // Example from RFC 6455 section 1.3.
    CHECK(sloppy::webSocketAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  }

  void testProtocol() {
    TestHost host;
    sloppy::ControlServer server(host, File());

    json state = single(server, { { "type", "hello" } });
    CHECK(state["type"] == "state");
    CHECK(state["values"].size() > 700);
    CHECK(state["macros"].size() == 8);
    CHECK(state["macros"][7] == "MACRO 8");

    json info = single(server, { { "type", "get_param_info" } });
    CHECK(info["type"] == "param_info");
    bool found_model = false, found_style = false;
    for (const json& param : info["params"]) {
      if (param["name"] == "filter_1_model") {
        found_model = true;
        CHECK(param.count("options") == 1);
        CHECK(param["options"].size() == 8);
      }
      if (param["name"] == "filter_1_style") {
        found_style = true;
        // Vital's style names array is shorter than the parameter's range.
        CHECK(param.count("options") == 1);
        CHECK(param["options"].size() == 5);
      }
    }
    CHECK(found_model);
    CHECK(found_style);

    CHECK(server.handleMessage(json({ { "type", "set" }, { "name", "filter_1_cutoff" }, { "value", 90.0 } }).dump()).empty());
    CHECK(host.getEngine().getParameter("filter_1_cutoff") == 90.0f);
    // Out-of-range values are clamped.
    server.handleMessage(json({ { "type", "set" }, { "name", "filter_1_cutoff" }, { "value", 1e6 } }).dump());
    CHECK(host.getEngine().getParameter("filter_1_cutoff") == 136.0f);

    CHECK(single(server, { { "type", "set" }, { "name", "nope" }, { "value", 1 } })["type"] == "error");
    CHECK(single(server, { { "type", "set" }, { "name", "filter_1_cutoff" } })["type"] == "error");
    CHECK(single(server, { { "type", "note" }, { "note", 300 } })["type"] == "error");
    CHECK(single(server, { { "type", "whatever" } })["type"] == "error");
    CHECK(json::parse(server.handleMessage("not json")[0])["type"] == "error");

    json patches = single(server, { { "type", "list_patches" } });
    CHECK(patches["patches"].size() == 2);
    CHECK(patches["patches"][0]["name"] == "Test Bass");

    CHECK(server.handleMessage(json({ { "type", "load_patch" }, { "index", 0 } }).dump()).empty());
    CHECK(host.getCurrentPatchIndex() == 0);
    CHECK(host.getEngine().getParameter("filter_1_on") == 1.0f);
    CHECK(single(server, { { "type", "load_patch" }, { "index", 1 } })["type"] == "error");
    CHECK(single(server, { { "type", "load_patch" }, { "index", 9 } })["type"] == "error");
    CHECK(host.getCurrentPatchIndex() == 0);

    // Macro CC assignments.
    json macro_midi = single(server, { { "type", "get_macro_midi" } });
    CHECK(macro_midi["type"] == "macro_midi");
    CHECK(macro_midi["assignments"].size() == 8);
    CHECK(macro_midi["assignments"][0]["cc"] == 21);
    CHECK(macro_midi["assignments"][0]["channel"] == 0);
    CHECK(macro_midi["defaults"][7]["cc"] == 28);
    CHECK(macro_midi["learning"] == 0);
    sloppy::MacroMidiMap& map = host.getEngine().getMacroMidi();
    CHECK(server.handleMessage(json({ { "type", "set_macro_midi" }, { "macro", 2 }, { "cc", 74 } }).dump()).empty());
    CHECK(map.get(1).cc == 74 && map.get(1).channel == 0);
    CHECK(server.handleMessage(json({ { "type", "set_macro_midi" }, { "macro", 2 }, { "channel", 16 } }).dump()).empty());
    CHECK(map.get(1).cc == 74 && map.get(1).channel == 16);
    CHECK(single(server, { { "type", "set_macro_midi" }, { "macro", 2 }, { "cc", 127 } })["type"] == "error");
    CHECK(single(server, { { "type", "set_macro_midi" }, { "macro", 9 }, { "cc", 1 } })["type"] == "error");
    CHECK(single(server, { { "type", "set_macro_midi" }, { "cc", 1 } })["type"] == "error");
    CHECK(server.handleMessage(json({ { "type", "learn_macro_midi" }, { "macro", 3 } }).dump()).empty());
    CHECK(single(server, { { "type", "get_macro_midi" } })["learning"] == 3);
    CHECK(server.handleMessage(json({ { "type", "learn_macro_midi" } }).dump()).empty());
    CHECK(map.getLearning() == -1);
    CHECK(single(server, { { "type", "learn_macro_midi" }, { "macro", 9 } })["type"] == "error");
    CHECK(server.handleMessage(json({ { "type", "reset_macro_midi" } }).dump()).empty());
    CHECK(map.get(1).cc == 22 && map.get(1).channel == 0);

    json mod_info = single(server, { { "type", "get_mod_info" } });
    CHECK(mod_info["type"] == "mod_info");
    CHECK(std::find(mod_info["sources"].begin(), mod_info["sources"].end(), "lfo_1") != mod_info["sources"].end());
    CHECK(std::find(mod_info["destinations"].begin(), mod_info["destinations"].end(), "filter_1_cutoff") !=
          mod_info["destinations"].end());

    size_t before = single(server, { { "type", "get_modulations" } })["modulations"].size();
    CHECK(server.handleMessage(json({ { "type", "add_modulation" }, { "source", "lfo_2" },
                                      { "destination", "filter_1_cutoff" }, { "amount", 0.3 } }).dump()).empty());
    json mods = single(server, { { "type", "get_modulations" } });
    CHECK(mods["modulations"].size() == before + 1);
    json added;
    for (const json& mod : mods["modulations"]) {
      if (mod["source"] == "lfo_2" && mod["destination"] == "filter_1_cutoff")
        added = mod;
    }
    CHECK(added.is_object());
    if (added.is_object()) {
      std::string amount = "modulation_" + std::to_string(added["slot"].get<int>()) + "_amount";
      CHECK(std::abs(host.getEngine().getParameter(amount) - 0.3f) < 1e-6f);
    }
    CHECK(single(server, { { "type", "add_modulation" }, { "source", "nope" },
                           { "destination", "filter_1_cutoff" } })["type"] == "error");
    CHECK(server.handleMessage(json({ { "type", "remove_modulation" }, { "source", "lfo_2" },
                                      { "destination", "filter_1_cutoff" } }).dump()).empty());
    CHECK(single(server, { { "type", "get_modulations" } })["modulations"].size() == before);
    CHECK(single(server, { { "type", "remove_modulation" }, { "source", "lfo_2" },
                           { "destination", "filter_1_cutoff" } })["type"] == "error");
  }

  // Minimal WebSocket client over a raw socket.
  struct WsClient {
    StreamingSocket socket;

    bool connect(int port) {
      if (!socket.connect("127.0.0.1", port, 2000))
        return false;
      String request = "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
      socket.write(request.toRawUTF8(), static_cast<int>(request.getNumBytesAsUTF8()));
      std::string response;
      char c;
      while (response.find("\r\n\r\n") == std::string::npos) {
        if (socket.waitUntilReady(true, 2000) <= 0 || socket.read(&c, 1, true) != 1)
          return false;
        response.push_back(c);
      }
      return response.find("101") != std::string::npos &&
             response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos;
    }

    void sendText(const std::string& text) {
      std::string frame;
      frame.push_back(static_cast<char>(0x81));
      const unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
      if (text.size() < 126)
        frame.push_back(static_cast<char>(0x80 | text.size()));
      else {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>(text.size() >> 8));
        frame.push_back(static_cast<char>(text.size() & 0xff));
      }
      frame.append(reinterpret_cast<const char*>(mask), 4);
      for (size_t i = 0; i < text.size(); ++i)
        frame.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
      socket.write(frame.data(), static_cast<int>(frame.size()));
    }

    bool readExact(void* buffer, int bytes) {
      char* out = static_cast<char*>(buffer);
      int done = 0;
      while (done < bytes) {
        if (socket.waitUntilReady(true, 3000) <= 0)
          return false;
        int read = socket.read(out + done, bytes - done, false);
        if (read <= 0)
          return false;
        done += read;
      }
      return true;
    }

    // Returns the next text message, or "" on timeout.
    std::string receive() {
      unsigned char header[2];
      if (!readExact(header, 2))
        return "";
      uint64_t length = header[1] & 0x7f;
      if (length == 126) {
        unsigned char ext[2];
        readExact(ext, 2);
        length = (uint64_t(ext[0]) << 8) | ext[1];
      }
      else if (length == 127) {
        unsigned char ext[8];
        readExact(ext, 8);
        length = 0;
        for (unsigned char b : ext)
          length = (length << 8) | b;
      }
      std::string payload(static_cast<size_t>(length), '\0');
      if (length && !readExact(&payload[0], static_cast<int>(length)))
        return "";
      return payload;
    }

    // Reads messages until one of `type` arrives.
    json receiveType(const std::string& type) {
      for (int i = 0; i < 50; ++i) {
        std::string text = receive();
        if (text.empty())
          return json();
        json message = json::parse(text, nullptr, false);
        if (message.is_object() && message["type"] == type)
          return message;
      }
      return json();
    }
  };

  std::string httpGet(int port, const String& path) {
    StreamingSocket socket;
    if (!socket.connect("127.0.0.1", port, 2000))
      return "";
    String request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    socket.write(request.toRawUTF8(), static_cast<int>(request.getNumBytesAsUTF8()));
    std::string response;
    char buffer[4096];
    while (socket.waitUntilReady(true, 2000) > 0) {
      int read = socket.read(buffer, sizeof(buffer), false);
      if (read <= 0)
        break;
      response.append(buffer, read);
    }
    return response;
  }

  void testServerOverNetwork() {
    TemporaryFile temp_dir;
    File web_root = temp_dir.getFile();
    web_root.createDirectory();
    web_root.getChildFile("index.html").replaceWithText("<!doctype html><title>t</title>");
    web_root.getSiblingFile("secret.txt").replaceWithText("secret");

    TestHost host;
    sloppy::ControlServer server(host, web_root);
    std::string error;
    CHECK(server.start(0, "127.0.0.1", error));
    int port = server.getPort();
    CHECK(port > 0);

    std::string index = httpGet(port, "/");
    CHECK(index.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(index.find("text/html") != std::string::npos);
    CHECK(index.find("<title>t</title>") != std::string::npos);
    CHECK(httpGet(port, "/missing.js").rfind("HTTP/1.1 404", 0) == 0);
    CHECK(httpGet(port, "/../secret.txt").rfind("HTTP/1.1 404", 0) == 0);
    CHECK(httpGet(port, "/%2e%2e/secret.txt").rfind("HTTP/1.1 404", 0) == 0);

    WsClient a, b;
    CHECK(a.connect(port));
    CHECK(b.connect(port));

    a.sendText(json({ { "type", "hello" } }).dump());
    json state = a.receiveType("state");
    CHECK(state["type"] == "state");

    // Long message (16-bit length) round trip.
    a.sendText(json({ { "type", "get_param_info" }, { "padding", std::string(300, 'x') } }).dump());
    json info = a.receiveType("param_info");
    CHECK(info["params"].size() > 700);

    // A change from one client reaches the other.
    b.sendText(json({ { "type", "hello" } }).dump());
    CHECK(b.receiveType("state")["type"] == "state");
    a.sendText(json({ { "type", "set" }, { "name", "macro_control_2" }, { "value", 0.75 } }).dump());
    json params = b.receiveType("params");
    CHECK(params["values"].count("macro_control_2") == 1);
    CHECK(params["values"]["macro_control_2"] == 0.75);

    // A new routing from one client reaches the other.
    a.sendText(json({ { "type", "add_modulation" }, { "source", "lfo_3" }, { "destination", "osc_2_level" } }).dump());
    json routed = b.receiveType("modulations");
    CHECK(routed["vital_warnings"].empty());
    bool seen = false;
    for (const json& mod : routed["modulations"])
      seen = seen || (mod["source"] == "lfo_3" && mod["destination"] == "osc_2_level");
    CHECK(seen);

    // CC assignments from one client reach the other, and so does a MIDI
    // learn finished by the audio thread.
    a.sendText(json({ { "type", "set_macro_midi" }, { "macro", 5 }, { "cc", 40 }, { "channel", 3 } }).dump());
    json assigned = b.receiveType("macro_midi");
    CHECK(assigned["assignments"][4]["cc"] == 40);
    CHECK(assigned["assignments"][4]["channel"] == 3);
    a.sendText(json({ { "type", "learn_macro_midi" }, { "macro", 6 } }).dump());
    CHECK(b.receiveType("macro_midi")["learning"] == 6);
    {
      AudioSampleBuffer buffer(2, 256);
      MidiBuffer midi;
      midi.addEvent(MidiMessage::controllerEvent(1, 90, 100), 0);
      host.getEngine().process(buffer, midi);
    }
    json learned = b.receiveType("macro_midi");
    CHECK(learned["learning"] == 0);
    CHECK(learned["assignments"][5]["cc"] == 90);

    // Loading a patch sends everyone the new state.
    a.sendText(json({ { "type", "load_patch" }, { "index", 0 } }).dump());
    json loaded = b.receiveType("state");
    CHECK(loaded["patch"]["index"] == 0);
    CHECK(loaded["values"]["filter_1_on"] == 1.0);

    CHECK(server.getNumClients() == 2);
    server.stop();
    web_root.deleteRecursively();
    web_root.getSiblingFile("secret.txt").deleteFile();
  }

  void testLayoutMatchesParameters() {
    File layout_file = File(SLOPPY_WEB_DIR).getChildFile("layout.json");
    CHECK(layout_file.existsAsFile());
    json layout = json::parse(layout_file.loadFileAsString().toStdString());
    sloppy::Engine engine;

    int missing = 0;
    auto check = [&](const std::string& name) {
      if (!engine.hasParameter(name)) {
        std::cerr << "  layout.json names unknown parameter " << name << "\n";
        ++missing;
      }
    };
    CHECK(layout["macros"].size() == sloppy::MacroMidiMap::kNumMacros);
    for (const json& name : layout["macros"])
      check(name);
    std::vector<std::string> sources = engine.getModulationSources();
    for (const json& page : layout["pages"]) {
      json instances = page.count("instances") ? page["instances"] : json::array({ nullptr });
      for (const json& instance : instances) {
        std::string n = instance.is_null() ? "" : instance.is_string() ? instance.get<std::string>()
                                                                        : std::to_string(instance.get<int>());
        auto resolve = [&](std::string name) {
          size_t at = name.find("{n}");
          if (at != std::string::npos)
            name.replace(at, 3, n);
          return name;
        };
        if (page.count("params")) {
          for (const json& param : page["params"])
            check(resolve(param));
        }
        if (page.count("switch"))
          check(resolve(page["switch"]));
        if (page.count("source")) {
          std::string source = resolve(page["source"]);
          if (std::find(sources.begin(), sources.end(), source) == sources.end()) {
            std::cerr << "  layout.json names unknown modulation source " << source << "\n";
            ++missing;
          }
        }
      }
    }
    CHECK(missing == 0);
  }
}

int main() {
  crash_report::install();
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  struct Test { const char* name; void (*run)(); };
  const Test tests[] = {
    { "websocket accept key", testAcceptKey },
    { "protocol messages", testProtocol },
    { "server over the network", testServerOverNetwork },
    { "web layout names real parameters", testLayoutMatchesParameters },
  };

  for (const Test& test : tests) {
    int before = failures;
    std::cout << "run  " << test.name << "\n";
    test.run();
    std::cout << (failures == before ? "ok   " : "FAIL ") << test.name << "\n";
  }
  std::cout << checks << " checks, " << failures << " failed\n";
  return failures == 0 ? 0 : 1;
}
