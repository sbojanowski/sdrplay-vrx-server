#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sdrconnect {

/**
 * Minimal RFC6455 WebSocket client - just enough framing to talk to
 * SDRconnect's WebSocket API (plain ws://, no TLS, no extensions, no
 * fragmentation of outgoing messages). Hand-rolled rather than pulling in a
 * WebSocket library, matching this project's existing style of hand-rolling
 * small protocol primitives on plain POSIX sockets (see rtltcp/server.cpp)
 * instead of adding a dependency that isn't guaranteed present on the ARM
 * boxes this project deploys to.
 */
class WebSocketClient {
public:
  using TextHandler = std::function<void(const std::string& text)>;
  using BinaryHandler = std::function<void(const uint8_t* data, size_t len)>;

  ~WebSocketClient();

  /** TCP-connects and performs the HTTP Upgrade handshake. Throws std::runtime_error on failure. */
  void connect(const std::string& host, uint16_t port);

  /** Sends a single-frame masked TEXT message. */
  void sendText(const std::string& text);

  /**
   * Blocking read loop: dispatches TEXT/BINARY frames to the given handlers
   * as they arrive, replies to PING with PONG automatically, and returns
   * when a CLOSE frame or a recv() EOF/error is seen. Intended to be run on
   * its own thread.
   */
  void runReadLoop(const TextHandler& onText, const BinaryHandler& onBinary);

  /** Sends a CLOSE frame, then shuts down the socket to unblock runReadLoop(). */
  void close();

private:
  void sendFrame(uint8_t opcode, const uint8_t* payload, size_t len);

  int fd_ = -1;
};

} // namespace sdrconnect
