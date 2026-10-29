#include <nxcord/log.hpp>
#include <nxcord/util.hpp>
#include <nxcord/voice.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sodium.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>

using json = nlohmann::json;

namespace nxcord {

namespace {

constexpr const char *kMode = "aead_xchacha20_poly1305_rtpsize";
constexpr uint8_t kOpusPayloadType = 0x78;
constexpr size_t kRtpHeader = 12;
constexpr uint8_t kSilence[3] = {0xF8, 0xFF, 0xFE};

enum VoiceOp {
  kIdentify = 0,
  kSelectProtocol = 1,
  kReady = 2,
  kHeartbeat = 3,
  kSessionDescription = 4,
  kSpeaking = 5,
  kHeartbeatAck = 6,
  kResume = 7,
  kHello = 8,
  kResumed = 9,
  kClientsConnect = 11,
  kClientDisconnect = 13,
  kDavePrepareTransition = 21,
  kDaveExecuteTransition = 22,
  kDavePrepareEpoch = 24,
  kDaveExternalSender = 25,
  kDaveProposals = 27,
  kDaveAnnounceCommit = 29,
  kDaveWelcome = 30,
};

std::string str(const json &j, const char *key) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return {};
  if (it->is_string()) return it->get<std::string>();
  return it->dump();
}

void put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

void put32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

uint16_t get16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint32_t get32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

VoiceConnection::VoiceConnection(Params params, OpusHandler on_opus)
    : _params(std::move(params)), _on_opus(std::move(on_opus)) {
  static bool sodium_ready = sodium_init() >= 0;
  if (!sodium_ready) log::error("voice: libsodium failed to initialise");
}

VoiceConnection::~VoiceConnection() { stop(); }

void VoiceConnection::start() {
  _running = true;
  _state = State::Connecting;
  _thread = std::thread([this] { run(); });
}

void VoiceConnection::stop() {
  _running = false;
  if (_thread.joinable()) _thread.join();
  _ws.close(1000);
  if (_udp >= 0) ::close(_udp);
  _udp = -1;
  if (_state != State::Failed) _state = State::Closed;
}

std::string VoiceConnection::error() const {
  std::scoped_lock lock(_error_mutex);
  return _error;
}

void VoiceConnection::fail(const std::string &err) {
  log::error("voice: %s", err.c_str());
  {
    std::scoped_lock lock(_error_mutex);
    _error = err;
  }
  _state = State::Failed;
  _running = false;
}

bool VoiceConnection::connectWebsocket(bool resume) {
  std::string endpoint = _params.endpoint;
  if (endpoint.rfind("wss://", 0) == 0) endpoint = endpoint.substr(6);
  std::string url = "wss://" + endpoint + "/?v=8";
  log::info("voice: connecting to %s%s", url.c_str(), resume ? " (resume)" : "");
  if (!_ws.connect(url)) {
    log::error("voice: websocket connect failed: %s", _ws.error().c_str());
    return false;
  }
  _heartbeat_interval = 0;
  _resumable = resume;
  return true;
}

void VoiceConnection::run() {
  // An uncaught exception (bad_alloc, a malformed JSON field) would abort
  // the whole sysmodule; fail just this call instead.
  try {
    runLoop();
  } catch (const std::exception &e) {
    fail(std::string("internal error: ") + e.what());
  }
}

void VoiceConnection::runLoop() {
  if (!connectWebsocket(false)) {
    fail("could not reach the voice server");
    return;
  }
  int reconnects = 0;

  while (_running) {
    int64_t now = nowMs();
    int timeout = 20;
    if (_heartbeat_interval > 0) timeout = static_cast<int>(std::clamp<int64_t>(_next_heartbeat - now, 0, 20));

    pollfd fds[2];
    int nfds = 0;
    fds[nfds++] = {_ws.fd(), POLLIN, 0};
    if (_udp >= 0) fds[nfds++] = {_udp, POLLIN, 0};
    ::poll(fds, nfds, timeout);

    bool alive = _ws.poll(0, [this](WebSocket::Type type, std::string &&msg) {
      if (type == WebSocket::Type::Text) {
        handleText(msg);
      } else {
        handleBinary(msg);
      }
    });
    if (_udp >= 0 && nfds > 1 && (fds[1].revents & POLLIN)) readUdp();

    now = nowMs();
    if (alive && _heartbeat_interval > 0 && now >= _next_heartbeat) sendHeartbeat();
    if (_udp >= 0 && _have_key && now >= _next_keepalive) {
      uint8_t ka[8] = {};
      put32(ka, _keepalive_counter++);
      ::send(_udp, ka, sizeof(ka), 0);
      _next_keepalive = now + 5000;
    }

    if (!alive && _running) {
      uint16_t code = _ws.closeCode();
      switch (code) {
        case 4014:
          fail("disconnected from the voice channel");
          return;
        case 4017:
          fail("voice server requires DAVE (4017)");
          return;
        case 4004:
        case 4006:
        case 4009:
        case 4011:
        case 4016:
        case 4021:
        case 4022:
          fail("voice session ended (" + std::to_string(code) + ")");
          return;
        default:
          break;
      }
      if (++reconnects > 5 || !_have_key) {
        fail("lost connection to the voice server");
        return;
      }
      sleepMs(500 * reconnects);
      if (!connectWebsocket(true)) continue;
    }
  }
}

void VoiceConnection::sendJson(int op, const std::string &d_json) {
  _ws.sendText("{\"op\":" + std::to_string(op) + ",\"d\":" + d_json + "}");
}

void VoiceConnection::sendBinary(uint8_t op, const uint8_t *data, size_t len) {
  std::vector<uint8_t> msg(len + 1);
  msg[0] = op;
  memcpy(msg.data() + 1, data, len);
  _ws.sendBinary(msg.data(), msg.size());
}

void VoiceConnection::sendHeartbeat() {
  json d = {{"t", unixMs()}, {"seq_ack", _seq_ack}};
  sendJson(kHeartbeat, d.dump());
  _next_heartbeat = nowMs() + _heartbeat_interval;
}

void VoiceConnection::handleText(const std::string &msg) {
  json j = json::parse(msg, nullptr, false);
  if (j.is_discarded()) return;
  if (j.contains("seq") && j["seq"].is_number_integer()) _seq_ack = j["seq"].get<int>();
  int op = j.value("op", -1);
  const json &d = j.contains("d") ? j["d"] : json();
  log::debug("voice: op %d", op);

  switch (op) {
    case kHello: {
      _heartbeat_interval = static_cast<int64_t>(d.value("heartbeat_interval", 13750.0));
      _next_heartbeat = nowMs() + _heartbeat_interval;
      if (_resumable) {
        json r = {{"server_id", _params.guild_id},
                  {"session_id", _params.session_id},
                  {"token", _params.token},
                  {"seq_ack", _seq_ack}};
        sendJson(kResume, r.dump());
      } else {
        _seq_ack = -1;
        json id = {{"server_id", _params.guild_id},
                   {"user_id", _params.user_id},
                   {"session_id", _params.session_id},
                   {"token", _params.token},
                   {"max_dave_protocol_version", DaveSession::maxProtocolVersion()}};
        sendJson(kIdentify, id.dump());
      }
      break;
    }
    case kReady: {
      _ssrc = d.value("ssrc", 0u);
      std::string ip = str(d, "ip");
      uint16_t port = d.value("port", 0);
      bool has_mode = false;
      for (auto &m : d["modes"]) has_mode |= m.get<std::string>() == kMode;
      if (!has_mode) {
        fail("voice server offers no supported encryption mode");
        return;
      }
      std::string ext_ip;
      uint16_t ext_port = 0;
      if (!setupUdp(ip, port, ext_ip, ext_port)) {
        fail("UDP setup / IP discovery failed");
        return;
      }
      log::info("voice: ready, ssrc %u, udp %s:%u, external %s:%u", _ssrc, ip.c_str(), port, ext_ip.c_str(),
                ext_port);
      _dave = std::make_unique<DaveSession>(
          _params.user_id, _params.channel_id,
          [this](int dop, const std::string &dj) { sendJson(dop, dj); },
          [this](uint8_t bop, const uint8_t *data, size_t len) { sendBinary(bop, data, len); });
      _dave->setOwnSsrc(_ssrc);
      json sp = {{"protocol", "udp"}, {"data", {{"address", ext_ip}, {"port", ext_port}, {"mode", kMode}}}};
      sendJson(kSelectProtocol, sp.dump());
      break;
    }
    case kSessionDescription: {
      if (str(d, "mode") != kMode) {
        fail("voice server picked an unexpected mode: " + str(d, "mode"));
        return;
      }
      size_t i = 0;
      for (auto &b : d["secret_key"]) {
        if (i < sizeof(_secret_key)) _secret_key[i++] = b.get<uint8_t>();
      }
      {
        std::scoped_lock lock(_send_mutex);
        randomBytes(&_rtp_seq, sizeof(_rtp_seq));
        randomBytes(&_rtp_ts, sizeof(_rtp_ts));
        _nonce = 0;
      }
      _have_key = true;
      _state = State::Connected;
      if (_dave) _dave->onSessionDescription(static_cast<uint16_t>(d.value("dave_protocol_version", 0)));
      log::info("voice: connected (DAVE v%d)", d.value("dave_protocol_version", 0));
      break;
    }
    case kResumed:
      log::info("voice: resumed");
      _state = State::Connected;
      break;
    case kHeartbeatAck:
      break;
    case kSpeaking: {
      uint32_t ssrc = d.value("ssrc", 0u);
      Snowflake user = str(d, "user_id");
      if (ssrc && !user.empty()) {
        std::scoped_lock lock(_users_mutex);
        _ssrc_users[ssrc] = user;
      }
      if (_dave && !user.empty()) _dave->addUser(user);
      break;
    }
    case kClientsConnect:
      if (_dave) {
        for (auto &u : d["user_ids"]) _dave->addUser(u.get<std::string>());
      }
      break;
    case kClientDisconnect: {
      Snowflake user = str(d, "user_id");
      if (_dave) _dave->removeUser(user);
      std::scoped_lock lock(_users_mutex);
      for (auto it = _ssrc_users.begin(); it != _ssrc_users.end();) {
        it = it->second == user ? _ssrc_users.erase(it) : std::next(it);
      }
      break;
    }
    case kDavePrepareTransition:
      if (_dave) _dave->onPrepareTransition(d.value("transition_id", 0), d.value("protocol_version", 0));
      break;
    case kDaveExecuteTransition:
      if (_dave) _dave->onExecuteTransition(d.value("transition_id", 0));
      break;
    case kDavePrepareEpoch:
      if (_dave) _dave->onPrepareEpoch(d.value("epoch", 0ull), d.value("protocol_version", 0));
      break;
    default:
      break;
  }
}

void VoiceConnection::handleBinary(const std::string &msg) {
  if (msg.size() < 3) return;
  auto *p = reinterpret_cast<const uint8_t *>(msg.data());
  _seq_ack = get16(p);
  uint8_t op = p[2];
  const uint8_t *body = p + 3;
  size_t len = msg.size() - 3;
  log::debug("voice: binary op %u (%zu bytes)", op, len);
  if (!_dave) return;

  switch (op) {
    case kDaveExternalSender:
      _dave->onExternalSender(body, len);
      break;
    case kDaveProposals:
      _dave->onProposals(body, len);
      break;
    case kDaveAnnounceCommit:
    case kDaveWelcome: {
      if (len < 2) return;
      uint16_t transition_id = get16(body);
      if (op == kDaveAnnounceCommit) {
        _dave->onAnnounceCommit(transition_id, body + 2, len - 2);
      } else {
        _dave->onWelcome(transition_id, body + 2, len - 2);
      }
      break;
    }
    default:
      break;
  }
}

bool VoiceConnection::setupUdp(const std::string &ip, uint16_t port, std::string &ext_ip, uint16_t &ext_port) {
  if (_udp >= 0) ::close(_udp);
  _udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (_udp < 0) return false;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return false;
  if (::connect(_udp, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) return false;

  uint8_t req[74] = {};
  put16(req, 1);
  put16(req + 2, 70);
  put32(req + 4, _ssrc);

  for (int attempt = 0; attempt < 5; attempt++) {
    ::send(_udp, req, sizeof(req), 0);
    if (waitReadable(_udp, 1000) <= 0) continue;
    uint8_t resp[74];
    ssize_t n = ::recv(_udp, resp, sizeof(resp), 0);
    if (n < 74 || get16(resp) != 2) continue;
    resp[71] = 0;
    ext_ip = reinterpret_cast<const char *>(resp + 8);
    ext_port = get16(resp + 72);
    int flags = ::fcntl(_udp, F_GETFL, 0);
    ::fcntl(_udp, F_SETFL, flags | O_NONBLOCK);
    return true;
  }
  return false;
}

void VoiceConnection::readUdp() {
  uint8_t buf[2048];
  for (;;) {
    ssize_t n = ::recv(_udp, buf, sizeof(buf), 0);
    if (n <= 0) return;
    handleRtp(buf, static_cast<size_t>(n));
  }
}

void VoiceConnection::handleRtp(const uint8_t *pkt, size_t len) {
  if (!_have_key || len < kRtpHeader + 4 + crypto_aead_xchacha20poly1305_ietf_ABYTES) return;
  if ((pkt[0] & 0xC0) != 0x80) return;
  if ((pkt[1] & 0x7F) != kOpusPayloadType) return;  // RTCP and other payloads

  uint16_t seq = get16(pkt + 2);
  uint32_t ssrc = get32(pkt + 8);
  size_t header_len = kRtpHeader + 4 * (pkt[0] & 0x0F);
  bool has_ext = pkt[0] & 0x10;
  size_t ext_body = 0;
  if (has_ext) {
    // rtpsize: the 4-byte extension preamble is part of the unencrypted header.
    if (len < header_len + 4) return;
    ext_body = 4 * get16(pkt + header_len + 2);
    header_len += 4;
  }
  if (len < header_len + 4 + crypto_aead_xchacha20poly1305_ietf_ABYTES) return;

  uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {};
  memcpy(nonce, pkt + len - 4, 4);
  const uint8_t *cipher = pkt + header_len;
  size_t cipher_len = len - header_len - 4;

  uint8_t plain[2048];
  unsigned long long plain_len = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &plain_len, nullptr, cipher, cipher_len, pkt, header_len,
                                                 nonce, _secret_key) != 0) {
    return;
  }
  if (plain_len <= ext_body) return;
  const uint8_t *opus = plain + ext_body;
  size_t opus_len = static_cast<size_t>(plain_len) - ext_body;

  Snowflake user;
  {
    std::scoped_lock lock(_users_mutex);
    auto it = _ssrc_users.find(ssrc);
    if (it != _ssrc_users.end()) user = it->second;
  }
  if (user.empty()) return;  // we don't know who this is yet (no op 5 seen)

  static thread_local std::vector<uint8_t> frame;
  if (_dave) {
    if (!_dave->decrypt(user, opus, opus_len, frame)) return;
  } else {
    frame.assign(opus, opus + opus_len);
  }
  if (_on_opus) _on_opus(ssrc, user, seq, frame.data(), frame.size());
}

void VoiceConnection::setSpeaking(bool speaking) {
  if (_speaking == speaking) return;
  _speaking = speaking;
  json d = {{"speaking", speaking ? 1 : 0}, {"delay", 0}, {"ssrc", _ssrc}};
  sendJson(kSpeaking, d.dump());
}

bool VoiceConnection::sendOpus(const uint8_t *opus, size_t len, uint32_t samples) {
  if (_state != State::Connected || !_have_key || _udp < 0) return false;
  std::scoped_lock lock(_send_mutex);
  setSpeaking(true);

  const uint8_t *payload = opus;
  size_t payload_len = len;
  if (_dave) {
    if (!_dave->encrypt(_ssrc, opus, len, _e2ee_buf)) return false;
    payload = _e2ee_buf.data();
    payload_len = _e2ee_buf.size();
  }

  uint8_t pkt[kRtpHeader + 1500];
  if (payload_len + kRtpHeader + crypto_aead_xchacha20poly1305_ietf_ABYTES + 4 > sizeof(pkt)) return false;
  pkt[0] = 0x80;
  pkt[1] = kOpusPayloadType;
  put16(pkt + 2, _rtp_seq++);
  put32(pkt + 4, _rtp_ts);
  put32(pkt + 8, _ssrc);
  _rtp_ts += samples;

  uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {};
  put32(nonce, _nonce);
  unsigned long long clen = 0;
  crypto_aead_xchacha20poly1305_ietf_encrypt(pkt + kRtpHeader, &clen, payload, payload_len, pkt, kRtpHeader,
                                             nullptr, nonce, _secret_key);
  memcpy(pkt + kRtpHeader + clen, nonce, 4);
  _nonce++;
  ::send(_udp, pkt, kRtpHeader + clen + 4, 0);
  return true;
}

void VoiceConnection::stopSpeaking() {
  if (!_speaking) return;
  for (int i = 0; i < 5; i++) sendOpus(kSilence, sizeof(kSilence));
  std::scoped_lock lock(_send_mutex);
  setSpeaking(false);
}

}  // namespace nxcord
