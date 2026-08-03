#include "websocket.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>

namespace sdrconnect {

namespace {

// --- SHA-1 (RFC 3174) - hand-rolled so the WebSocket handshake needs no
// OpenSSL/libcrypto dependency (not guaranteed present on the ARM boxes this
// project deploys to, see websocket.hpp). ---
std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len) {
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

  std::vector<uint8_t> msg(data, data + len);
  const uint64_t bitLen = static_cast<uint64_t>(len) * 8;
  msg.push_back(0x80);
  while (msg.size() % 64 != 56) msg.push_back(0x00);
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFF));

  auto rotl = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };

  for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) |
             (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) | static_cast<uint32_t>(msg[chunk + i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const uint32_t temp = rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  std::array<uint8_t, 20> out{};
  auto put = [&out](size_t offset, uint32_t v) {
    out[offset] = static_cast<uint8_t>(v >> 24);
    out[offset + 1] = static_cast<uint8_t>(v >> 16);
    out[offset + 2] = static_cast<uint8_t>(v >> 8);
    out[offset + 3] = static_cast<uint8_t>(v);
  };
  put(0, h0);
  put(4, h1);
  put(8, h2);
  put(12, h3);
  put(16, h4);
  return out;
}

std::string base64Encode(const uint8_t* data, size_t len) {
  static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);

  size_t i = 0;
  for (; i + 3 <= len; i += 3) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) |
                        static_cast<uint32_t>(data[i + 2]);
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += kTable[(n >> 6) & 0x3F];
    out += kTable[n & 0x3F];
  }
  const size_t remaining = len - i;
  if (remaining == 1) {
    const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += "==";
  } else if (remaining == 2) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += kTable[(n >> 6) & 0x3F];
    out += "=";
  }
  return out;
}

std::string generateWebSocketKey() {
  std::array<uint8_t, 16> raw{};
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& b : raw) b = static_cast<uint8_t>(dist(gen));
  return base64Encode(raw.data(), raw.size());
}

// Case-insensitive search for an HTTP header's value in the raw response text.
std::string findHeaderValue(const std::string& response, const std::string& headerName) {
  size_t pos = 0;
  while (pos < response.size()) {
    const size_t lineEnd = response.find("\r\n", pos);
    const std::string line = response.substr(pos, lineEnd == std::string::npos ? std::string::npos : lineEnd - pos);
    const size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string wantKey = headerName;
      for (auto& c : wantKey) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (key == wantKey) {
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
          value.pop_back();
        return value;
      }
    }
    if (lineEnd == std::string::npos) break;
    pos = lineEnd + 2;
  }
  return "";
}

void sendAll(int fd, const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const ssize_t n = send(fd, data + sent, len - sent, 0);
    if (n <= 0) throw std::runtime_error("send() failed: " + std::string(std::strerror(errno)));
    sent += static_cast<size_t>(n);
  }
}

} // namespace

WebSocketClient::~WebSocketClient() {
  if (fd_ >= 0) ::close(fd_);
}

void WebSocketClient::connect(const std::string& host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const int gaiErr = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result);
  if (gaiErr != 0) {
    throw std::runtime_error("getaddrinfo(\"" + host + "\") failed: " + gai_strerror(gaiErr));
  }

  int fd = -1;
  std::string lastError;
  for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) {
      lastError = std::strerror(errno);
      continue;
    }
    if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    lastError = std::strerror(errno);
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(result);

  if (fd < 0) {
    throw std::runtime_error("connect() to " + host + ":" + std::to_string(port) + " failed: " + lastError);
  }
  fd_ = fd;

  const std::string key = generateWebSocketKey();
  const std::string request = "GET / HTTP/1.1\r\n"
                               "Host: " +
                               host + ":" + std::to_string(port) +
                               "\r\n"
                               "Upgrade: websocket\r\n"
                               "Connection: Upgrade\r\n"
                               "Sec-WebSocket-Key: " +
                               key +
                               "\r\n"
                               "Sec-WebSocket-Version: 13\r\n"
                               "\r\n";
  try {
    sendAll(fd_, reinterpret_cast<const uint8_t*>(request.data()), request.size());

    std::string response;
    char chunk[1024];
    while (response.find("\r\n\r\n") == std::string::npos) {
      const ssize_t n = recv(fd_, chunk, sizeof(chunk), 0);
      if (n <= 0) throw std::runtime_error("connection closed during WebSocket handshake");
      response.append(chunk, static_cast<size_t>(n));
    }

    if (response.find("101") == std::string::npos || response.find(' ') == std::string::npos ||
        response.compare(0, 5, "HTTP/") != 0) {
      throw std::runtime_error("handshake failed, server did not return HTTP 101: " + response.substr(0, 64));
    }
    const size_t statusSpace = response.find(' ');
    const std::string statusCode = response.substr(statusSpace + 1, 3);
    if (statusCode != "101") {
      throw std::runtime_error("handshake failed, server returned status " + statusCode);
    }

    const std::string accept = findHeaderValue(response, "Sec-WebSocket-Accept");
    static const std::string kMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string toHash = key + kMagicGuid;
    const auto digest = sha1(reinterpret_cast<const uint8_t*>(toHash.data()), toHash.size());
    const std::string expected = base64Encode(digest.data(), digest.size());
    if (accept != expected) {
      throw std::runtime_error("handshake failed: Sec-WebSocket-Accept mismatch");
    }
  } catch (...) {
    ::close(fd_);
    fd_ = -1;
    throw;
  }
}

void WebSocketClient::sendFrame(uint8_t opcode, const uint8_t* payload, size_t len) {
  std::vector<uint8_t> frame;
  frame.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F))); // FIN=1, single-frame messages only

  if (len < 126) {
    frame.push_back(static_cast<uint8_t>(0x80 | len)); // MASK=1
  } else if (len <= 0xFFFF) {
    frame.push_back(0x80 | 126);
    frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(len & 0xFF));
  } else {
    frame.push_back(0x80 | 127);
    for (int i = 7; i >= 0; --i) frame.push_back(static_cast<uint8_t>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
  }

  std::array<uint8_t, 4> mask{};
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& m : mask) m = static_cast<uint8_t>(dist(gen));
  frame.insert(frame.end(), mask.begin(), mask.end());

  const size_t headerLen = frame.size();
  frame.resize(headerLen + len);
  for (size_t i = 0; i < len; ++i) frame[headerLen + i] = payload[i] ^ mask[i % 4];

  sendAll(fd_, frame.data(), frame.size());
}

void WebSocketClient::sendText(const std::string& text) {
  sendFrame(0x1, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void WebSocketClient::close() {
  if (fd_ < 0) return;
  try {
    sendFrame(0x8, nullptr, 0);
  } catch (...) {
    // best-effort - the socket may already be broken
  }
  shutdown(fd_, SHUT_RDWR);
  // Deliberately not closing fd_ here - the read loop thread owns close()
  // once its blocking recv() unblocks, avoiding a double-close/fd-reuse race
  // (same reasoning as RtlTcpServer::stop()/clientReadLoop()).
}

void WebSocketClient::runReadLoop(const TextHandler& onText, const BinaryHandler& onBinary) {
  std::vector<uint8_t> buf;
  uint8_t chunk[65536]; // matches the vendor's own C# reference client's receive buffer size

  bool fragActive = false;
  uint8_t fragOpcode = 0;
  std::vector<uint8_t> fragPayload;

  while (true) {
    const ssize_t n = recv(fd_, chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    buf.insert(buf.end(), chunk, chunk + n);

    bool sawClose = false;
    size_t offset = 0;
    while (true) {
      if (buf.size() - offset < 2) break;
      const uint8_t b0 = buf[offset];
      const uint8_t b1 = buf[offset + 1];
      const bool fin = (b0 & 0x80) != 0;
      const uint8_t opcode = b0 & 0x0F;
      const bool masked = (b1 & 0x80) != 0;
      const uint8_t len7 = b1 & 0x7F;

      size_t headerLen = 2;
      uint64_t payloadLen = len7;
      if (len7 == 126) {
        if (buf.size() - offset < headerLen + 2) break;
        payloadLen = (static_cast<uint64_t>(buf[offset + 2]) << 8) | buf[offset + 3];
        headerLen += 2;
      } else if (len7 == 127) {
        if (buf.size() - offset < headerLen + 8) break;
        payloadLen = 0;
        for (int i = 0; i < 8; ++i) payloadLen = (payloadLen << 8) | buf[offset + 2 + i];
        headerLen += 8;
      }
      if (masked) headerLen += 4;

      if (buf.size() - offset < headerLen + payloadLen) break; // wait for the rest of this frame

      const uint8_t* maskKey = masked ? &buf[offset + headerLen - 4] : nullptr;
      const size_t payloadStart = offset + headerLen;

      std::vector<uint8_t> payload(payloadLen);
      for (uint64_t i = 0; i < payloadLen; ++i) {
        payload[i] = buf[payloadStart + i] ^ (masked ? maskKey[i % 4] : 0);
      }

      switch (opcode) {
        case 0x0: // continuation
          if (fragActive) {
            fragPayload.insert(fragPayload.end(), payload.begin(), payload.end());
            if (fin) {
              if (fragOpcode == 0x1 && onText) {
                onText(std::string(fragPayload.begin(), fragPayload.end()));
              } else if (fragOpcode == 0x2 && onBinary) {
                onBinary(fragPayload.data(), fragPayload.size());
              }
              fragActive = false;
              fragPayload.clear();
            }
          }
          break;
        case 0x1: // text
        case 0x2: // binary
          if (!fin) {
            fragActive = true;
            fragOpcode = opcode;
            fragPayload.assign(payload.begin(), payload.end());
          } else {
            if (opcode == 0x1 && onText) {
              onText(std::string(payload.begin(), payload.end()));
            } else if (opcode == 0x2 && onBinary) {
              onBinary(payload.data(), payload.size());
            }
          }
          break;
        case 0x8: // close
          sawClose = true;
          break;
        case 0x9: // ping
          try {
            sendFrame(0xA, payload.data(), payload.size());
          } catch (...) {
          }
          break;
        case 0xA: // pong
        default:
          break;
      }

      offset += headerLen + payloadLen;
      if (sawClose) break;
    }
    buf.erase(buf.begin(), buf.begin() + static_cast<long>(offset));
    if (sawClose) break;
  }

  ::close(fd_);
  fd_ = -1;
}

} // namespace sdrconnect
