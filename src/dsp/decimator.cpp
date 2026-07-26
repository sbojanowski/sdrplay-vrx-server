#include "decimator.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

std::vector<float> designLowPassFir(int numTaps, double cutoffHz, double sampleRateHz) {
  std::vector<float> taps(static_cast<size_t>(numTaps));
  const double fc = cutoffHz / sampleRateHz; // normalized cutoff, 0..0.5
  const double mid = (numTaps - 1) / 2.0;

  double sum = 0;
  for (int n = 0; n < numTaps; ++n) {
    const double x = n - mid;
    // sinc
    const double sinc = (x == 0) ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
    // Hamming window
    const double window = 0.54 - 0.46 * std::cos((2 * M_PI * n) / (numTaps - 1));
    const double tap = sinc * window;
    taps[static_cast<size_t>(n)] = static_cast<float>(tap);
    sum += tap;
  }
  // Normalize for unity DC gain.
  for (int n = 0; n < numTaps; ++n) {
    taps[static_cast<size_t>(n)] = static_cast<float>(taps[static_cast<size_t>(n)] / sum);
  }
  return taps;
}

Decimator::Decimator(std::vector<float> taps, int decimationFactor)
    : taps_(std::move(taps)), factor_(decimationFactor) {
  if (decimationFactor < 1) {
    throw std::runtime_error("Decimator: decimationFactor must be a positive integer, got " +
                              std::to_string(decimationFactor));
  }
  // History holds (numTaps - 1) complex samples = (numTaps - 1) * 2 floats.
  history_.assign((taps_.size() - 1) * 2, 0.0f);
}

std::vector<float> Decimator::process(const float* input, size_t inputLen) {
  const size_t numTaps = taps_.size();
  const size_t histSamples = history_.size() / 2;
  const size_t inSamples = inputLen / 2;

  // Concatenate history + new input so the FIR has full context from sample 0.
  std::vector<float> buf(history_.size() + inputLen);
  std::copy(history_.begin(), history_.end(), buf.begin());
  std::copy(input, input + inputLen, buf.begin() + static_cast<long>(history_.size()));
  const size_t totalSamples = histSamples + inSamples;

  const long rawOutSamples = (static_cast<long>(totalSamples) - static_cast<long>(numTaps) + 1) / factor_;
  const size_t outSamples = static_cast<size_t>(std::max<long>(0, rawOutSamples));

  std::vector<float> out(outSamples * 2);

  for (size_t o = 0; o < outSamples; ++o) {
    const size_t centerIdx = o * static_cast<size_t>(factor_);
    double accI = 0;
    double accQ = 0;
    for (size_t t = 0; t < numTaps; ++t) {
      const size_t sampleIdx = (centerIdx + t) * 2;
      const float tap = taps_[t];
      accI += buf[sampleIdx] * tap;
      accQ += buf[sampleIdx + 1] * tap;
    }
    out[o * 2] = static_cast<float>(accI);
    out[o * 2 + 1] = static_cast<float>(accQ);
  }

  // Save the tail of `buf` as history for next call.
  const size_t consumedSamples = outSamples * static_cast<size_t>(factor_);
  const size_t remainderStart = consumedSamples * 2;
  const size_t remainderLen = buf.size() - remainderStart;
  const size_t newHistLen = std::min(remainderLen, history_.size());
  std::vector<float> newHistory(history_.size(), 0.0f);
  std::copy(buf.end() - static_cast<long>(newHistLen), buf.end(),
            newHistory.end() - static_cast<long>(newHistLen));
  history_ = std::move(newHistory);

  return out;
}
