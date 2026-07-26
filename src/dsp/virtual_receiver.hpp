#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../config.hpp"
#include "decimator.hpp"
#include "nco.hpp"

/**
 * A single channelized "virtual receiver": takes wideband IQ chunks in,
 * frequency-shifts + decimates down to its own configured span, converts to
 * rtl_tcp's 8-bit unsigned IQ format, and emits the result for the
 * corresponding TCP server to forward to connected clients.
 *
 * Retuning (via the rtl_tcp SET_FREQUENCY command) just updates the NCO's
 * offset - no upstream renegotiation with the SDRplay client needed, as long
 * as the new frequency stays inside the wideband capture's span.
 */
class VirtualReceiver {
public:
  using IqCallback = std::function<void(const uint8_t* data, size_t len)>;

  VirtualReceiver(const VirtualReceiverConfig& cfg, double widebandCenterHz, double widebandSampleRateHz);

  const std::string& name() const { return name_; }
  double getCenterFrequencyHz() const { return centerFrequencyHz_; }
  double getSampleRateHz() const { return widebandSampleRateHz_ / decimationFactor_; }

  /** Handle a client's SET_FREQUENCY request - retune the NCO in place. Throws std::out_of_range if outside the wideband span. */
  void retune(double newCenterFrequencyHz);

  /**
   * Feed a chunk of wideband interleaved IQ (Float32, range roughly -1..1)
   * through this VRX's DDC chain. Invokes the IQ callback with 8-bit
   * unsigned interleaved IQ, ready to write straight to an rtl_tcp socket,
   * if the chain produced any output samples for this chunk.
   */
  void processWidebandChunk(const float* widebandIq, size_t numSamples);

  void setIqCallback(IqCallback cb) { onIq_ = std::move(cb); }

private:
  void assertWithinSpan(double freqHz) const;

  std::string name_;
  double widebandCenterHz_;
  double widebandSampleRateHz_;
  double centerFrequencyHz_;
  Nco nco_;
  Decimator decimator_;
  int decimationFactor_;
  IqCallback onIq_;

  // Reusable scratch buffers, sized on demand - avoids a heap allocation on
  // every wideband chunk (this runs at multi-Msps, once per VRX).
  std::vector<float> mixBuf_;
  std::vector<uint8_t> outBuf_;
};
