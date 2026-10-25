#include <nxcord/gateway.hpp>
#include <nxcord/log.hpp>
#include <nxcord/util.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>

using json = nlohmann::json;

namespace nxcord {

namespace {

constexpr const char *kGatewayUrl = "wss://gateway.discord.gg/?v=10&encoding=json";
constexpr int kIntents = (1 << 0) | (1 << 7);  // GUILDS | GUILD_VOICE_STATES

std::string str(const json &j, const char *key) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return {};
  if (it->is_string()) return it->get<std::string>();
  return it->dump();
}

std::string displayName(const json &member) {
  std::string nick = str(member, "nick");
  if (!nick.empty()) return nick;
  if (auto u = member.find("user"); u != member.end()) {
    std::string global = str(*u, "global_name");
    return global.empty() ? str(*u, "username") : global;
  }
  return {};
}

Channel parseChannel(const json &c) {
  Channel ch;
  ch.id = str(c, "id");
  ch.name = str(c, "name");
  ch.type = c.value("type", 0);
  ch.position = c.value("position", 0);
  ch.parent_id = str(c, "parent_id");
  return ch;
}

void sortChannels(std::vector<Channel> &channels) {
  std::stable_sort(channels.begin(), channels.end(),
                   [](const Channel &a, const Channel &b) { return a.position < b.position; });
}

VoiceMember parseVoiceState(const json &v) {
  VoiceMember m;
  m.user_id = str(v, "user_id");
  m.channel_id = str(v, "channel_id");
  m.self_mute = v.value("self_mute", false);
  m.self_deaf = v.value("self_deaf", false);
  m.mute = v.value("mute", false);
  m.deaf = v.value("deaf", false);
  return m;
}

}  // namespace

std::string Gateway::lastError() const {
  std::scoped_lock lock(_mutex);
  return _error;
}

void Gateway::setError(const std::string &err) {
  log::error("gateway: %s", err.c_str());
  std::scoped_lock lock(_mutex);
  _error = err;
}

std::vector<Guild> Gateway::guilds() const {
  std::scoped_lock lock(_mutex);
  std::vector<Guild> out;
  for (auto &[id, g] : _guilds) out.push_back(g);
  return out;
}

bool Gateway::guild(const Snowflake &id, Guild &out) const {
  std::scoped_lock lock(_mutex);
  auto it = _guilds.find(id);
  if (it == _guilds.end()) return false;
  out = it->second;
  return true;
}

void Gateway::resetSession() {
  _seq = -1;
  _session_id.clear();
  _resume_url.clear();
}

void Gateway::disconnect() {
  _ws.close(1000);
  resetSession();
  _status = Status::Disconnected;
  {
    std::scoped_lock lock(_mutex);
    _guilds.clear();
  }
  if (onStateChanged) onStateChanged();
}

void Gateway::tick(int timeout_ms) {
  if (_token.empty() || _status == Status::AuthFailed) {
    sleepMs(timeout_ms);
    return;
  }

  if (!_ws.connected()) {
    if (nowMs() < _reconnect_at) {
      sleepMs(std::min<int64_t>(timeout_ms, _reconnect_at - nowMs()));
      return;
    }
    _status = Status::Connecting;
    std::string url = _resume_url.empty() ? kGatewayUrl : _resume_url + "/?v=10&encoding=json";
    log::info("gateway: connecting to %s", url.c_str());
    if (!_ws.connect(url)) {
      setError("connect failed: " + _ws.error());
      _reconnect_at = nowMs() + _reconnect_delay;
      _reconnect_delay = std::min(_reconnect_delay * 2, 60000);
      return;
    }
    _heartbeat_interval = 0;
    _heartbeat_acked = true;
  }

  int wait = timeout_ms;
  if (_heartbeat_interval > 0) {
    wait = static_cast<int>(std::clamp<int64_t>(_next_heartbeat - nowMs(), 0, timeout_ms));
  }
  bool alive = _ws.poll(wait, [this](WebSocket::Type type, std::string &&msg) {
    if (type == WebSocket::Type::Text) handleMessage(msg);
  });

  if (alive && _heartbeat_interval > 0 && nowMs() >= _next_heartbeat) {
    if (!_heartbeat_acked) {
      log::info("gateway: heartbeat not acknowledged, reconnecting");
      _ws.close(4000);
      alive = false;
    } else {
      sendHeartbeat();
    }
  }

  if (!alive) {
    uint16_t code = _ws.closeCode();
    _ws.close(4000);
    _heartbeat_interval = 0;
    if (_status == Status::AuthFailed) return;
    _status = Status::Connecting;
    switch (code) {
      case 4004:
        setError("invalid bot token (4004)");
        _status = Status::AuthFailed;
        return;
      case 4013:
      case 4014:
        setError("intents not allowed for this bot (" + std::to_string(code) + ")");
        _status = Status::AuthFailed;
        return;
      case 4007:
      case 4009:
      case 4010:
      case 4011:
      case 4012:
        resetSession();
        break;
      default:
        break;
    }
    _reconnect_at = nowMs() + _reconnect_delay;
    _reconnect_delay = std::min(_reconnect_delay * 2, 60000);
    if (onStateChanged) onStateChanged();
  }
}

void Gateway::sendHeartbeat() {
  json hb = {{"op", 1}, {"d", _seq >= 0 ? json(_seq) : json(nullptr)}};
  _ws.sendText(hb.dump());
  _heartbeat_acked = false;
  _next_heartbeat = nowMs() + _heartbeat_interval;
}

void Gateway::sendIdentify() {
  json id = {{"op", 2},
             {"d",
              {{"token", _token},
               {"intents", kIntents},
               {"properties", {{"os", "horizon"}, {"browser", "nxcord"}, {"device", "nxcord"}}}}}};
  _ws.sendText(id.dump());
}

void Gateway::sendResume() {
  json r = {{"op", 6}, {"d", {{"token", _token}, {"session_id", _session_id}, {"seq", _seq}}}};
  _ws.sendText(r.dump());
}

bool Gateway::updateVoiceState(const Snowflake &guild_id, const Snowflake &channel_id, bool self_mute,
                               bool self_deaf) {
  if (_status != Status::Ready) return false;
  json vs = {{"op", 4},
             {"d",
              {{"guild_id", guild_id},
               {"channel_id", channel_id.empty() ? json(nullptr) : json(channel_id)},
               {"self_mute", self_mute},
               {"self_deaf", self_deaf}}}};
  return _ws.sendText(vs.dump());
}

void Gateway::handleMessage(const std::string &msg) {
  json j = json::parse(msg, nullptr, false);
  if (j.is_discarded()) {
    log::error("gateway: invalid JSON");
    return;
  }
  int op = j.value("op", -1);
  if (j.contains("s") && j["s"].is_number()) _seq = j["s"].get<int64_t>();

  switch (op) {
    case 0:
      handleDispatch(j.value("t", ""), j["d"]);
      break;
    case 1:
      sendHeartbeat();
      break;
    case 7:
      log::info("gateway: server asked us to reconnect");
      _ws.close(4000);
      break;
    case 9: {
      bool resumable = j["d"].is_boolean() && j["d"].get<bool>();
      log::info("gateway: invalid session (resumable=%d)", resumable);
      if (!resumable) resetSession();
      _ws.close(resumable ? 4000 : 1000);
      _reconnect_at = nowMs() + 2000;
      break;
    }
    case 10: {
      _heartbeat_interval = j["d"].value("heartbeat_interval", 41250);
      // First beat is jittered, as the docs ask.
      uint32_t r;
      randomBytes(&r, sizeof(r));
      _next_heartbeat = nowMs() + static_cast<int64_t>(_heartbeat_interval * ((r % 1000) / 1000.0));
      _heartbeat_acked = true;
      if (!_session_id.empty()) {
        sendResume();
      } else {
        sendIdentify();
      }
      break;
    }
    case 11:
      _heartbeat_acked = true;
      break;
    default:
      break;
  }
}

void Gateway::handleDispatch(const std::string &type, const json &d) {
  bool changed = false;

  if (type == "READY") {
    _session_id = str(d, "session_id");
    _resume_url = str(d, "resume_gateway_url");
    _user_id = str(d["user"], "id");
    _status = Status::Ready;
    _reconnect_delay = 1000;
    {
      std::scoped_lock lock(_mutex);
      _error.clear();
      _guilds.clear();
      // Guilds arrive unavailable here and are filled in by GUILD_CREATE.
      for (auto &g : d["guilds"]) _guilds[str(g, "id")].id = str(g, "id");
    }
    log::info("gateway: ready as %s (%s)", str(d["user"], "username").c_str(), _user_id.c_str());
    changed = true;
  } else if (type == "RESUMED") {
    _status = Status::Ready;
    _reconnect_delay = 1000;
    log::info("gateway: resumed");
  } else if (type == "GUILD_CREATE" || type == "GUILD_UPDATE") {
    std::scoped_lock lock(_mutex);
    Guild &g = _guilds[str(d, "id")];
    g.id = str(d, "id");
    g.name = str(d, "name");
    if (d.contains("channels")) {
      g.channels.clear();
      for (auto &c : d["channels"]) g.channels.push_back(parseChannel(c));
      sortChannels(g.channels);
    }
    if (d.contains("members")) {
      for (auto &m : d["members"]) g.names[str(m["user"], "id")] = displayName(m);
    }
    if (d.contains("voice_states")) {
      g.voice.clear();
      for (auto &v : d["voice_states"]) {
        VoiceMember vm = parseVoiceState(v);
        g.voice[vm.user_id] = vm;
      }
    }
    changed = true;
  } else if (type == "GUILD_DELETE") {
    std::scoped_lock lock(_mutex);
    _guilds.erase(str(d, "id"));
    changed = true;
  } else if (type == "CHANNEL_CREATE" || type == "CHANNEL_UPDATE" || type == "CHANNEL_DELETE") {
    std::scoped_lock lock(_mutex);
    auto it = _guilds.find(str(d, "guild_id"));
    if (it != _guilds.end()) {
      auto &chs = it->second.channels;
      Channel ch = parseChannel(d);
      chs.erase(std::remove_if(chs.begin(), chs.end(), [&](const Channel &c) { return c.id == ch.id; }),
                chs.end());
      if (type != "CHANNEL_DELETE") chs.push_back(ch);
      sortChannels(chs);
      changed = true;
    }
  } else if (type == "VOICE_STATE_UPDATE") {
    VoiceMember vm = parseVoiceState(d);
    Snowflake guild_id = str(d, "guild_id");
    {
      std::scoped_lock lock(_mutex);
      auto it = _guilds.find(guild_id);
      if (it != _guilds.end()) {
        if (vm.channel_id.empty()) {
          it->second.voice.erase(vm.user_id);
        } else {
          it->second.voice[vm.user_id] = vm;
        }
        if (d.contains("member")) {
          std::string name = displayName(d["member"]);
          if (!name.empty()) it->second.names[vm.user_id] = name;
        }
      }
    }
    if (vm.user_id == _user_id && onOwnVoiceState) {
      onOwnVoiceState(guild_id, vm.channel_id, str(d, "session_id"));
    }
    changed = true;
  } else if (type == "VOICE_SERVER_UPDATE") {
    if (onVoiceServer) onVoiceServer({str(d, "guild_id"), str(d, "token"), str(d, "endpoint")});
  }

  if (changed && onStateChanged) onStateChanged();
}

}  // namespace nxcord
