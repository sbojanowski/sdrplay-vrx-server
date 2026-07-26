#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <sdrplay_api.h>

#include "config.hpp"

/**
 * Talks directly to the SDRplay API service (`libsdrplay_api`), the same
 * way SDRplay's own `rsp_tcp.c` does
 * (https://github.com/SDRplay/RSPTCPServer/blob/master/rsp_tcp.c). This
 * class owns the device end-to-end (open API, select device, configure
 * tuner, start streaming) rather than connecting to any other process over
 * the network; only one process can own an RSP at a time.
 *
 * Ported from this project's original Node/koffi FFI prototype - the FFI
 * version had to decode a native struct into a plain JS object, mutate one
 * field, and re-encode the *entire* struct (koffi has no live view into
 * native memory). Here, with the real sdrplay_api.h included directly, we
 * just write through the pointers GetDeviceParams() hands back, exactly
 * like rsp_tcp.c's own `chParams->tunerParams.rfFreq.rfHz = freq;` pattern.
 *
 * NOT VERIFIED AGAINST A REAL BUILD OF sdrplay_api.h - see cpp/README.md's
 * "Known risk" section before trusting this against real hardware.
 */
class SdrplayApiClient {
public:
  using IqCallback = std::function<void(const float* interleavedIq, size_t numSamples)>;

  explicit SdrplayApiClient(const SdrplayConfig& cfg);
  ~SdrplayApiClient();

  SdrplayApiClient(const SdrplayApiClient&) = delete;
  SdrplayApiClient& operator=(const SdrplayApiClient&) = delete;

  void connect();
  void close();

  /** Retune the RSP's RF center frequency while streaming. */
  void retune(double centerFrequencyHz);
  /** Manual IF gain reduction, in dB. Has no effect while AGC is enabled. */
  void setGainReduction(int gRdB);
  void setLnaState(int lnaState);
  void setAgc(bool enabled, std::optional<int> setPointDbfs = std::nullopt);
  /** Enable/disable bias-tee, if the selected model supports it. */
  void setBiasTee(bool enabled);

  /** Invoked with normalized interleaved IQ (roughly -1..1) for every wideband chunk the driver delivers. */
  void setIqCallback(IqCallback cb) { onIq_ = std::move(cb); }

private:
  static void streamCallbackTrampoline(short* xi, short* xq, sdrplay_api_StreamCbParamsT* params,
                                        unsigned int numSamples, unsigned int reset, void* cbContext);
  static void eventCallbackTrampoline(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner,
                                       sdrplay_api_EventParamsT* params, void* cbContext);
  void handleStream(const short* xi, const short* xq, unsigned int numSamples);
  void handleEvent(int eventId, sdrplay_api_EventParamsT* params);

  void configureInitialParams();
  /** Bias-T lives in a different sub-struct per model - see sdrplay_api_rsp*.h. */
  void applyBiasTField(sdrplay_api_RxChannelParamsT* rx);
  /** Antenna selection is device-level on RSPdx/RSPdxR2 but per-channel on RSP2. */
  void applyAntennaField(sdrplay_api_DevParamsT* devParams, sdrplay_api_RxChannelParamsT* rx);
  void checkOrThrow(sdrplay_api_ErrT err, const char* what);
  void update(uint32_t reasonForUpdate, uint32_t reasonForUpdateExt1 = 0);

  SdrplayConfig cfg_;
  bool opened_ = false;
  bool deviceSelected_ = false;
  sdrplay_api_DeviceT device_{};
  sdrplay_api_DeviceParamsT* deviceParams_ = nullptr;

  IqCallback onIq_;
  std::vector<float> sampleBuf_; // reusable scratch for the interleaved float conversion, grown on demand
};
