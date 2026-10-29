#pragma once

#include <nxcord/tls.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace nxcord {

// Small RFC 6455 client over TLS. Only what Discord's gateways need:
// text and binary messages, ping/pong, close codes. No extensions.
class WebSocket {
 public:
  enum class Type { Text, Binary };
  using MessageHandler = std::function<void(Type, std::string &&)>;

  // url: wss://host[:port]/path?query
  bool connect(const std::string &url, int timeout_ms = 10000);
  void close(uint16_t code = 1000);

  bool sendText(const std::string &msg) { return sendFrame(0x1, msg.data(), msg.size()); }
  bool sendBinary(const void *data, size_t len) { return sendFrame(0x2, data, len); }

  // Reads whatever is available (waiting up to timeout_ms for the first byte)
  // and dispatches complete messages. Returns false once the connection is gone.
  bool poll(int timeout_ms, const MessageHandler &handler);

  bool connected() const { return _tls.connected() && !_closed; }
  int fd() const { return _tls.fd(); }
  // Close code sent by the server, or 0 if the connection dropped without one.
  uint16_t closeCode() const { return _close_code; }
  const std::string &error() const { return _error; }

 private:
  bool sendFrame(uint8_t opcode, const void *data, size_t len);
  bool sendFrameLocked(uint8_t opcode, const void *data, size_t len);
  bool parseFrames(const MessageHandler &handler);
  void dropConnection();

  TlsConnection _tls;
  std::mutex _send_mutex;
  std::vector<uint8_t> _rx;
  std::string _fragment;
  uint8_t _fragment_op = 0;
  bool _closed = false;
  uint16_t _close_code = 0;
  std::string _error;
};

}  // namespace nxcord
