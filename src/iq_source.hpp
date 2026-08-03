#pragma once

#include <cstddef>
#include <functional>

/**
 * Backend-agnostic source of wideband IQ, so main.cpp can hold either the
 * local SDRplay API client or a remote SDRconnect WebSocket client behind
 * the same pointer. Only connect()/close()/setIqCallback() are needed here -
 * gain/AGC/bias-tee/retune knobs are configured once at startup by each
 * concrete backend from its own config, not driven from main.cpp.
 */
class IqSource {
public:
  using IqCallback = std::function<void(const float* interleavedIq, size_t numSamples)>;

  virtual ~IqSource() = default;

  virtual void connect() = 0;
  virtual void close() = 0;

  /** Invoked with normalized interleaved IQ (roughly -1..1) for every wideband chunk received. */
  virtual void setIqCallback(IqCallback cb) = 0;
};
