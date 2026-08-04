#include "client.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>

#include <yaml-cpp/yaml.h>

namespace sdrconnect {

SdrConnectClient::SdrConnectClient(double centerFrequencyHz, double sampleRateHz, const SdrplayConfig& deviceCfg,
                                    const SdrConnectConfig& connCfg)
    : centerFrequencyHz_(centerFrequencyHz), sampleRateHz_(sampleRateHz), deviceCfg_(deviceCfg), connCfg_(connCfg) {}

SdrConnectClient::~SdrConnectClient() { close(); }

void SdrConnectClient::sendSetProperty(const std::string& property, const std::string& value,
                                        const std::string& device) {
  std::ostringstream json;
  json << "{\"event_type\":\"set_property\",\"property\":\"" << property << "\",\"value\":\"" << value
       << "\",\"device\":\"" << device << "\"}";
  ws_.sendText(json.str());
}

void SdrConnectClient::sendControl(const std::string& eventType, const std::string& value) {
  std::ostringstream json;
  json << "{\"event_type\":\"" << eventType << "\",\"property\":\"\",\"value\":\"" << value << "\"}";
  ws_.sendText(json.str());
}

void SdrConnectClient::connect() {
  ws_.connect(connCfg_.host, connCfg_.port);

  if (connCfg_.serialNumber) {
    sendControl("selected_device_serial", *connCfg_.serialNumber + ":" + connCfg_.networkMode);
  }

  // device_center_frequency/device_sample_rate/lna_state describe the
  // wideband capture layout the VRX decimation math depends on - honored
  // here the same as the local sdrplay_api backend. AGC/bias-tee/PPM have no
  // equivalent property in this API and are intentionally not sent.
  sendSetProperty("device_center_frequency", std::to_string(static_cast<uint64_t>(centerFrequencyHz_)));
  sendSetProperty("device_sample_rate", std::to_string(sampleRateHz_));
  sendSetProperty("lna_state", std::to_string(deviceCfg_.lnaState));
  if (connCfg_.antenna) {
    sendSetProperty("active_antenna", *connCfg_.antenna);
  }

  sendControl("device_stream_enable", "true");
  sendControl("iq_stream_enable", "true");

  running_ = true;
  readThread_ = std::thread([this] {
    ws_.runReadLoop([this](const std::string& text) { handleTextMessage(text); },
                     [this](const uint8_t* data, size_t len) { handleBinaryMessage(data, len); });
    running_ = false;
    std::cout << "[sdrconnect] connection to " << connCfg_.host << ":" << connCfg_.port << " closed\n";
  });

  std::cout << "[sdrconnect] connected to " << connCfg_.host << ":" << connCfg_.port << "\n";
}

void SdrConnectClient::close() {
  ws_.close();
  if (readThread_.joinable()) readThread_.join();
}

void SdrConnectClient::handleTextMessage(const std::string& json) {
  YAML::Node node;
  try {
    // JSON is a valid YAML flow mapping, so this reuses the yaml-cpp
    // dependency already required for config.yaml instead of adding a JSON
    // library just for these tiny flat control messages.
    node = YAML::Load(json);
  } catch (const YAML::Exception&) {
    return;
  }
  if (!node.IsMap() || !node["event_type"] || !node["property"]) return;

  const std::string eventType = node["event_type"].as<std::string>();
  const std::string property = node["property"].as<std::string>();
  if (eventType == "property_changed" && property == "overload") {
    const std::string value = node["value"] ? node["value"].as<std::string>() : "";
    std::cout << "[sdrconnect] overload event: " << (value == "true" ? "Detected" : "Corrected") << "\n";
  }
}

void SdrConnectClient::handleBinaryMessage(const uint8_t* data, size_t len) {
  if (len < 2) return;
  const uint16_t type = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  if (type != 2) return; // only Primary Device raw IQ is handled - see client.hpp

  const uint8_t* iqBytes = data + 2;
  const size_t iqByteLen = len - 2;
  const size_t numSamples = iqByteLen / 4; // int16 I + int16 Q per complex sample
  if (numSamples == 0) return;

  sampleBuf_.resize(numSamples * 2);
  for (size_t i = 0; i < numSamples; ++i) {
    const auto readLe16 = [](const uint8_t* p) {
      return static_cast<int16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
    };
    const int16_t iRaw = readLe16(iqBytes + i * 4);
    const int16_t qRaw = readLe16(iqBytes + i * 4 + 2);
    // Same normalization as SdrplayApiClient::handleStream (sdrplay_client.cpp).
    sampleBuf_[i * 2] = static_cast<float>(iRaw) / 32768.0f;
    sampleBuf_[i * 2 + 1] = static_cast<float>(qRaw) / 32768.0f;
  }
  if (onIq_) onIq_(sampleBuf_.data(), numSamples);
}

} // namespace sdrconnect
