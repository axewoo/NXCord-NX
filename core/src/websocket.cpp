#include <nxcord/log.hpp>
#include <nxcord/util.hpp>
#include <nxcord/websocket.hpp>

#include <mbedtls/base64.h>
#include <mbedtls/version.h>
#include <mbedtls/sha1.h>

#include <chrono>
#include <cstring>

namespace nxcord {

namespace {

constexpr size_t kMaxMessage = 8 * 1024 * 1024;

std::string base64(const uint8_t *data, size_t len) {
  std::string out(((len + 2) / 3) * 4 + 1, '\0');
  size_t olen = 0;
  mbedtls_base64_encode(reinterpret_cast<unsigned char *>(out.data()), out.size(), &olen, data, len);
  out.resize(olen);
  return out;
}

std::string acceptKey(const std::string &key) {
  std::string s = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char sha[20];
#if MBEDTLS_VERSION_MAJOR >= 3
  mbedtls_sha1(reinterpret_cast<const unsigned char *>(s.data()), s.size(), sha);
#else
  mbedtls_sha1_ret(reinterpret_cast<const unsigned char *>(s.data()), s.size(), sha);
#endif
  return base64(sha, sizeof(sha));
}

bool iequalsPrefix(const std::string &line, const char *name) {
  size_t n = strlen(name);
  if (line.size() < n) return false;
  for (size_t i = 0; i < n; i++) {
    if (tolower(static_cast<unsigned char>(line[i])) != tolower(static_cast<unsigned char>(name[i]))) return false;
  }
  return true;
}

}  // namespace

bool WebSocket::connect(const std::string &url, int timeout_ms) {
  // Hold the send lock so other threads can't write while the TLS state is rebuilt.
  std::scoped_lock lock(_send_mutex);
  _rx.clear();
  _fragment.clear();
  _closed = false;
  _close_code = 0;
  _error.clear();

  std::string rest;
  if (url.rfind("wss://", 0) == 0) {
    rest = url.substr(6);
  } else {
    _error = "only wss:// is supported";
    return false;
  }
  size_t slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
  std::string host = hostport;
  uint16_t port = 443;
  if (size_t colon = hostport.find(':'); colon != std::string::npos) {
    host = hostport.substr(0, colon);
    port = static_cast<uint16_t>(std::stoi(hostport.substr(colon + 1)));
  }

  if (!_tls.connect(host, port, timeout_ms)) {
    _error = _tls.error();
    return false;
  }

  uint8_t nonce[16];
  randomBytes(nonce, sizeof(nonce));
  std::string key = base64(nonce, sizeof(nonce));
  std::string req = "GET " + path + " HTTP/1.1\r\n"
                    "Host: " + host + "\r\n"
                    "User-Agent: DiscordBot (https://github.com/axewoo/NXCord-NX, 1.0)\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: " + key + "\r\n"
                    "Sec-WebSocket-Version: 13\r\n\r\n";
  if (!_tls.writeAll(req.data(), req.size())) {
    _error = _tls.error();
    _tls.close();
    return false;
  }

  // Read the HTTP response headers; anything after them is websocket data.
  std::string resp;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  char buf[1024];
  size_t header_end;
  while ((header_end = resp.find("\r\n\r\n")) == std::string::npos) {
    if (std::chrono::steady_clock::now() > deadline || resp.size() > 16384) {
      _error = "websocket handshake timed out";
      _tls.close();
      return false;
    }
    int n = _tls.read(buf, sizeof(buf));
    if (n < 0) {
      _error = "websocket handshake: " + _tls.error();
      _tls.close();
      return false;
    }
    if (n == 0) {
      if (!_tls.hasPending()) waitReadable(_tls.fd(), 100);
      continue;
    }
    resp.append(buf, n);
  }

  std::string headers = resp.substr(0, header_end);
  _rx.assign(resp.begin() + header_end + 4, resp.end());
  if (headers.rfind("HTTP/1.1 101", 0) != 0) {
    _error = "websocket upgrade refused: " + headers.substr(0, headers.find("\r\n"));
    _tls.close();
    return false;
  }
  bool accepted = false;
  std::string expected = acceptKey(key);
  size_t pos = 0;
  while (pos < headers.size()) {
    size_t eol = headers.find("\r\n", pos);
    std::string line = headers.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    if (iequalsPrefix(line, "sec-websocket-accept:")) {
      std::string value = line.substr(21);
      value.erase(0, value.find_first_not_of(' '));
      accepted = value == expected;
    }
    if (eol == std::string::npos) break;
    pos = eol + 2;
  }
  if (!accepted) {
    _error = "bad Sec-WebSocket-Accept";
    _tls.close();
    return false;
  }
  return true;
}

void WebSocket::close(uint16_t code) {
  std::scoped_lock lock(_send_mutex);
  if (_tls.connected() && !_closed) {
    uint8_t payload[2] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code)};
    sendFrameLocked(0x8, payload, sizeof(payload));
  }
  _closed = true;
  _tls.close();
}

bool WebSocket::sendFrame(uint8_t opcode, const void *data, size_t len) {
  std::scoped_lock lock(_send_mutex);
  return sendFrameLocked(opcode, data, len);
}

bool WebSocket::sendFrameLocked(uint8_t opcode, const void *data, size_t len) {
  if (!_tls.connected() || _closed) return false;
  std::vector<uint8_t> frame;
  frame.reserve(len + 14);
  frame.push_back(0x80 | opcode);
  if (len < 126) {
    frame.push_back(0x80 | static_cast<uint8_t>(len));
  } else if (len < 65536) {
    frame.push_back(0x80 | 126);
    frame.push_back(static_cast<uint8_t>(len >> 8));
    frame.push_back(static_cast<uint8_t>(len));
  } else {
    frame.push_back(0x80 | 127);
    for (int i = 7; i >= 0; i--) frame.push_back(static_cast<uint8_t>(static_cast<uint64_t>(len) >> (i * 8)));
  }
  uint8_t mask[4];
  randomBytes(mask, sizeof(mask));
  frame.insert(frame.end(), mask, mask + 4);
  auto *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; i++) frame.push_back(p[i] ^ mask[i & 3]);
  if (!_tls.writeAll(frame.data(), frame.size())) {
    _error = _tls.error();
    return false;
  }
  return true;
}

void WebSocket::dropConnection() {
  std::scoped_lock lock(_send_mutex);
  _tls.close();
}

bool WebSocket::poll(int timeout_ms, const MessageHandler &handler) {
  if (!_tls.connected()) return false;
  if (!_rx.empty() && !parseFrames(handler)) return false;
  bool pending;
  {
    std::scoped_lock lock(_send_mutex);
    pending = _tls.hasPending();
  }
  if (!pending) {
    int w = waitReadable(_tls.fd(), timeout_ms);
    if (w < 0) {
      _error = "socket error";
      dropConnection();
      return false;
    }
    if (w == 0) return true;
  }
  uint8_t buf[4096];
  for (;;) {
    // mbedTLS contexts aren't safe for concurrent read and write, so reads
    // take the same lock as sends.
    int n;
    {
      std::scoped_lock lock(_send_mutex);
      n = _tls.read(buf, sizeof(buf));
    }
    if (n < 0) {
      _error = _tls.error();
      dropConnection();
      return false;
    }
    if (n == 0) break;
    _rx.insert(_rx.end(), buf, buf + n);
    if (_rx.size() > kMaxMessage + 16) {
      _error = "message too large";
      dropConnection();
      return false;
    }
  }
  return parseFrames(handler) && _tls.connected();
}

bool WebSocket::parseFrames(const MessageHandler &handler) {
  size_t off = 0;
  for (;;) {
    size_t avail = _rx.size() - off;
    if (avail < 2) break;
    const uint8_t *p = _rx.data() + off;
    bool fin = p[0] & 0x80;
    uint8_t opcode = p[0] & 0x0f;
    bool masked = p[1] & 0x80;
    uint64_t len = p[1] & 0x7f;
    size_t hdr = 2;
    if (len == 126) {
      if (avail < 4) break;
      len = (uint64_t(p[2]) << 8) | p[3];
      hdr = 4;
    } else if (len == 127) {
      if (avail < 10) break;
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | p[2 + i];
      hdr = 10;
    }
    if (len > kMaxMessage) {
      _error = "frame too large";
      dropConnection();
      return false;
    }
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
      if (avail < hdr + 4) break;
      memcpy(mask, p + hdr, 4);
      hdr += 4;
    }
    if (avail < hdr + len) break;
    std::string payload(reinterpret_cast<const char *>(p + hdr), static_cast<size_t>(len));
    if (masked) {
      for (size_t i = 0; i < payload.size(); i++) payload[i] ^= mask[i & 3];
    }
    off += hdr + static_cast<size_t>(len);

    switch (opcode) {
      case 0x0:  // continuation
        _fragment += payload;
        if (fin) {
          handler(_fragment_op == 0x2 ? Type::Binary : Type::Text, std::move(_fragment));
          _fragment.clear();
        }
        break;
      case 0x1:
      case 0x2:
        if (fin) {
          handler(opcode == 0x2 ? Type::Binary : Type::Text, std::move(payload));
        } else {
          _fragment = std::move(payload);
          _fragment_op = opcode;
        }
        break;
      case 0x8:  // close
        _close_code = payload.size() >= 2
                          ? static_cast<uint16_t>((uint8_t(payload[0]) << 8) | uint8_t(payload[1]))
                          : 1005;
        log::info("websocket closed by server: %u %s", _close_code,
                  payload.size() > 2 ? payload.substr(2).c_str() : "");
        _closed = true;
        _rx.clear();
        dropConnection();
        return false;
      case 0x9:  // ping
        sendFrame(0xA, payload.data(), payload.size());
        break;
      default:  // pong or reserved
        break;
    }
    if (!_tls.connected()) return false;
  }
  _rx.erase(_rx.begin(), _rx.begin() + off);
  return true;
}

}  // namespace nxcord
