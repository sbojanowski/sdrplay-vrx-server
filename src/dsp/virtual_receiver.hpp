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
 * rtl_tcp's 8-bit unsigned IQ format, and emits the result for every
 * registered TCP server (rtl_tcp, SpyServer, ...) to forward to its own
 * connected clients - one VRX can feed more than one protocol endpoint at
 * once, since both currently want the exact same uint8 sample format.
 *
 * Retuning (via the rtl_tcp SET_FREQUENCY command, or SpyServer's
 * IQ_FREQUENCY setting) just updates the NCO's offset - no upstream
 * renegotiation with the SDRplay client needed, as long as the new frequency
 * stays inside the wideband capture's span. Since a VRX is shared by every
 * client connected to any of its endpoints, retuning affects all of them at
 * once - same single-shared-VRX tradeoff rtl_tcp alone already had.
 */
class VirtualReceiver {
public:
  using IqCallback = std::function<void(const uint8_t* data, size_t len)>;

  VirtualReceiver(const VirtualReceiverConfig& cfg, double widebandCenterHz, double widebandSampleRateHz);

  const std::string& name() const { return name_; }
  double getCenterFrequencyHz() const { return centerFrequencyHz_; }
  double getSampleRateHz() const { return widebandSampleRateHz_ / decimationFactor_; }
  /** The wideband capture's own center/span - the outer bounds retune() will accept. Used by SpyServer's DeviceInfo/ClientSync tuning-range fields. */
  double getWidebandCenterHz() const { return widebandCenterHz_; }
  double getWidebandSampleRateHz() const { return widebandSampleRateHz_; }

  /** Handle a client's SET_FREQUENCY request - retune the NCO in place. Throws std::out_of_range if outside the wideband span. */
  void retune(double newCenterFrequencyHz);

  /**
   * Feed a chunk of wideband interleaved IQ (Float32, range roughly -1..1)
   * through this VRX's DDC chain. Invokes every registered IQ callback with
   * 8-bit unsigned interleaved IQ, ready to write straight to an rtl_tcp (or
   * SpyServer UINT8_IQ) socket, if the chain produced any output samples for
   * this chunk.
   */
  void processWidebandChunk(const float* widebandIq, size_t numSamples);

  /** Registers another consumer of this VRX's decimated uint8 IQ output - called once per protocol endpoint (RtlTcpServer, SpyServerServer, ...) wired to this VRX. */
  void addIqCallback(IqCallback cb) { callbacks_.push_back(std::move(cb)); }

private:
  void assertWithinSpan(double freqHz) const;

  std::string name_;
  double widebandCenterHz_;
  double widebandSampleRateHz_;
  double centerFrequencyHz_;
  Nco nco_;
  Decimator decimator_;
  int decimationFactor_;
  std::vector<IqCallback> callbacks_;

  // Reusable scratch buffers, sized on demand - avoids a heap allocation on
  // every wideband chunk (this runs at multi-Msps, once per VRX).
  std::vector<float> mixBuf_;
  std::vector<uint8_t> outBuf_;
};
