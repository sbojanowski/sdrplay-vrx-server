#pragma once

#include <cmath>
#include <cstddef>

/**
 * Numerically controlled oscillator used to shift a target frequency down
 * to baseband before decimation. This is the "digital downconversion" (DDC)
 * mixer stage of each virtual receiver's DSP chain.
 */
class Nco {
public:
  explicit Nco(double sampleRateHz) : sampleRateHz_(sampleRateHz) {}

  /**
   * Set the frequency this NCO should shift by. Positive offsetHz moves a
   * signal at wideband-center + offsetHz down to baseband.
   */
  void setOffsetHz(double offsetHz) {
    // Negative sign: we want to *cancel* the target's offset from the
    // wideband center, i.e. multiply by e^{-j*2*pi*offset*t}.
    const double phaseIncrement = (-2.0 * M_PI * offsetHz) / sampleRateHz_;
    rotationRe_ = std::cos(phaseIncrement);
    rotationIm_ = std::sin(phaseIncrement);
  }

  /**
   * Mix (frequency-shift) a block of interleaved IQ samples in place.
   * `iq` is [I0, Q0, I1, Q1, ...], numSamples complex samples long.
   *
   * Tracks the mixer as a rotating unit phasor advanced by one complex
   * multiply per sample, rather than calling cos/sin per sample (those only
   * run once, in setOffsetHz()) - matters at multi-Msps wideband rates.
   */
  void mixInPlace(float* iq, size_t numSamples) {
    double phaseRe = phaseRe_;
    double phaseIm = phaseIm_;
    const double rotationRe = rotationRe_;
    const double rotationIm = rotationIm_;

    for (size_t i = 0; i < numSamples; ++i) {
      const size_t idx = i * 2;
      const double sampleI = iq[idx];
      const double sampleQ = iq[idx + 1];

      // Complex multiply: (I + jQ) * (phaseRe + j phaseIm)
      iq[idx] = static_cast<float>(sampleI * phaseRe - sampleQ * phaseIm);
      iq[idx + 1] = static_cast<float>(sampleI * phaseIm + sampleQ * phaseRe);

      // Advance the phasor: (phaseRe + j phaseIm) * (rotationRe + j rotationIm)
      const double nextRe = phaseRe * rotationRe - phaseIm * rotationIm;
      const double nextIm = phaseRe * rotationIm + phaseIm * rotationRe;
      phaseRe = nextRe;
      phaseIm = nextIm;
    }   

    // Repeated complex multiplication drifts off the unit circle over time -
    // renormalize once per chunk (cheap relative to the per-sample loop) to
    // keep the mix from gaining/losing amplitude.
    const double mag = std::hypot(phaseRe, phaseIm);
    phaseRe_ = phaseRe / mag;
    phaseIm_ = phaseIm / mag;
  }

private:
  double sampleRateHz_;
  double phaseRe_ = 1.0;
  double phaseIm_ = 0.0;
  double rotationRe_ = 1.0;
  double rotationIm_ = 0.0;
};
