#pragma once

#include <nxcord/dave.hpp>
#include <nxcord/model.hpp>
#include <nxcord/websocket.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nxcord {

// One voice channel connection: voice gateway v8 websocket, UDP media with
// aead_xchacha20_poly1305_rtpsize transport encryption, and DAVE E2EE.
class VoiceConnection {
 public:
  enum class State { Connecting, Connected, Failed, Closed };

  struct Params {
    Snowflake guild_id;
    Snowflake channel_id;
    Snowflake user_id;
    std::string session_id;
    std::string token;
    std::string endpoint;
  };

  // Called on the voice thread for every received, decrypted Opus frame.
  using OpusHandler =
      std::function<void(uint32_t ssrc, const Snowflake &user_id, uint16_t seq, const uint8_t *opus, size_t len)>;

  VoiceConnection(Params params, OpusHandler on_opus);
  ~VoiceConnection();
  VoiceConnection(const VoiceConnection &) = delete;
  VoiceConnection &operator=(const VoiceConnection &) = delete;

  void start();
  void stop();

  // Sends one 20 ms Opus frame. Safe to call from any thread.
  bool sendOpus(const uint8_t *opus, size_t len, uint32_t samples = 960);
  // Sends the five silence frames Discord asks for and clears the speaking flag.
  void stopSpeaking();

  State state() const { return _state; }
  std::string error() const;
  bool e2ee() const { return _dave && _dave->encrypted(); }
  const Params &params() const { return _params; }

 private:
  void run();
  void runLoop();
  bool connectWebsocket(bool resume);
  void handleText(const std::string &msg);
  void handleBinary(const std::string &msg);
  void sendJson(int op, const std::string &d_json);
  void sendBinary(uint8_t op, const uint8_t *data, size_t len);
  void sendHeartbeat();
  bool setupUdp(const std::string &ip, uint16_t port, std::string &ext_ip, uint16_t &ext_port);
  void readUdp();
  void handleRtp(const uint8_t *pkt, size_t len);
  void setSpeaking(bool speaking);
  void fail(const std::string &err);

  Params _params;
  OpusHandler _on_opus;
  std::thread _thread;
  std::atomic<bool> _running{false};
  std::atomic<State> _state{State::Connecting};
  mutable std::mutex _error_mutex;
  std::string _error;

  WebSocket _ws;
  int64_t _heartbeat_interval = 0;
  int64_t _next_heartbeat = 0;
  int _seq_ack = -1;
  bool _resumable = false;

  int _udp = -1;
  int64_t _next_keepalive = 0;
  uint32_t _keepalive_counter = 0;
  uint32_t _ssrc = 0;
  uint8_t _secret_key[32] = {};
  std::atomic<bool> _have_key{false};

  std::unique_ptr<DaveSession> _dave;
  std::mutex _users_mutex;
  std::map<uint32_t, Snowflake> _ssrc_users;

  // Sender state, guarded by _send_mutex.
  std::mutex _send_mutex;
  uint16_t _rtp_seq = 0;
  uint32_t _rtp_ts = 0;
  uint32_t _nonce = 0;
  bool _speaking = false;
  std::vector<uint8_t> _e2ee_buf;
};

}  // namespace nxcord
