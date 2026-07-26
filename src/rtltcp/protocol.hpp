#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

/**
 * Minimal rtl_tcp wire protocol implementation - just enough for SDR++ and
 * AIS-catcher (and any other librtlsdr-based client) to connect, tune, and
 * stream IQ. Reference: librtlsdr's rtl_tcp.c command/header layout.
 */
namespace rtltcp {

enum Command : uint8_t {
  SET_FREQUENCY = 0x01,
  SET_SAMPLE_RATE = 0x02,
  SET_GAIN_MODE = 0x03,
  SET_GAIN = 0x04,
  SET_FREQUENCY_CORRECTION = 0x05,
  SET_IF_STAGE = 0x06,
  SET_TEST_MODE = 0x07,
  SET_AGC_MODE = 0x08,
  SET_DIRECT_SAMPLING = 0x09,
  SET_OFFSET_TUNING = 0x0a,
  SET_RTL_XTAL = 0x0b,
  SET_TUNER_XTAL = 0x0c,
  SET_TUNER_GAIN_BY_INDEX = 0x0d,
  SET_BIAS_TEE = 0x0e,
};

constexpr size_t COMMAND_LENGTH = 5;

struct ParsedCommand {
  uint8_t cmd;
  uint32_t param;
};

/**
 * Build the 12-byte "dongle info" header rtl_tcp sends immediately on
 * connect. Real dongles report a tuner type/gain count here; we report a
 * generic value since there's no physical RTL dongle behind this.
 */
std::array<uint8_t, 12> buildDongleInfoHeader();

/** Parse a 5-byte command buffer (1-byte cmd id, 4-byte big-endian param). */
std::optional<ParsedCommand> parseCommand(const uint8_t* buf, size_t len);

} // namespace rtltcp
