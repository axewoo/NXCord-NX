#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nxcord {

// A TLS client connection over a TCP socket (mbedTLS).
// connect() blocks; read()/write() are non-blocking once connected.
class TlsConnection {
 public:
  TlsConnection();
  ~TlsConnection();
  TlsConnection(const TlsConnection &) = delete;
  TlsConnection &operator=(const TlsConnection &) = delete;

  bool connect(const std::string &host, uint16_t port, int timeout_ms = 10000);
  void close();

  // Writes the whole buffer, waiting for the socket if needed.
  bool writeAll(const void *data, size_t len, int timeout_ms = 5000);
  // Returns bytes read, 0 if nothing is available right now, -1 on error/EOF.
  int read(void *buf, size_t len);
  // True when mbedTLS still holds decrypted bytes that poll() won't report.
  bool hasPending() const;

  int fd() const { return _fd; }
  bool connected() const { return _fd >= 0; }
  const std::string &error() const { return _error; }

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
  int _fd = -1;
  std::string _error;
};

// Waits until fd is readable (or timeout). Returns >0 readable, 0 timeout, <0 error.
int waitReadable(int fd, int timeout_ms);

}  // namespace nxcord
