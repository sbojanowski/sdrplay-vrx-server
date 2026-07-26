#include "protocol.hpp"

#include <cstring>

namespace rtltcp {

std::array<uint8_t, 12> buildDongleInfoHeader() {
  std::array<uint8_t, 12> buf{};
  std::memcpy(buf.data(), "RTL0", 4); // magic
  // tuner type: 6 = RTLSDR_TUNER_R820T (closest generic default), big-endian uint32 at offset 4
  buf[4] = 0;
  buf[5] = 0;
  buf[6] = 0;
  buf[7] = 6;
  // tuner gain count (0 - report no discrete gain steps), big-endian uint32 at offset 8
  buf[8] = 0;
  buf[9] = 0;
  buf[10] = 0;
  buf[11] = 0;
  return buf;
}

std::optional<ParsedCommand> parseCommand(const uint8_t* buf, size_t len) {
  if (len < COMMAND_LENGTH) return std::nullopt;
  const uint32_t param = (static_cast<uint32_t>(buf[1]) << 24) | (static_cast<uint32_t>(buf[2]) << 16) |
                          (static_cast<uint32_t>(buf[3]) << 8) | static_cast<uint32_t>(buf[4]);
  return ParsedCommand{buf[0], param};
}

} // namespace rtltcp
