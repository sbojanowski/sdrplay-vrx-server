#pragma once

#include <cstddef>
#include <vector>

/**
 * Windowed-sinc low-pass FIR filter + integer decimation.
 *
 * NOTE - starter-project simplification: this only handles integer
 * decimation factors (wideband rate / VRX rate must be a whole number, see
 * config.hpp). Real deployments where a VRX wants an arbitrary sample rate
 * need rational resampling (interpolate by L, decimate by M) - swap this
 * module out for a polyphase rational resampler if you need that.
 */

/** Design a windowed-sinc low-pass FIR filter. */
std::vector<float> designLowPassFir(int numTaps, double cutoffHz, double sampleRateHz);

class Decimator {
public:
  Decimator(std::vector<float> taps, int decimationFactor);

  /**
   * Filter + decimate a block of interleaved IQ input (Float32, [I,Q,I,Q,...]).
   * Returns a new, shorter interleaved IQ vector.
   */
  std::vector<float> process(const float* input, size_t inputLen);

private:
  std::vector<float> taps_;
  int factor_;
  /** Rolling history buffer of interleaved I/Q samples for FIR overlap. */
  std::vector<float> history_;
};
