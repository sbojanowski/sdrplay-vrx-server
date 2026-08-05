#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "../dsp/virtual_receiver.hpp"

/**
 * Exposes one VirtualReceiver as a SpyServer-protocol TCP server, alongside
 * (not instead of) its RtlTcpServer - any SpyServer client (SDR++, SDR#,
 * CubicSDR, ...) can connect and stream this VRX's IQ using the protocol's
 * 8-bit unsigned format, at roughly a quarter the bandwidth of rtl_tcp's own
 * implicit 8-bit-but-uncompressed-framing... actually both are 8-bit
 * per-sample already (see VirtualReceiver's doc comment) - what SpyServer
 * adds over plain rtl_tcp here is the framed, multi-format, session-negotiated
 * protocol multiple existing SDR apps already speak, not a smaller sample
 * format than what this project already produces.
 *
 * Only IQ streaming is implemented - SpyServer's AF (demodulated audio) and
 * FFT (spectrum display) stream types are out of scope, same as this
 * project's rtl_tcp side only ever dealing in raw IQ. See protocol.hpp for
 * the wire format this was implemented against.
 */
class SpyServerServer {
public:
  SpyServerServer(VirtualReceiver& vrx, uint16_t port);
  ~SpyServerServer();

  SpyServerServer(const SpyServerServer&) = delete;
  SpyServerServer& operator=(const SpyServerServer&) = delete;

  void start();
  void stop();

private:
  struct ClientState {
    bool handshakeDone = false;
    bool streamingEnabled = false;
    uint32_t streamingMode = 0; // bitmask of spyserver::StreamType - defaulted to IQ once handshake completes
    uint32_t sequenceNumber = 0;
  };

  void acceptLoop();
  void clientReadLoop(int fd);
  void handleCommand(int fd, uint32_t commandType, const uint8_t* body, uint32_t bodySize);
  void handleSetSetting(int fd, const uint8_t* body, uint32_t bodySize);
  void handleHello(int fd, const uint8_t* body, uint32_t bodySize);

  /** Builds header+body into one contiguous buffer and sends it in a single send() call, holding clientsMutex_ throughout so it can't interleave with broadcastIq()'s own sends to the same fd. Used for the one-off handshake/control replies (blocking send - fine off the streaming hot path). */
  void sendMessage(int fd, uint32_t messageType, uint32_t streamType, const void* body, uint32_t bodyLen);
  void broadcastIq(const uint8_t* data, size_t len);
  void removeClient(int fd);

  VirtualReceiver& vrx_;
  uint16_t port_;
  int listenFd_ = -1;
  std::atomic<bool> running_{false};
  std::thread acceptThread_;

  std::mutex clientsMutex_;
  std::map<int, ClientState> clients_;
  // Reused across broadcastIq() calls (header+body scratch) - avoids a heap
  // allocation on every wideband chunk, same reasoning as VirtualReceiver's
  // own mixBuf_/outBuf_ (this runs on that same shared streaming thread).
  std::vector<uint8_t> broadcastScratch_;
};
