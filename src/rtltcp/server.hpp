#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "../dsp/virtual_receiver.hpp"

/**
 * Exposes one VirtualReceiver as an rtl_tcp-protocol TCP server, so any
 * librtlsdr-based client (SDR++, AIS-catcher, GQRX, ...) can connect to it
 * unmodified and treat it as a normal rtl-sdr dongle already tuned to
 * (and retunable within) that VRX's slice of the wideband capture.
 */
class RtlTcpServer {
public:
  RtlTcpServer(VirtualReceiver& vrx, uint16_t port);
  ~RtlTcpServer();

  RtlTcpServer(const RtlTcpServer&) = delete;
  RtlTcpServer& operator=(const RtlTcpServer&) = delete;

  void start();
  void stop();

private:
  void acceptLoop();
  void clientReadLoop(int fd);
  void handleCommand(uint8_t cmd, uint32_t param);
  void broadcastIq(const uint8_t* data, size_t len);
  void removeClient(int fd);

  VirtualReceiver& vrx_;
  uint16_t port_;
  int listenFd_ = -1;
  std::atomic<bool> running_{false};
  std::thread acceptThread_;

  std::mutex clientsMutex_;
  std::vector<int> clientFds_;
};
