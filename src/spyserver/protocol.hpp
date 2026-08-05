#pragma once

#include <cstddef>
#include <cstdint>

/**
 * SpyServer wire protocol structures/constants, transcribed from the
 * original author's own header (Youssef Touil, Copyright (C) 2017,
 * "Corrections by Ryzerth"), as vendored into SDR++'s open-source client:
 * https://github.com/AlexandreRouma/SDRPlusPlus/blob/master/source_modules/spyserver_source/src/spyserver_protocol.h
 *
 * No official server-side reference exists (the real Airspy `spyserver`
 * binary is closed-source), so this was implemented against that client's
 * parsing logic instead (spyserver_client.cpp in the same repo) - i.e.
 * against what a real, widely-deployed client actually expects on the wire,
 * rather than against the original server's own (unavailable) source. Two
 * details aren't nailed down by any public source and are this server's own
 * reasonable choice, flagged where used below: the exact value of
 * SpyServerMessageHeader::ProtocolID (SDR++'s client never validates it), and
 * SequenceNumber's exact semantics (treated here as a simple per-client
 * counter, which is all any known client needs from it).
 *
 * All fields are uint32_t, so every struct below is naturally 4-byte-aligned
 * with no compiler-inserted padding on any real ABI - the static_asserts in
 * server.cpp catch it if that's ever not true. Wire format is native
 * little-endian (no htonl/ntohl anywhere in the reference client) - fine
 * since every platform this project targets (x86, ARM) is little-endian.
 */
namespace spyserver {

constexpr uint32_t kProtocolVersion = ((2u) << 24) | ((0u) << 16) | 1700u;

constexpr size_t kMaxCommandBodySize = 256;

enum class DeviceType : uint32_t {
  Invalid = 0,
  AirspyOne = 1,
  AirspyHf = 2,
  RtlSdr = 3,
};

enum class CommandType : uint32_t {
  Hello = 0,
  SetSetting = 2,
  Ping = 3,
};

enum class SettingType : uint32_t {
  StreamingMode = 0,
  StreamingEnabled = 1,
  Gain = 2,

  IqFormat = 100,
  IqFrequency = 101,
  IqDecimation = 102,
  IqDigitalGain = 103,

  FftFormat = 200,
  FftFrequency = 201,
  FftDecimation = 202,
  FftDbOffset = 203,
  FftDbRange = 204,
  FftDisplayPixels = 205,
};

enum StreamType : uint32_t {
  kStreamTypeStatus = 0,
  kStreamTypeIq = 1,
  kStreamTypeAf = 2,
  kStreamTypeFft = 4,
};

enum class StreamFormat : uint32_t {
  Invalid = 0,
  Uint8 = 1,
  Int16 = 2,
  Int24 = 3,
  Float = 4,
  Dint4 = 5,
};

enum MessageType : uint32_t {
  kMsgTypeDeviceInfo = 0,
  kMsgTypeClientSync = 1,
  kMsgTypePong = 2,
  kMsgTypeReadSetting = 3,

  kMsgTypeUint8Iq = 100,
  kMsgTypeInt16Iq = 101,
  kMsgTypeInt24Iq = 102,
  kMsgTypeFloatIq = 103,

  kMsgTypeUint8Af = 200,
  kMsgTypeInt16Af = 201,
  kMsgTypeInt24Af = 202,
  kMsgTypeFloatAf = 203,

  kMsgTypeDint4Fft = 300,
  kMsgTypeUint8Fft = 301,
};

#pragma pack(push, 1)

/** Body of the client's first command (CommandType::Hello), followed by an app-name string of (BodySize - 4) bytes that this server ignores beyond logging it. */
struct ClientHandshake {
  uint32_t protocolVersion;
};

/** Precedes every client->server command's body. */
struct CommandHeader {
  uint32_t commandType;
  uint32_t bodySize;
};

/** Body of a CommandType::SetSetting command. */
struct SettingTarget {
  uint32_t setting;
  uint32_t value;
};

/** Precedes every server->client message's body (DeviceInfo, ClientSync, Pong, or a streamed IQ chunk). */
struct MessageHeader {
  uint32_t protocolId;
  uint32_t messageType;
  uint32_t streamType;
  uint32_t sequenceNumber;
  uint32_t bodySize;
};

/** Body of a MessageType::kMsgTypeDeviceInfo message - sent once, right after a valid Hello. */
struct DeviceInfo {
  uint32_t deviceType;
  uint32_t deviceSerial;
  uint32_t maximumSampleRate;
  uint32_t maximumBandwidth;
  uint32_t decimationStageCount;
  uint32_t gainStageCount;
  uint32_t maximumGainIndex;
  uint32_t minimumFrequency;
  uint32_t maximumFrequency;
  uint32_t resolution;
  uint32_t minimumIqDecimation;
  uint32_t forcedIqFormat;
};

/** Body of a MessageType::kMsgTypeClientSync message - sent once, right after DeviceInfo. */
struct ClientSync {
  uint32_t canControl;
  uint32_t gain;
  uint32_t deviceCenterFrequency;
  uint32_t iqCenterFrequency;
  uint32_t fftCenterFrequency;
  uint32_t minimumIqCenterFrequency;
  uint32_t maximumIqCenterFrequency;
  uint32_t minimumFftCenterFrequency;
  uint32_t maximumFftCenterFrequency;
};

#pragma pack(pop)

} // namespace spyserver
