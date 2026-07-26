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
  /** Center frequency of the wideband capture, in Hz. */
  double centerFrequencyHz = 0;
  /** Sample rate of the wideband capture, in Hz. The RSP ADC's 2 MSPS floor is handled via hardware decimation automatically. */
  double sampleRateHz = 0;
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
   * sdrplay_const::kRsp2Antenna* / kRspDxAntenna* in sdrplay_client.hpp.
   * Unset (nullopt) is ignored on single-antenna models.
   */
  std::optional<int> antenna;
};

struct VirtualReceiverConfig {
  /** Human-readable name, used in logs. */
  std::string name;
  /** TCP port this VRX's rtl_tcp-compatible server listens on. */
  uint16_t tcpPort = 0;
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
  SdrplayConfig sdrplay;
  std::vector<VirtualReceiverConfig> virtualReceivers;
};

/** Load and validate an AppConfig from a YAML file. Throws std::runtime_error on any parse/validation failure. */
AppConfig loadConfig(const std::string& path);
