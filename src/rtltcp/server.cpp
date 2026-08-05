#include "server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "protocol.hpp"

RtlTcpServer::RtlTcpServer(VirtualReceiver& vrx, uint16_t port) : vrx_(vrx), port_(port) {}

RtlTcpServer::~RtlTcpServer() { stop(); }

void RtlTcpServer::start() {
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
  // Fan this VRX's decimated IQ out to every currently-connected client.
  vrx_.addIqCallback([this](const uint8_t* data, size_t len) { broadcastIq(data, len); });

  std::cout << "[" << vrx_.name() << "] rtl_tcp server listening on :" << port_ << " (center "
            << (vrx_.getCenterFrequencyHz() / 1e6) << " MHz, " << vrx_.getSampleRateHz() << " sps)\n";

  acceptThread_ = std::thread(&RtlTcpServer::acceptLoop, this);
}

void RtlTcpServer::stop() {
  if (!running_) return;
  running_ = false;

  // Unblocks any in-progress accept() (Linux: closing a listening fd another
  // thread is blocked on in accept() makes it return -1) and rejects further
  // connection attempts.
  if (listenFd_ >= 0) {
    shutdown(listenFd_, SHUT_RDWR);
    close(listenFd_);
    listenFd_ = -1;
  }
  if (acceptThread_.joinable()) acceptThread_.join();

  // acceptThread_ is joined, so no new fds can appear in clientFds_ past this
  // point - safe to snapshot and shut them down. Each client's own detached
  // clientReadLoop() thread is the sole owner of close()-ing its fd (to avoid
  // a double-close/fd-reuse race), so only shutdown() here, not close().
  std::vector<int> fds;
  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    fds = clientFds_;
  }
  for (const int fd : fds) {
    shutdown(fd, SHUT_RDWR);
  }
}

void RtlTcpServer::acceptLoop() {
  while (running_) {
    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    const int fd = accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
    if (fd < 0) {
      if (running_) {
        std::cerr << "[" << vrx_.name() << "] accept() failed: " << std::strerror(errno) << "\n";
      }
      continue; // if !running_, listenFd_ was just closed by stop() - loop will exit on next check
    }

    char addrStr[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &clientAddr.sin_addr, addrStr, sizeof(addrStr));
    std::cout << "[" << vrx_.name() << "] client connected: " << addrStr << ":" << ntohs(clientAddr.sin_port)
              << "\n";

    const auto header = rtltcp::buildDongleInfoHeader();
    send(fd, header.data(), header.size(), 0);

    {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      clientFds_.push_back(fd);
    }

    std::thread(&RtlTcpServer::clientReadLoop, this, fd).detach();
  }
}

void RtlTcpServer::clientReadLoop(int fd) {
  std::vector<uint8_t> recvBuf;
  uint8_t chunk[4096];

  while (true) {
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) break; // 0 = orderly disconnect, <0 = error/shutdown() from stop()

    recvBuf.insert(recvBuf.end(), chunk, chunk + n);

    size_t offset = 0;
    while (recvBuf.size() - offset >= rtltcp::COMMAND_LENGTH) {
      const auto parsed = rtltcp::parseCommand(recvBuf.data() + offset, recvBuf.size() - offset);
      if (parsed) handleCommand(parsed->cmd, parsed->param);
      offset += rtltcp::COMMAND_LENGTH;
    }
    recvBuf.erase(recvBuf.begin(), recvBuf.begin() + static_cast<long>(offset));
  }

  std::cout << "[" << vrx_.name() << "] client disconnected\n";
  removeClient(fd);
  close(fd);
}

void RtlTcpServer::handleCommand(uint8_t cmd, uint32_t param) {
  switch (cmd) {
    case rtltcp::SET_FREQUENCY:
      try {
        vrx_.retune(param);
        std::cout << "[" << vrx_.name() << "] retuned to " << (param / 1e6) << " MHz\n";
      } catch (const std::exception& e) {
        std::cerr << "[" << vrx_.name() << "] retune rejected: " << e.what() << "\n";
      }
      break;

    case rtltcp::SET_SAMPLE_RATE:
      // This starter implementation has a fixed decimation factor set at
      // startup (see config.yaml / VirtualReceiver). A client requesting a
      // different sample rate than what's configured is silently ignored
      // here - log it so it's visible during development.
      if (static_cast<double>(param) != vrx_.getSampleRateHz()) {
        std::cerr << "[" << vrx_.name() << "] client requested sample rate " << param
                   << ", but this VRX is fixed at " << vrx_.getSampleRateHz() << " (ignored)\n";
      }
      break;

    case rtltcp::SET_GAIN_MODE:
    case rtltcp::SET_GAIN:
    case rtltcp::SET_AGC_MODE:
    case rtltcp::SET_FREQUENCY_CORRECTION:
    case rtltcp::SET_TUNER_GAIN_BY_INDEX:
    case rtltcp::SET_BIAS_TEE:
      // No physical tuner behind this VRX - gain/AGC/bias-tee are controlled
      // via the shared SdrplayApiClient's own config.yaml settings, not per
      // rtl_tcp connection. Accepted-and-ignored so clients don't error out
      // on startup.
      break;

    case rtltcp::SET_DIRECT_SAMPLING:
    case rtltcp::SET_OFFSET_TUNING:
      // Both are real-RTL-dongle tuner quirks (bypassing the tuner for HF,
      // and dodging the E4000's DC spike, respectively) - this VRX always
      // emits clean zero-IF baseband via its own DDC chain, so neither
      // concept applies. Accepted-and-ignored, same as above.
      break;

    default:
      std::cout << "[" << vrx_.name() << "] unhandled command 0x" << std::hex << static_cast<int>(cmd)
                << std::dec << " param=" << param << "\n";
  }
}

void RtlTcpServer::broadcastIq(const uint8_t* data, size_t len) {
  std::lock_guard<std::mutex> lock(clientsMutex_);
  for (const int fd : clientFds_) {
    // MSG_DONTWAIT: a slow/stalled client's full kernel send buffer must not
    // block this call - it runs on the SDRplay streaming thread shared by
    // every VRX. Unlike an unbounded userspace queue, this drops the chunk
    // for that one client instead of stalling the whole pipeline or growing
    // memory without bound.
    int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const ssize_t sent = send(fd, data, len, flags);
    if (sent < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        std::cerr << "[" << vrx_.name() << "] write error on fd " << fd << ": " << std::strerror(errno) << "\n";
      }
    } else if (static_cast<size_t>(sent) < len) {
      std::cerr << "[" << vrx_.name() << "] backpressure: short write to fd " << fd << " (" << sent << "/"
                << len << " bytes) - client can't keep up, remainder dropped\n";
    }
  }
}

void RtlTcpServer::removeClient(int fd) {
  std::lock_guard<std::mutex> lock(clientsMutex_);
  clientFds_.erase(std::remove(clientFds_.begin(), clientFds_.end(), fd), clientFds_.end());
}
