/* sloppy-synth: HTTP + WebSocket control server for the web UI.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 *
 * Serves the web UI's static files over HTTP, and speaks a small JSON
 * protocol over a WebSocket at /ws (see docs/PROTOCOL.md). It only uses
 * juce_core sockets and threads, so it needs no message loop and works the
 * same on Linux and Android.
 */
#pragma once

#include "JuceHeader.h"
#include "patch_library.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sloppy {

  class Engine;

  // What the server needs from the program hosting the engine. Every method
  // may be called from server threads, so implementations must be thread
  // safe.
  class ControlHost {
    public:
      virtual ~ControlHost() = default;
      virtual Engine& getEngine() = 0;
      virtual std::vector<PatchEntry> getPatches() = 0;
      // Returns false and fills `error` if the patch couldn't be loaded.
      virtual bool loadPatchIndex(int index, std::string& error) = 0;
      virtual int getCurrentPatchIndex() = 0;
      // Bumped every time a different patch is loaded, from any source.
      virtual int getPatchGeneration() = 0;
  };

  class ControlServer {
    public:
      ControlServer(ControlHost& host, File web_root);
      ~ControlServer();

      // Starts listening; port 0 picks a free port. `bind_address` empty
      // means all interfaces, so phones on the same network can connect.
      bool start(int port, const String& bind_address, std::string& error);
      void stop();

      int getPort() const;
      int getNumClients() const;

      // The JSON message handler, exposed for tests. Returns the replies to
      // send back to the sender (broadcasts go out separately).
      std::vector<std::string> handleMessage(const std::string& message);

    private:
      class Impl;
      std::unique_ptr<Impl> impl_;
  };

  // Exposed for tests.
  std::string webSocketAcceptKey(const std::string& client_key);

} // namespace sloppy
