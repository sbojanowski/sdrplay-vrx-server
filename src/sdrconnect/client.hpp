#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "../config.hpp"
#include "../iq_source.hpp"
#include "websocket.hpp"

namespace sdrconnect {

/**
 * Talks to a running SDRconnect instance (GUI or Headless) over its
 * WebSocket API (https://www.sdrplay.com/docs/SDRconnect_WebSocket_API.pdf)
 * instead of opening the RSP directly - lets this server pull wideband IQ
 * from a device owned by another process, possibly on another machine.
 *
 * Only the Primary Device's raw IQ stream is consumed (binary message type
 * 2) - audio/spectrum/secondary-tuner messages are ignored. The API exposes
 * no property for IF-gain AGC, bias-tee, or PPM correction, so those
 * SdrplayConfig fields have no effect here - see config.hpp's SdrConnectConfig
 * doc comment.
 */
class SdrConnectClient : public IqSource {
public:
  SdrConnectClient(const SdrplayConfig& deviceCfg, const SdrConnectConfig& connCfg);
  ~SdrConnectClient() override;

  SdrConnectClient(const SdrConnectClient&) = delete;
  SdrConnectClient& operator=(const SdrConnectClient&) = delete;

  void connect() override;
  void close() override;
  void setIqCallback(IqCallback cb) override { onIq_ = std::move(cb); }

private:
  void sendSetProperty(const std::string& property, const std::string& value, const std::string& device = "primary");
  void sendControl(const std::string& eventType, const std::string& value);
  void handleTextMessage(const std::string& json);
  void handleBinaryMessage(const uint8_t* data, size_t len);

  SdrplayConfig deviceCfg_;
  SdrConnectConfig connCfg_;
  WebSocketClient ws_;
  std::thread readThread_;
  std::atomic<bool> running_{false};
  IqCallback onIq_;
  std::vector<float> sampleBuf_; // reusable scratch for the interleaved float conversion, grown on demand
};

} // namespace sdrconnect
