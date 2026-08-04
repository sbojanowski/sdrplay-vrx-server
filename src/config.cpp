#include "config.hpp"

#include <cmath>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace {

template <typename T>
T requireField(const YAML::Node& node, const char* key, const std::string& context) {
  const YAML::Node field = node[key];
  if (!field) {
    throw std::runtime_error(context + ": missing required field \"" + key + "\"");
  }
  return field.as<T>();
}

SdrplayConfig parseSdrplayConfig(const YAML::Node& node) {
  SdrplayConfig cfg;
  if (node["serial_number"]) cfg.serialNumber = node["serial_number"].as<std::string>();
  if (node["gain_reduction_db"]) cfg.gainReductionDb = node["gain_reduction_db"].as<int>();
  if (node["lna_state"]) cfg.lnaState = node["lna_state"].as<int>();
  if (node["agc"]) cfg.agc = node["agc"].as<bool>();
  if (node["agc_set_point_dbfs"]) cfg.agcSetPointDbfs = node["agc_set_point_dbfs"].as<int>();
  if (node["ppm"]) cfg.ppm = node["ppm"].as<double>();
  if (node["bias_t"]) cfg.biasT = node["bias_t"].as<bool>();
  if (node["antenna"]) cfg.antenna = node["antenna"].as<int>();
  return cfg;
}

SdrConnectConfig parseSdrConnectConfig(const YAML::Node& node) {
  if (!node) throw std::runtime_error("config: missing required \"sdrconnect\" section (connection: sdrconnect)");

  SdrConnectConfig cfg;
  if (node["host"]) cfg.host = node["host"].as<std::string>();
  if (node["port"]) cfg.port = node["port"].as<uint16_t>();
  if (node["serial_number"]) cfg.serialNumber = node["serial_number"].as<std::string>();
  if (node["network_mode"]) cfg.networkMode = node["network_mode"].as<std::string>();
  if (node["antenna"]) cfg.antenna = node["antenna"].as<std::string>();

  if (cfg.networkMode != "Full IQ" && cfg.networkMode != "IQ Lite" && cfg.networkMode != "Compact") {
    throw std::runtime_error(
        "config: sdrconnect.network_mode must be one of \"Full IQ\", \"IQ Lite\", \"Compact\", got \"" +
        cfg.networkMode + "\"");
  }
  return cfg;
}

VirtualReceiverConfig parseVirtualReceiverConfig(const YAML::Node& node) {
  VirtualReceiverConfig cfg;
  cfg.name = requireField<std::string>(node, "name", "virtual_receivers[]");
  cfg.tcpPort = requireField<uint16_t>(node, "tcp_port", "virtual_receivers[]: \"" + cfg.name + "\"");
  cfg.centerFrequencyHz =
      requireField<double>(node, "center_frequency_hz", "virtual_receivers[]: \"" + cfg.name + "\"");
  cfg.sampleRateHz = requireField<double>(node, "sample_rate_hz", "virtual_receivers[]: \"" + cfg.name + "\"");
  return cfg;
}

} // namespace

AppConfig loadConfig(const std::string& path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const YAML::Exception& e) {
    throw std::runtime_error("config: failed to parse \"" + path + "\": " + e.what());
  }

  AppConfig cfg;
  cfg.centerFrequencyHz = requireField<double>(root, "center_frequency_hz", "config");
  cfg.sampleRateHz = requireField<double>(root, "sample_rate_hz", "config");

  if (root["connection"]) {
    const std::string mode = root["connection"].as<std::string>();
    if (mode == "api") {
      cfg.connection = ConnectionMode::Api;
    } else if (mode == "sdrconnect") {
      cfg.connection = ConnectionMode::SdrConnect;
    } else {
      throw std::runtime_error("config: \"connection\" must be \"api\" or \"sdrconnect\", got \"" + mode + "\"");
    }
  }

  // sdrplay: is required for connection: api, but still parsed (if present)
  // for connection: sdrconnect too, since lna_state is honored by both
  // backends - see SdrConnectConfig's doc comment in config.hpp.
  if (cfg.connection == ConnectionMode::Api) {
    if (!root["sdrplay"]) throw std::runtime_error("config: missing required \"sdrplay\" section (connection: api)");
    cfg.sdrplay = parseSdrplayConfig(root["sdrplay"]);
  } else if (root["sdrplay"]) {
    cfg.sdrplay = parseSdrplayConfig(root["sdrplay"]);
  }

  if (cfg.connection == ConnectionMode::SdrConnect) {
    cfg.sdrconnect = parseSdrConnectConfig(root["sdrconnect"]);
  }

  const YAML::Node vrxNode = root["virtual_receivers"];
  if (!vrxNode || !vrxNode.IsSequence() || vrxNode.size() == 0) {
    throw std::runtime_error("config: missing/empty required \"virtual_receivers\" list");
  }
  for (const auto& entry : vrxNode) {
    cfg.virtualReceivers.push_back(parseVirtualReceiverConfig(entry));
  }

  for (const auto& vrx : cfg.virtualReceivers) {
    if (cfg.sampleRateHz <= 0 || std::fmod(cfg.sampleRateHz, vrx.sampleRateHz) != 0.0) {
      throw std::runtime_error(
          "config: VRX \"" + vrx.name + "\": wideband sample rate " +
          std::to_string(cfg.sampleRateHz) + " is not an integer multiple of requested VRX sample rate " +
          std::to_string(vrx.sampleRateHz) + ". See dsp/decimator.hpp.");
    }
  }

  return cfg;
}
