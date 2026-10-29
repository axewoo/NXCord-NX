#pragma once

#include <nxcord/model.hpp>
#include <nxcord/websocket.hpp>

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace nxcord {

// Discord main gateway (v10, JSON, no compression).
// Keeps a small cache of guilds, voice channels and who is in them.
class Gateway {
 public:
  enum class Status { Disconnected, Connecting, Ready, AuthFailed };

  struct VoiceServer {
    Snowflake guild_id;
    std::string token;
    std::string endpoint;
  };

  std::function<void(const VoiceServer &)> onVoiceServer;
  // Fired for our own voice state changes (session id + channel).
  std::function<void(const Snowflake &guild_id, const Snowflake &channel_id, const std::string &session_id)>
      onOwnVoiceState;
  // Fired whenever the cache changes, so UIs can refresh.
  std::function<void()> onStateChanged;

  void setToken(const std::string &token) { _token = token; }

  // Runs one iteration: (re)connects if needed, reads messages, heartbeats.
  void tick(int timeout_ms);
  void disconnect();

  // op 4: join (channel_id non-empty) or leave (empty) a voice channel.
  bool updateVoiceState(const Snowflake &guild_id, const Snowflake &channel_id, bool self_mute,
                        bool self_deaf);

  Status status() const { return _status; }
  const Snowflake &userId() const { return _user_id; }
  std::string lastError() const;

  // Copies the cache under the lock.
  std::vector<Guild> guilds() const;
  bool guild(const Snowflake &id, Guild &out) const;

 private:
  void handleMessage(const std::string &msg);
  void handleDispatch(const std::string &type, const nlohmann::json &d);
  void sendIdentify();
  void sendResume();
  void sendHeartbeat();
  void resetSession();
  void setError(const std::string &err);

  WebSocket _ws;
  std::string _token;
  Status _status = Status::Disconnected;

  int64_t _heartbeat_interval = 0;
  int64_t _next_heartbeat = 0;
  bool _heartbeat_acked = true;
  int64_t _seq = -1;
  std::string _session_id;
  std::string _resume_url;
  int64_t _reconnect_at = 0;
  int _reconnect_delay = 1000;

  Snowflake _user_id;
  mutable std::mutex _mutex;
  std::map<Snowflake, Guild> _guilds;
  std::string _error;
};

}  // namespace nxcord
