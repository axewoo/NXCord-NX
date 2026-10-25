#pragma once

#include <nxcord/model.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct DAVESessionHandle_s;
struct DAVEEncryptorHandle_s;
struct DAVEDecryptorHandle_s;

namespace nxcord {

// Discord's DAVE end-to-end encryption, driven by voice gateway opcodes 21-31.
// Follows libdave's reference DaveSessionManager; all methods are thread-safe.
class DaveSession {
 public:
  // Sends a JSON voice gateway message: {"op": op, "d": d_json}.
  using SendJson = std::function<void(int op, const std::string &d_json)>;
  // Sends a binary voice gateway message: [op][payload].
  using SendBinary = std::function<void(uint8_t op, const uint8_t *data, size_t len)>;

  DaveSession(Snowflake self_user_id, Snowflake channel_id, SendJson send_json,
              SendBinary send_binary);
  ~DaveSession();
  DaveSession(const DaveSession &) = delete;
  DaveSession &operator=(const DaveSession &) = delete;

  static uint16_t maxProtocolVersion();

  void setOwnSsrc(uint32_t ssrc);
  void addUser(const Snowflake &user_id);
  void removeUser(const Snowflake &user_id);

  // Incoming voice gateway events.
  void onSessionDescription(uint16_t protocol_version);                  // op 4
  void onPrepareTransition(uint16_t transition_id, uint16_t version);    // op 21
  void onExecuteTransition(uint16_t transition_id);                      // op 22
  void onPrepareEpoch(uint64_t epoch, uint16_t version);                 // op 24
  void onExternalSender(const uint8_t *data, size_t len);                // op 25
  void onProposals(const uint8_t *data, size_t len);                     // op 27
  void onAnnounceCommit(uint16_t transition_id, const uint8_t *data, size_t len);  // op 29
  void onWelcome(uint16_t transition_id, const uint8_t *data, size_t len);         // op 30

  // Media. Return false if the frame must be dropped.
  bool encrypt(uint32_t ssrc, const uint8_t *frame, size_t len, std::vector<uint8_t> &out);
  bool decrypt(const Snowflake &user_id, const uint8_t *frame, size_t len, std::vector<uint8_t> &out);

  uint16_t protocolVersion() const;
  bool encrypted() const { return protocolVersion() != 0; }

 private:
  void handleProtocolInit(uint16_t version);
  void initGroup(uint16_t version);
  void sendKeyPackage();
  void sendReadyForTransition(uint16_t transition_id);
  void sendInvalidCommitWelcome(uint16_t transition_id);
  void prepareRatchets(uint16_t transition_id, uint16_t version);
  void executeTransition(uint16_t transition_id);
  void setupRatchetForUser(const Snowflake &user_id, uint16_t version);
  std::vector<std::string> recognizedUsers() const;

  mutable std::recursive_mutex _mutex;
  Snowflake _self;
  Snowflake _channel;
  SendJson _send_json;
  SendBinary _send_binary;

  DAVESessionHandle_s *_session = nullptr;
  DAVEEncryptorHandle_s *_encryptor = nullptr;
  std::map<Snowflake, DAVEDecryptorHandle_s *> _decryptors;
  std::set<Snowflake> _users;
  std::map<uint16_t, uint16_t> _pending_transitions;  // transition id -> protocol version
  uint16_t _latest_prepared_version = 0;
  uint16_t _version = 0;
};

}  // namespace nxcord
