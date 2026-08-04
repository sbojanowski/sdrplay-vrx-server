#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "config.hpp"
#include "dsp/virtual_receiver.hpp"
#include "iq_source.hpp"
#include "rtltcp/server.hpp"
#include "sdrconnect/client.hpp"
#include "sdrplay/client.hpp"

namespace {

std::atomic<bool> g_shouldExit{false};

void handleSignal(int) { g_shouldExit = true; }

} // namespace

int main(int argc, char** argv) {
  const std::string configPath = argc > 1 ? argv[1] : "config.yaml";

  AppConfig config;
  try {
    config = loadConfig(configPath);
  } catch (const std::exception& e) {
    std::cerr << "Failed to load config \"" << configPath << "\": " << e.what() << "\n";
    return 1;
  }

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);
  std::signal(SIGPIPE, SIG_IGN); // a client disconnecting mid-write must not kill the process

  std::unique_ptr<IqSource> client;
  if (config.connection == ConnectionMode::Api) {
    client = std::make_unique<SdrplayApiClient>(config.sdrplay, config.centerFrequencyHz, config.sampleRateHz);
  } else {
    client = std::make_unique<sdrconnect::SdrConnectClient>(config.centerFrequencyHz, config.sampleRateHz,
                                                              config.sdrplay, *config.sdrconnect);
  }

  std::vector<std::unique_ptr<VirtualReceiver>> vrxs;
  vrxs.reserve(config.virtualReceivers.size());
  for (const auto& vrxCfg : config.virtualReceivers) {
    vrxs.push_back(std::make_unique<VirtualReceiver>(vrxCfg, config.centerFrequencyHz, config.sampleRateHz));
  }

  std::vector<std::unique_ptr<RtlTcpServer>> servers;
  servers.reserve(vrxs.size());
  for (size_t i = 0; i < vrxs.size(); ++i) {
    servers.push_back(std::make_unique<RtlTcpServer>(*vrxs[i], config.virtualReceivers[i].tcpPort));
  }

  // Fan each wideband IQ chunk out to every VRX's DDC chain - the "single
  // upstream link, N downstream channels" fanout. Each VRX independently
  // mixes+decimates the same wideband samples down to its own slice. Runs
  // on the SDRplay driver's own streaming thread.
  client->setIqCallback([&vrxs](const float* samples, size_t numSamples) {
    for (auto& vrx : vrxs) {
      vrx->processWidebandChunk(samples, numSamples);
    }
  });

  try {
    for (auto& server : servers) server->start();
    client->connect();
  } catch (const std::exception& e) {
    std::cerr << "Startup failed: " << e.what() << "\n";
    for (auto& server : servers) server->stop();
    return 1;
  }

  std::cout << "Wideband capture: " << (config.centerFrequencyHz / 1e6) << " MHz center, "
            << (config.sampleRateHz / 1e6) << " MHz span\n";

  while (!g_shouldExit) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  std::cout << "\nShutting down...\n";
  for (auto& server : servers) server->stop();
  client->close();
  return 0;
}
