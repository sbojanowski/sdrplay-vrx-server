#include "server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "protocol.hpp"

namespace {
// Every protocol struct is all-uint32_t fields, so none of this should ever
// need #pragma pack - these just catch it at compile time if some exotic ABI
// disagrees, same spirit as this project's other wire-format code.
static_assert(sizeof(spyserver::ClientHandshake) == 4, "unexpected padding in ClientHandshake");
static_assert(sizeof(spyserver::CommandHeader) == 8, "unexpected padding in CommandHeader");
static_assert(sizeof(spyserver::SettingTarget) == 8, "unexpected padding in SettingTarget");
static_assert(sizeof(spyserver::MessageHeader) == 20, "unexpected padding in MessageHeader");
static_assert(sizeof(spyserver::DeviceInfo) == 48, "unexpected padding in DeviceInfo");
static_assert(sizeof(spyserver::ClientSync) == 36, "unexpected padding in ClientSync");
} // namespace

SpyServerServer::SpyServerServer(VirtualReceiver& vrx, uint16_t port) : vrx_(vrx), port_(port) {}

SpyServerServer::~SpyServerServer() { stop(); }

void SpyServerServer::start() {
  listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd_ < 0) {
    throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
  }

  const int opt = 1;
  setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    const std::string err = std::strerror(errno);
    close(listenFd_);
    throw std::runtime_error("bind() to port " + std::to_string(port_) + " failed: " + err);
  }
  if (listen(listenFd_, 8) < 0) {
    const std::string err = std::strerror(errno);
    close(listenFd_);
    throw std::runtime_error("listen() failed: " + err);
  }

  running_ = true;
  // Fan this VRX's decimated IQ out to every connected+streaming-enabled
  // SpyServer client - added alongside RtlTcpServer's own callback, not in
  // place of it (see VirtualReceiver::addIqCallback).
  vrx_.addIqCallback([this](const uint8_t* data, size_t len) { broadcastIq(data, len); });

  std::cout << "[" << vrx_.name() << "] SpyServer listening on :" << port_ << " (center "
            << (vrx_.getCenterFrequencyHz() / 1e6) << " MHz, " << vrx_.getSampleRateHz() << " sps, 8-bit IQ)\n";

  acceptThread_ = std::thread(&SpyServerServer::acceptLoop, this);
}

void SpyServerServer::stop() {
  if (!running_) return;
  running_ = false;

  if (listenFd_ >= 0) {
    shutdown(listenFd_, SHUT_RDWR);
    close(listenFd_);
    listenFd_ = -1;
  }
  if (acceptThread_.joinable()) acceptThread_.join();

  // acceptThread_ is joined, so no new fds can appear in clients_ past this
  // point - safe to snapshot and shut them down. Each client's own detached
  // clientReadLoop() thread is the sole owner of close()-ing its fd (to avoid
  // a double-close/fd-reuse race), so only shutdown() here, not close() -
  // same reasoning as RtlTcpServer::stop().
  std::vector<int> fds;
  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    fds.reserve(clients_.size());
    for (const auto& [fd, state] : clients_) fds.push_back(fd);
  }
  for (const int fd : fds) {
    shutdown(fd, SHUT_RDWR);
  }
}

void SpyServerServer::acceptLoop() {
  while (running_) {
    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    const int fd = accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
    if (fd < 0) {
      if (running_) {
        std::cerr << "[" << vrx_.name() << "] SpyServer accept() failed: " << std::strerror(errno) << "\n";
      }
      continue; // if !running_, listenFd_ was just closed by stop() - loop will exit on next check
    }

    char addrStr[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &clientAddr.sin_addr, addrStr, sizeof(addrStr));
    std::cout << "[" << vrx_.name() << "] SpyServer client connecting: " << addrStr << ":"
              << ntohs(clientAddr.sin_port) << "\n";

    // Unlike rtl_tcp (server speaks first), SpyServer clients send Hello
    // first - nothing to send here until handleHello() replies to it.
    {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      clients_[fd] = ClientState{};
    }

    std::thread(&SpyServerServer::clientReadLoop, this, fd).detach();
  }
}

void SpyServerServer::clientReadLoop(int fd) {
  std::vector<uint8_t> recvBuf;
  uint8_t chunk[4096];
  bool protocolError = false;

  while (!protocolError) {
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) break; // 0 = orderly disconnect, <0 = error/shutdown() from stop()

    recvBuf.insert(recvBuf.end(), chunk, chunk + n);

    size_t offset = 0;
    while (recvBuf.size() - offset >= sizeof(spyserver::CommandHeader)) {
      spyserver::CommandHeader hdr;
      std::memcpy(&hdr, recvBuf.data() + offset, sizeof(hdr));

      if (hdr.bodySize > spyserver::kMaxCommandBodySize) {
        std::cerr << "[" << vrx_.name() << "] SpyServer client sent an oversized command body (" << hdr.bodySize
                   << " bytes, max " << spyserver::kMaxCommandBodySize << ") - disconnecting\n";
        protocolError = true;
        break;
      }
      if (recvBuf.size() - offset < sizeof(hdr) + hdr.bodySize) break; // wait for the rest of this command

      handleCommand(fd, hdr.commandType, recvBuf.data() + offset + sizeof(hdr), hdr.bodySize);
      offset += sizeof(hdr) + hdr.bodySize;
    }
    recvBuf.erase(recvBuf.begin(), recvBuf.begin() + static_cast<long>(offset));
  }

  std::cout << "[" << vrx_.name() << "] SpyServer client disconnected\n";
  removeClient(fd);
  close(fd);
}

void SpyServerServer::handleCommand(int fd, uint32_t commandType, const uint8_t* body, uint32_t bodySize) {
  if (commandType == static_cast<uint32_t>(spyserver::CommandType::Hello)) {
    handleHello(fd, body, bodySize);
  } else if (commandType == static_cast<uint32_t>(spyserver::CommandType::SetSetting)) {
    handleSetSetting(fd, body, bodySize);
  } else if (commandType == static_cast<uint32_t>(spyserver::CommandType::Ping)) {
    sendMessage(fd, spyserver::kMsgTypePong, spyserver::kStreamTypeStatus, nullptr, 0);
  } else {
    std::cout << "[" << vrx_.name() << "] SpyServer client sent unhandled command type " << commandType << "\n";
  }
}

void SpyServerServer::handleHello(int fd, const uint8_t* body, uint32_t bodySize) {
  if (bodySize < sizeof(spyserver::ClientHandshake)) {
    std::cerr << "[" << vrx_.name() << "] SpyServer client sent a malformed Hello (body too short) - ignoring\n";
    return;
  }
  spyserver::ClientHandshake handshake;
  std::memcpy(&handshake, body, sizeof(handshake));

  // Trailing bytes (BodySize - sizeof(ClientHandshake)) are the client's own
  // app name, not null-terminated - logged only, no protocol meaning here.
  const std::string appName(reinterpret_cast<const char*>(body + sizeof(handshake)), bodySize - sizeof(handshake));
  std::cout << "[" << vrx_.name() << "] SpyServer client identified as \"" << appName << "\" (protocol 0x" << std::hex
            << handshake.protocolVersion << std::dec << ")\n";

  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    auto it = clients_.find(fd);
    if (it == clients_.end()) return; // disconnected already
    it->second.handshakeDone = true;
    // Default to IQ, the only stream type this server implements - a client
    // that never sends SettingType::StreamingMode still gets IQ once it sets
    // StreamingEnabled, matching how SDR++'s own client only ever toggles
    // StreamingEnabled and relies on this default (see spyserver_client.cpp).
    it->second.streamingMode = spyserver::kStreamTypeIq;
  }

  const double loLo = vrx_.getWidebandCenterHz() - vrx_.getWidebandSampleRateHz() / 2;
  const double loHi = vrx_.getWidebandCenterHz() + vrx_.getWidebandSampleRateHz() / 2;

  spyserver::DeviceInfo info{};
  // RtlSdr: the simplest of the enum's gain models, and thematically
  // consistent with this project already presenting every VRX as an
  // rtl_tcp-compatible device on its other endpoint.
  info.deviceType = static_cast<uint32_t>(spyserver::DeviceType::RtlSdr);
  info.deviceSerial = 0; // no physical device/serial behind a VRX
  info.maximumSampleRate = static_cast<uint32_t>(vrx_.getSampleRateHz());
  info.maximumBandwidth = static_cast<uint32_t>(vrx_.getSampleRateHz());
  info.decimationStageCount = 0; // fixed decimation set at startup - see SettingType::IqDecimation handling below
  info.gainStageCount = 0;       // no gain concept behind a VRX - see SettingType::Gain handling below
  info.maximumGainIndex = 0;
  info.minimumFrequency = static_cast<uint32_t>(loLo);
  info.maximumFrequency = static_cast<uint32_t>(loHi);
  info.resolution = 1;
  info.minimumIqDecimation = 0;
  // This server only ever sends UINT8_IQ (see class doc comment) - telling
  // well-behaved clients up front means they won't bother asking for
  // anything else via SettingType::IqFormat.
  info.forcedIqFormat = static_cast<uint32_t>(spyserver::StreamFormat::Uint8);
  sendMessage(fd, spyserver::kMsgTypeDeviceInfo, spyserver::kStreamTypeStatus, &info, sizeof(info));

  spyserver::ClientSync sync{};
  sync.canControl = 1;
  sync.gain = 0;
  sync.deviceCenterFrequency = static_cast<uint32_t>(vrx_.getWidebandCenterHz());
  sync.iqCenterFrequency = static_cast<uint32_t>(vrx_.getCenterFrequencyHz());
  sync.fftCenterFrequency = 0; // FFT streaming unsupported
  sync.minimumIqCenterFrequency = static_cast<uint32_t>(loLo);
  sync.maximumIqCenterFrequency = static_cast<uint32_t>(loHi);
  sync.minimumFftCenterFrequency = 0;
  sync.maximumFftCenterFrequency = 0;
  sendMessage(fd, spyserver::kMsgTypeClientSync, spyserver::kStreamTypeStatus, &sync, sizeof(sync));

  // Known simplification: if another client (on this or the rtl_tcp side)
  // retunes this shared VRX later, already-connected SpyServer clients don't
  // get a fresh ClientSync pushed to them - their UI may show a stale
  // frequency even though the actual stream did retune. Same "last write
  // wins, no live notification" tradeoff as the shared-VRX model already has.
}

void SpyServerServer::handleSetSetting(int fd, const uint8_t* body, uint32_t bodySize) {
  if (bodySize < sizeof(spyserver::SettingTarget)) {
    std::cerr << "[" << vrx_.name()
               << "] SpyServer client sent a malformed SetSetting (body too short) - ignoring\n";
    return;
  }
  spyserver::SettingTarget target;
  std::memcpy(&target, body, sizeof(target));

  switch (static_cast<spyserver::SettingType>(target.setting)) {
    case spyserver::SettingType::StreamingMode: {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      auto it = clients_.find(fd);
      if (it != clients_.end()) it->second.streamingMode = target.value;
      if ((target.value & spyserver::kStreamTypeIq) == 0) {
        std::cerr << "[" << vrx_.name() << "] SpyServer client requested stream mode 0x" << std::hex
                   << target.value << std::dec << " (no IQ bit set) - this server only ever sends IQ, "
                   << "nothing will be streamed to it\n";
      }
      break;
    }

    case spyserver::SettingType::StreamingEnabled: {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      auto it = clients_.find(fd);
      if (it != clients_.end()) it->second.streamingEnabled = (target.value != 0);
      break;
    }

    case spyserver::SettingType::IqFrequency:
      try {
        vrx_.retune(target.value);
        std::cout << "[" << vrx_.name() << "] SpyServer client retuned to " << (target.value / 1e6) << " MHz\n";
      } catch (const std::exception& e) {
        std::cerr << "[" << vrx_.name() << "] SpyServer retune rejected: " << e.what() << "\n";
      }
      break;

    case spyserver::SettingType::IqFormat:
      if (target.value != static_cast<uint32_t>(spyserver::StreamFormat::Uint8)) {
        std::cerr << "[" << vrx_.name() << "] SpyServer client requested IQ format " << target.value
                   << ", but this server only ever sends UINT8 (DeviceInfo.ForcedIqFormat already says so) - ignored\n";
      }
      break;

    case spyserver::SettingType::IqDecimation:
      // Same "fixed decimation factor set at startup" simplification as
      // rtl_tcp's SET_SAMPLE_RATE - see RtlTcpServer::handleCommand.
      if (target.value != 0) {
        std::cerr << "[" << vrx_.name() << "] SpyServer client requested IQ decimation " << target.value
                   << ", but this VRX is fixed at " << vrx_.getSampleRateHz() << " sps (ignored)\n";
      }
      break;

    case spyserver::SettingType::Gain:
    case spyserver::SettingType::IqDigitalGain:
      // No physical/digital gain behind a VRX - accepted-and-ignored, same
      // treatment as rtl_tcp's SET_GAIN (see RtlTcpServer::handleCommand).
      break;

    case spyserver::SettingType::FftFormat:
    case spyserver::SettingType::FftFrequency:
    case spyserver::SettingType::FftDecimation:
    case spyserver::SettingType::FftDbOffset:
    case spyserver::SettingType::FftDbRange:
    case spyserver::SettingType::FftDisplayPixels:
      // FFT streaming isn't implemented - see class doc comment.
      break;

    default:
      std::cout << "[" << vrx_.name() << "] SpyServer client set unhandled setting " << target.setting << " = "
                << target.value << "\n";
  }
}

void SpyServerServer::sendMessage(int fd, uint32_t messageType, uint32_t streamType, const void* body,
                                   uint32_t bodyLen) {
  // Holds clientsMutex_ for the whole send() so this can't interleave on the
  // wire with broadcastIq()'s own send() to the same fd from the VRX's
  // streaming thread - both build one contiguous header+body buffer and
  // issue a single send() call specifically so one message can't get sliced
  // by another mid-write.
  std::lock_guard<std::mutex> lock(clientsMutex_);
  auto it = clients_.find(fd);
  if (it == clients_.end()) return; // disconnected already
  const uint32_t seq = it->second.sequenceNumber++;

  spyserver::MessageHeader hdr{};
  hdr.protocolId = spyserver::kProtocolVersion; // best-effort choice - see protocol.hpp's doc comment
  hdr.messageType = messageType;
  hdr.streamType = streamType;
  hdr.sequenceNumber = seq;
  hdr.bodySize = bodyLen;

  std::vector<uint8_t> buf(sizeof(hdr) + bodyLen);
  std::memcpy(buf.data(), &hdr, sizeof(hdr));
  if (bodyLen > 0) std::memcpy(buf.data() + sizeof(hdr), body, bodyLen);

  send(fd, buf.data(), buf.size(), 0);
}

void SpyServerServer::broadcastIq(const uint8_t* data, size_t len) {
  std::lock_guard<std::mutex> lock(clientsMutex_);
  if (clients_.empty()) return;

  broadcastScratch_.resize(sizeof(spyserver::MessageHeader) + len);
  std::memcpy(broadcastScratch_.data() + sizeof(spyserver::MessageHeader), data, len);

  spyserver::MessageHeader hdr{};
  hdr.protocolId = spyserver::kProtocolVersion;
  // No digital gain applied - flags (the upper 16 bits some clients read out
  // of MessageType, see spyserver_client.cpp's mflags) stay 0, matching the
  // plain 1/128 scale VirtualReceiver's uint8 samples already use.
  hdr.messageType = spyserver::kMsgTypeUint8Iq;
  hdr.streamType = spyserver::kStreamTypeIq;
  hdr.bodySize = static_cast<uint32_t>(len);

  for (auto& [fd, state] : clients_) {
    if (!state.handshakeDone || !state.streamingEnabled) continue;
    if ((state.streamingMode & spyserver::kStreamTypeIq) == 0) continue;

    hdr.sequenceNumber = state.sequenceNumber++;
    std::memcpy(broadcastScratch_.data(), &hdr, sizeof(hdr));

    // MSG_DONTWAIT: this runs on the shared upstream streaming thread (see
    // RtlTcpServer::broadcastIq's identical reasoning) - a slow client must
    // not stall every VRX's whole pipeline, so its chunk is dropped instead.
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const ssize_t sent = send(fd, broadcastScratch_.data(), broadcastScratch_.size(), flags);
    if (sent < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        std::cerr << "[" << vrx_.name() << "] SpyServer write error on fd " << fd << ": " << std::strerror(errno)
                   << "\n";
      }
    } else if (static_cast<size_t>(sent) < broadcastScratch_.size()) {
      std::cerr << "[" << vrx_.name() << "] SpyServer backpressure: short write to fd " << fd << " (" << sent
                 << "/" << broadcastScratch_.size() << " bytes) - client can't keep up, remainder dropped\n";
    }
  }
}

void SpyServerServer::removeClient(int fd) {
  std::lock_guard<std::mutex> lock(clientsMutex_);
  clients_.erase(fd);
}
