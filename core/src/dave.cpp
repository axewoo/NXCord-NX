#include <nxcord/dave.hpp>
#include <nxcord/log.hpp>

#include <dave/dave.h>

#include <cstdlib>

namespace nxcord {

namespace {

constexpr uint16_t kInitTransitionId = 0;
constexpr uint16_t kDisabledVersion = 0;
constexpr uint64_t kNewGroupEpoch = 1;

enum Op : uint8_t {
  kOpTransitionReady = 23,
  kOpKeyPackage = 26,
  kOpCommitWelcome = 28,
  kOpInvalidCommitWelcome = 31,
};

void onMlsFailure(const char *source, const char *reason, void *) {
  log::error("DAVE MLS failure: %s: %s", source, reason);
}

void daveLogSink(DAVELoggingSeverity severity, const char *file, int line, const char *message) {
  if (severity >= DAVE_LOGGING_SEVERITY_WARNING) {
    log::info("libdave %s:%d %s", file, line, message);
  } else {
    log::debug("libdave %s:%d %s", file, line, message);
  }
}

}  // namespace

uint16_t DaveSession::maxProtocolVersion() { return daveMaxSupportedProtocolVersion(); }

DaveSession::DaveSession(Snowflake self_user_id, Snowflake channel_id, SendJson send_json,
                         SendBinary send_binary)
    : _self(std::move(self_user_id)),
      _channel(std::move(channel_id)),
      _send_json(std::move(send_json)),
      _send_binary(std::move(send_binary)) {
  static bool sink_set = false;
  if (!sink_set) {
    daveSetLogSinkCallback(daveLogSink);
    sink_set = true;
  }
  _session = daveSessionCreate(nullptr, nullptr, onMlsFailure, this);
  _encryptor = daveEncryptorCreate();
  daveEncryptorSetPassthroughMode(_encryptor, true);
}

DaveSession::~DaveSession() {
  for (auto &[id, dec] : _decryptors) daveDecryptorDestroy(dec);
  if (_encryptor) daveEncryptorDestroy(_encryptor);
  if (_session) daveSessionDestroy(_session);
}

uint16_t DaveSession::protocolVersion() const {
  std::scoped_lock lock(_mutex);
  return _version;
}

void DaveSession::setOwnSsrc(uint32_t ssrc) {
  std::scoped_lock lock(_mutex);
  daveEncryptorAssignSsrcToCodec(_encryptor, ssrc, DAVE_CODEC_OPUS);
}

std::vector<std::string> DaveSession::recognizedUsers() const {
  std::vector<std::string> ids(_users.begin(), _users.end());
  ids.push_back(_self);
  return ids;
}

void DaveSession::addUser(const Snowflake &user_id) {
  std::scoped_lock lock(_mutex);
  if (user_id == _self) return;
  _users.insert(user_id);
  setupRatchetForUser(user_id, _latest_prepared_version);
}

void DaveSession::removeUser(const Snowflake &user_id) {
  std::scoped_lock lock(_mutex);
  _users.erase(user_id);
  auto it = _decryptors.find(user_id);
  if (it != _decryptors.end()) {
    daveDecryptorDestroy(it->second);
    _decryptors.erase(it);
  }
}

void DaveSession::onSessionDescription(uint16_t version) {
  std::scoped_lock lock(_mutex);
  log::info("DAVE: session description, protocol version %u", version);
  handleProtocolInit(version);
}

void DaveSession::onPrepareTransition(uint16_t transition_id, uint16_t version) {
  std::scoped_lock lock(_mutex);
  log::debug("DAVE: prepare transition %u -> v%u", transition_id, version);
  prepareRatchets(transition_id, version);
  if (transition_id != kInitTransitionId) sendReadyForTransition(transition_id);
}

void DaveSession::onExecuteTransition(uint16_t transition_id) {
  std::scoped_lock lock(_mutex);
  log::debug("DAVE: execute transition %u", transition_id);
  executeTransition(transition_id);
}

void DaveSession::onPrepareEpoch(uint64_t epoch, uint16_t version) {
  std::scoped_lock lock(_mutex);
  log::debug("DAVE: prepare epoch %llu v%u", static_cast<unsigned long long>(epoch), version);
  if (epoch == kNewGroupEpoch) {
    initGroup(version);
    sendKeyPackage();
  }
}

void DaveSession::onExternalSender(const uint8_t *data, size_t len) {
  std::scoped_lock lock(_mutex);
  log::debug("DAVE: external sender (%zu bytes)", len);
  daveSessionSetExternalSender(_session, data, len);
}

void DaveSession::onProposals(const uint8_t *data, size_t len) {
  std::scoped_lock lock(_mutex);
  auto ids = recognizedUsers();
  std::vector<const char *> cids;
  for (auto &id : ids) cids.push_back(id.c_str());
  uint8_t *out = nullptr;
  size_t out_len = 0;
  daveSessionProcessProposals(_session, data, len, cids.data(), cids.size(), &out, &out_len);
  if (out && out_len > 0) {
    log::debug("DAVE: sending commit/welcome (%zu bytes)", out_len);
    _send_binary(kOpCommitWelcome, out, out_len);
  }
  if (out) daveFree(out);
}

void DaveSession::onAnnounceCommit(uint16_t transition_id, const uint8_t *data, size_t len) {
  std::scoped_lock lock(_mutex);
  DAVECommitResultHandle result = daveSessionProcessCommit(_session, data, len);
  bool failed = !result || daveCommitResultIsFailed(result);
  bool ignored = result && daveCommitResultIsIgnored(result);
  if (result) daveCommitResultDestroy(result);
  if (ignored) {
    log::debug("DAVE: commit %u ignored", transition_id);
    return;
  }
  if (failed) {
    log::info("DAVE: commit %u failed, asking to be re-added", transition_id);
    sendInvalidCommitWelcome(transition_id);
    handleProtocolInit(daveSessionGetProtocolVersion(_session));
    return;
  }
  prepareRatchets(transition_id, daveSessionGetProtocolVersion(_session));
  if (transition_id != kInitTransitionId) sendReadyForTransition(transition_id);
}

void DaveSession::onWelcome(uint16_t transition_id, const uint8_t *data, size_t len) {
  std::scoped_lock lock(_mutex);
  auto ids = recognizedUsers();
  std::vector<const char *> cids;
  for (auto &id : ids) cids.push_back(id.c_str());
  DAVEWelcomeResultHandle result = daveSessionProcessWelcome(_session, data, len, cids.data(), cids.size());
  if (!result) {
    log::info("DAVE: welcome %u invalid, asking to be re-added", transition_id);
    sendInvalidCommitWelcome(transition_id);
    sendKeyPackage();
    return;
  }
  daveWelcomeResultDestroy(result);
  log::info("DAVE: joined MLS group (transition %u)", transition_id);
  prepareRatchets(transition_id, daveSessionGetProtocolVersion(_session));
  if (transition_id != kInitTransitionId) sendReadyForTransition(transition_id);
}

void DaveSession::handleProtocolInit(uint16_t version) {
  if (version > kDisabledVersion) {
    initGroup(version);
    sendKeyPackage();
  } else {
    prepareRatchets(kInitTransitionId, version);
    executeTransition(kInitTransitionId);
  }
}

void DaveSession::initGroup(uint16_t version) {
  daveSessionInit(_session, version, std::strtoull(_channel.c_str(), nullptr, 10), _self.c_str());
}

void DaveSession::sendKeyPackage() {
  uint8_t *kp = nullptr;
  size_t kp_len = 0;
  daveSessionGetMarshalledKeyPackage(_session, &kp, &kp_len);
  if (kp && kp_len > 0) {
    log::debug("DAVE: sending key package (%zu bytes)", kp_len);
    _send_binary(kOpKeyPackage, kp, kp_len);
  } else {
    log::error("DAVE: could not create a key package");
  }
  if (kp) daveFree(kp);
}

void DaveSession::sendReadyForTransition(uint16_t transition_id) {
  _send_json(kOpTransitionReady, "{\"transition_id\":" + std::to_string(transition_id) + "}");
}

void DaveSession::sendInvalidCommitWelcome(uint16_t transition_id) {
  _send_json(kOpInvalidCommitWelcome, "{\"transition_id\":" + std::to_string(transition_id) + "}");
}

void DaveSession::prepareRatchets(uint16_t transition_id, uint16_t version) {
  for (auto &user : _users) setupRatchetForUser(user, version);
  if (transition_id == kInitTransitionId) {
    setupRatchetForUser(_self, version);
    _version = version;
  } else {
    _pending_transitions[transition_id] = version;
  }
  _latest_prepared_version = version;
}

void DaveSession::executeTransition(uint16_t transition_id) {
  auto it = _pending_transitions.find(transition_id);
  if (it == _pending_transitions.end()) {
    if (transition_id != kInitTransitionId) log::debug("DAVE: unknown transition %u", transition_id);
    return;
  }
  uint16_t version = it->second;
  _pending_transitions.erase(it);
  if (version == kDisabledVersion) daveSessionReset(_session);
  setupRatchetForUser(_self, version);
  _version = version;
  log::info("DAVE: now using protocol version %u%s", version, version ? " (end-to-end encrypted)" : "");
}

void DaveSession::setupRatchetForUser(const Snowflake &user_id, uint16_t version) {
  DAVEKeyRatchetHandle ratchet = nullptr;
  if (version != kDisabledVersion) ratchet = daveSessionGetKeyRatchet(_session, user_id.c_str());

  if (user_id == _self) {
    if (ratchet) {
      daveEncryptorSetKeyRatchet(_encryptor, ratchet);
      daveEncryptorSetPassthroughMode(_encryptor, false);
    } else {
      daveEncryptorSetPassthroughMode(_encryptor, true);
    }
  } else {
    auto &dec = _decryptors[user_id];
    if (!dec) dec = daveDecryptorCreate();
    if (ratchet) {
      daveDecryptorTransitionToKeyRatchet(dec, ratchet);
    } else {
      daveDecryptorTransitionToPassthroughMode(dec, true);
    }
  }
  if (ratchet) daveKeyRatchetDestroy(ratchet);
}

bool DaveSession::encrypt(uint32_t ssrc, const uint8_t *frame, size_t len, std::vector<uint8_t> &out) {
  std::scoped_lock lock(_mutex);
  out.resize(daveEncryptorGetMaxCiphertextByteSize(_encryptor, DAVE_MEDIA_TYPE_AUDIO, len));
  size_t written = 0;
  auto rc = daveEncryptorEncrypt(_encryptor, DAVE_MEDIA_TYPE_AUDIO, ssrc, frame, len, out.data(),
                                 out.size(), &written);
  if (rc != DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS) {
    log::debug("DAVE: encrypt failed (%d)", rc);
    return false;
  }
  out.resize(written);
  return true;
}

bool DaveSession::decrypt(const Snowflake &user_id, const uint8_t *frame, size_t len,
                          std::vector<uint8_t> &out) {
  std::scoped_lock lock(_mutex);
  auto it = _decryptors.find(user_id);
  if (it == _decryptors.end()) {
    // Unknown sender: only plain frames can be played.
    if (_version != 0) return false;
    out.assign(frame, frame + len);
    return true;
  }
  out.resize(daveDecryptorGetMaxPlaintextByteSize(it->second, DAVE_MEDIA_TYPE_AUDIO, len));
  size_t written = 0;
  auto rc = daveDecryptorDecrypt(it->second, DAVE_MEDIA_TYPE_AUDIO, frame, len, out.data(), out.size(),
                                 &written);
  if (rc != DAVE_DECRYPTOR_RESULT_CODE_SUCCESS) {
    log::debug("DAVE: decrypt from %s failed (%d)", user_id.c_str(), rc);
    return false;
  }
  out.resize(written);
  return true;
}

}  // namespace nxcord
