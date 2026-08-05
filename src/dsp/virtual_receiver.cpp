#include "virtual_receiver.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

/** rsp_tcp.c's own bandwidth-from-samplerate ladder is unrelated to this - this is purely the VRX's output filter cutoff. */
std::vector<float> buildTaps(double vrxSampleRateHz, double widebandSampleRateHz) {
  // Cutoff at ~0.45x the output rate leaves guard band before the
  // decimated Nyquist edge - adjust to taste / signal bandwidth.
  return designLowPassFir(129, vrxSampleRateHz * 0.45, widebandSampleRateHz);
}

} // namespace

VirtualReceiver::VirtualReceiver(const VirtualReceiverConfig& cfg, double widebandCenterHz,
                                  double widebandSampleRateHz)
    : name_(cfg.name),
      widebandCenterHz_(widebandCenterHz),
      widebandSampleRateHz_(widebandSampleRateHz),
      centerFrequencyHz_(cfg.centerFrequencyHz),
      nco_(widebandSampleRateHz),
      decimator_(buildTaps(cfg.sampleRateHz, widebandSampleRateHz),
                 static_cast<int>(widebandSampleRateHz / cfg.sampleRateHz)),
      decimationFactor_(static_cast<int>(widebandSampleRateHz / cfg.sampleRateHz)) {
  if (std::fmod(widebandSampleRateHz, cfg.sampleRateHz) != 0.0) {
    throw std::runtime_error("VRX \"" + cfg.name + "\": wideband sample rate " +
                              std::to_string(widebandSampleRateHz) +
                              " is not an integer multiple of requested VRX sample rate " +
                              std::to_string(cfg.sampleRateHz) + ". See dsp/decimator.hpp.");
  }
  assertWithinSpan(centerFrequencyHz_);
  nco_.setOffsetHz(centerFrequencyHz_ - widebandCenterHz_);
}

void VirtualReceiver::retune(double newCenterFrequencyHz) {
  assertWithinSpan(newCenterFrequencyHz);
  centerFrequencyHz_ = newCenterFrequencyHz;
  nco_.setOffsetHz(newCenterFrequencyHz - widebandCenterHz_);
}

void VirtualReceiver::assertWithinSpan(double freqHz) const {
  const double halfSpan = widebandSampleRateHz_ / 2;
  const double lo = widebandCenterHz_ - halfSpan;
  const double hi = widebandCenterHz_ + halfSpan;
  if (freqHz < lo || freqHz > hi) {
    throw std::out_of_range("VRX \"" + name_ + "\": requested frequency " + std::to_string(freqHz) +
                             " Hz is outside the wideband capture's span [" + std::to_string(lo) + ", " +
                             std::to_string(hi) + "] Hz.");
  }
}

void VirtualReceiver::processWidebandChunk(const float* widebandIq, size_t numSamples) {
  // mixInPlace mutates - copy first since the same wideband chunk is fed
  // to multiple VRXs, each with a different NCO offset.
  mixBuf_.assign(widebandIq, widebandIq + numSamples * 2);
  nco_.mixInPlace(mixBuf_.data(), numSamples);

  const std::vector<float> decimated = decimator_.process(mixBuf_.data(), mixBuf_.size());
  if (decimated.empty()) return;

  outBuf_.resize(decimated.size());
  for (size_t i = 0; i < decimated.size(); ++i) {
    // rtl_tcp format: unsigned 8-bit, centered on 128, full scale ~ +-127.
    const float clamped = std::max(-1.0f, std::min(1.0f, decimated[i]));
    outBuf_[i] = static_cast<uint8_t>(std::lround(clamped * 127.0f) + 128);
  }

  for (const auto& cb : callbacks_) cb(outBuf_.data(), outBuf_.size());
}
