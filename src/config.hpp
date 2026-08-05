#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * Central configuration for the wideband capture and the virtual receivers
 * (VRXs) channelized out of it. Loaded from a YAML file - see config.yaml
 * for the schema and an annotated example.
 */
struct SdrplayConfig {
  /** Serial number of the RSP to use, if more than one is connected. Empty picks the first device found. */
  std::optional<std::string> serialNumber;
  /** Manual IF gain reduction, in dB (sdrplay_api_GainT.gRdB). Ignored while agc is true. */
  int gainReductionDb = 50;
  /** LNA state index - model- and band-dependent, see the SDRplay API spec's gain tables. */
  int lnaState = 0;
  /** Use the RSP's AGC instead of manual gain. */
  bool agc = false;
  /** AGC setpoint in dBFS, only used when agc is true. Valid range depends on sample rate - see the API spec. */
  int agcSetPointDbfs = -30;
  /** Frequency correction, in ppm. */
  double ppm = 0;
  /** Enable bias-tee, if supported by the selected RSP model (not RSP1). */
  bool biasT = false;
  /**
   * Antenna input to select, on models with more than one (RSP2: A/B,
   * RSPdx/RSPdxR2: A/B/C). Raw sdrplay_api antenna-select value - see
   * sdrplay_const::kRsp2Antenna* / kRspDxAntenna* in sdrplay/client.hpp. Only
   * used by the local api backend - see SdrConnectConfig::antenna for the
   * sdrconnect backend's equivalent. Unset (nullopt) is ignored on
   * single-antenna models.
   */
  std::optional<int> antenna;
};

enum class ConnectionMode { Api, SdrConnect };

/**
 * Connection settings for the SDRconnect WebSocket backend (an alternative to
 * the local sdrplay_api - see https://www.sdrplay.com/docs/SDRconnect_WebSocket_API.pdf).
 * Only used when AppConfig::connection == ConnectionMode::SdrConnect. Note
 * that SDRconnect's WebSocket API exposes no property for IF-gain AGC, bias-
 * tee, or PPM correction - SdrplayConfig's agc/gainReductionDb/
 * agcSetPointDbfs/ppm/biasT/serialNumber/antenna fields are specific to the
 * local api backend and have no effect here. AppConfig::centerFrequencyHz/
 * sampleRateHz are shared across both backends, since they describe the
 * wideband capture layout the VRX decimation math depends on regardless of
 * backend; lnaState (SdrplayConfig) is also honored here, since SDRconnect's
 * lna_state property is the same RF-gain concept.
 */
struct SdrConnectConfig {
  /** Host running SDRconnect (GUI or Headless). */
  std::string host = "127.0.0.1";
  /** SDRconnect's WebSocket API port. */
  uint16_t port = 5454;
  /** Select a specific device by serial number. Unset uses whatever device SDRconnect currently has active. */
  std::optional<std::string> serialNumber;
  /** Network streaming mode, appended to the device selector. Only takes effect if serialNumber is set. */
  std::string networkMode = "Full IQ"; // "Full IQ" | "IQ Lite" | "Compact"
  /**
   * Antenna to select, sent as SDRconnect's "active_antenna" property - a
   * device-reported display name (e.g. "Antenna C"), not the raw numeric
   * code SdrplayConfig::antenna uses for the local api backend. Unset leaves
   * whatever antenna SDRconnect currently has active.
   */
  std::optional<std::string> antenna;
};

struct VirtualReceiverConfig {
  /** Human-readable name, used in logs. */
  std::string name;
  /** TCP port this VRX's rtl_tcp-compatible server listens on. */
  uint16_t tcpPort = 0;
  /** TCP port this VRX's SpyServer-protocol server listens on, streaming 8-bit unsigned IQ. Unset (0) disables this VRX's SpyServer endpoint - only the rtl_tcp one is required. */
  uint16_t spyserverPort = 0;
  /**
   * Initial center frequency for this VRX, in Hz. Must fall within the
   * wideband capture's span. Clients can retune within that span later via
   * the normal rtl_tcp SET_FREQ command.
   */
  double centerFrequencyHz = 0;
  /**
   * Output sample rate, in Hz. Must be an integer divisor of the wideband
   * capture's sample rate in this starter implementation (see
   * dsp/decimator.hpp for why - rational resampling isn't implemented yet).
   */
  double sampleRateHz = 0;
};

struct AppConfig {
  /** Which backend supplies wideband IQ. Defaults to the local sdrplay_api. */
  ConnectionMode connection = ConnectionMode::Api;
  /** Center frequency of the wideband capture, in Hz. Shared across both backends. */
  double centerFrequencyHz = 0;
  /**
   * Sample rate of the wideband capture, in Hz. Shared across both backends.
   * For the local api backend, the RSP ADC's 2 MSPS floor is handled via
   * hardware decimation automatically.
   */
  double sampleRateHz = 0;
  SdrplayConfig sdrplay;
  /** Only populated when connection == ConnectionMode::SdrConnect. */
  std::optional<SdrConnectConfig> sdrconnect;
  std::vector<VirtualReceiverConfig> virtualReceivers;
};

/** Load and validate an AppConfig from a YAML file. Throws std::runtime_error on any parse/validation failure. */
AppConfig loadConfig(const std::string& path);
