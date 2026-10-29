#include <nxcord/log.hpp>
#include <nxcord/tls.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR >= 3
#include <psa/crypto.h>
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>

#include "ca_bundle.inc"

namespace nxcord {

namespace {

std::string mbedError(const char *what, int err) {
  char buf[128];
  mbedtls_strerror(err, buf, sizeof(buf));
  return std::string(what) + ": " + buf;
}

int netSend(void *ctx, const unsigned char *buf, size_t len) {
  int fd = *static_cast<int *>(ctx);
  ssize_t ret = ::send(fd, buf, len, 0);
  if (ret < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  return static_cast<int>(ret);
}

int netRecv(void *ctx, unsigned char *buf, size_t len) {
  int fd = *static_cast<int *>(ctx);
  ssize_t ret = ::recv(fd, buf, len, 0);
  if (ret < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
  }
  if (ret == 0) return MBEDTLS_ERR_NET_CONN_RESET;
  return static_cast<int>(ret);
}

int waitFor(int fd, short events, int timeout_ms) {
  pollfd p{};
  p.fd = fd;
  p.events = events;
  int ret = ::poll(&p, 1, timeout_ms);
  if (ret > 0 && (p.revents & (POLLERR | POLLNVAL))) return -1;
  return ret;
}

// One DRBG and CA chain shared by every connection: parsing the CA bundle and
// seeding costs time and memory we don't want to pay per socket.
struct SharedTls {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_x509_crt ca;
  std::mutex drbg_mutex;
  bool ok = false;

  SharedTls() {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&ca);
#if MBEDTLS_VERSION_MAJOR >= 3
    psa_crypto_init();  // TLS 1.3 in mbedTLS 3.x goes through PSA
#endif
    const char *pers = "nxcord";
    int ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                    reinterpret_cast<const unsigned char *>(pers), strlen(pers));
    if (ret != 0) {
      log::error("%s", mbedError("ctr_drbg_seed", ret).c_str());
      return;
    }
    ret = mbedtls_x509_crt_parse(&ca, reinterpret_cast<const unsigned char *>(kCaBundle),
                                 sizeof(kCaBundle));
    if (ret < 0) {
      log::error("%s", mbedError("x509_crt_parse", ret).c_str());
      return;
    }
    ok = true;
  }

  static int random(void *ctx, unsigned char *out, size_t len) {
    auto *self = static_cast<SharedTls *>(ctx);
    std::scoped_lock lock(self->drbg_mutex);
    return mbedtls_ctr_drbg_random(&self->drbg, out, len);
  }
};

SharedTls &shared() {
  static SharedTls s;
  return s;
}

}  // namespace

struct TlsConnection::Impl {
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  int fd = -1;  // mirrors TlsConnection::_fd for the bio callbacks

  Impl() {
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
  }
  ~Impl() {
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
  }
};

int waitReadable(int fd, int timeout_ms) { return waitFor(fd, POLLIN, timeout_ms); }

TlsConnection::TlsConnection() = default;

TlsConnection::~TlsConnection() { close(); }

bool TlsConnection::connect(const std::string &host, uint16_t port, int timeout_ms) {
  close();
  SharedTls &tls = shared();
  if (!tls.ok) {
    _error = "TLS init failed";
    return false;
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  std::string port_str = std::to_string(port);
  int gai = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
  if (gai != 0 || !res) {
    _error = "DNS lookup failed for " + host;
    return false;
  }

  int fd = -1;
  for (addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int ret = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (ret == 0) break;
    if (errno == EINPROGRESS && waitFor(fd, POLLOUT, timeout_ms) > 0) {
      int err = 0;
      socklen_t len = sizeof(err);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
      if (err == 0) break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd < 0) {
    _error = "TCP connect failed to " + host;
    return false;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  _impl = std::make_unique<Impl>();
  _fd = fd;
  _impl->fd = fd;

  int ret = mbedtls_ssl_config_defaults(&_impl->conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    _error = mbedError("ssl_config_defaults", ret);
    close();
    return false;
  }
  mbedtls_ssl_conf_authmode(&_impl->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&_impl->conf, &tls.ca, nullptr);
  mbedtls_ssl_conf_rng(&_impl->conf, SharedTls::random, &tls);
  if ((ret = mbedtls_ssl_setup(&_impl->ssl, &_impl->conf)) != 0 ||
      (ret = mbedtls_ssl_set_hostname(&_impl->ssl, host.c_str())) != 0) {
    _error = mbedError("ssl_setup", ret);
    close();
    return false;
  }
  mbedtls_ssl_set_bio(&_impl->ssl, &_impl->fd, netSend, netRecv, nullptr);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while ((ret = mbedtls_ssl_handshake(&_impl->ssl)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      _error = mbedError("TLS handshake", ret);
      uint32_t flags = mbedtls_ssl_get_verify_result(&_impl->ssl);
      if (flags != 0) {
        char vbuf[256];
        mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "", flags);
        _error += std::string(" (") + vbuf + ")";
      }
      close();
      return false;
    }
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) {
      _error = "TLS handshake timed out";
      close();
      return false;
    }
    waitFor(_fd, ret == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, static_cast<int>(left));
  }
  return true;
}

void TlsConnection::close() {
  if (_impl && _fd >= 0) mbedtls_ssl_close_notify(&_impl->ssl);
  if (_fd >= 0) {
    ::shutdown(_fd, SHUT_RDWR);
    ::close(_fd);
  }
  _fd = -1;
  _impl.reset();
}

bool TlsConnection::writeAll(const void *data, size_t len, int timeout_ms) {
  if (!_impl) return false;
  auto *p = static_cast<const unsigned char *>(data);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (len > 0) {
    int ret = mbedtls_ssl_write(&_impl->ssl, p, len);
    if (ret > 0) {
      p += ret;
      len -= static_cast<size_t>(ret);
      continue;
    }
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      _error = mbedError("TLS write", ret);
      return false;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      _error = "TLS write timed out";
      return false;
    }
    waitFor(_fd, ret == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, 50);
  }
  return true;
}

int TlsConnection::read(void *buf, size_t len) {
  if (!_impl) return -1;
  int ret = mbedtls_ssl_read(&_impl->ssl, static_cast<unsigned char *>(buf), len);
  if (ret > 0) return ret;
  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
  _error = ret == 0 ? "connection closed" : mbedError("TLS read", ret);
  return -1;
}

bool TlsConnection::hasPending() const {
  return _impl && mbedtls_ssl_get_bytes_avail(&_impl->ssl) > 0;
}

}  // namespace nxcord
