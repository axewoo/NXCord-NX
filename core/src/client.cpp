#include <nxcord/client.hpp>
#include <nxcord/log.hpp>

#include <algorithm>

namespace nxcord {

Client::Client() {
  _gateway.onOwnVoiceState = [this](const Snowflake &guild_id, const Snowflake &channel_id,
                                    const std::string &session_id) {
    std::scoped_lock lock(_mutex);
    if (guild_id != _target_guild) return;
    if (channel_id.empty()) {
      // Kicked, moved out, or our own leave.
      if (_voice_state != VoiceState::Idle) log::info("client: left voice channel");
      dropVoice();
      _voice_state = VoiceState::Idle;
      _target_channel.clear();
      return;
    }
    if (channel_id != _target_channel) {
      // Moved by a moderator: follow along.
      _target_channel = channel_id;
      dropVoice();
      _have_server = false;
      _voice_state = VoiceState::Joining;
    }
    _session_id = session_id;
    _have_session = true;
    maybeStartVoice();
  };
  _gateway.onVoiceServer = [this](const Gateway::VoiceServer &server) {
    std::scoped_lock lock(_mutex);
    if (server.guild_id != _target_guild) return;
    dropVoice();
    if (server.endpoint.empty()) {
      _have_server = false;  // voice server going away; a new update follows
      return;
    }
    _server = server;
    _have_server = true;
    maybeStartVoice();
  };
}

Client::~Client() { stop(); }

void Client::start(const std::string &token) {
  stop();
  _gateway.setToken(token);
  _running = true;
  _thread = std::thread([this] { run(); });
}

void Client::stop() {
  _running = false;
  if (_thread.joinable()) _thread.join();
  std::scoped_lock lock(_mutex);
  dropVoice();
  _voice_state = VoiceState::Idle;
  _gateway.disconnect();
}

Client::Status Client::status() const {
  Status s;
  s.gateway = _gateway.status();
  s.gateway_error = _gateway.lastError();
  s.user_id = _gateway.userId();
  s.mic_level = _encoder.lastLevel();
  std::scoped_lock lock(_mutex);
  s.voice = _voice_state;
  s.voice_error = _voice_error;
  s.voice_guild = _target_guild;
  s.voice_channel = _target_channel;
  s.e2ee = _voice && _voice->e2ee();
  s.self_mute = _self_mute;
  s.self_deaf = _self_deaf;
  return s;
}

void Client::joinVoice(const Snowflake &guild_id, const Snowflake &channel_id) {
  std::scoped_lock lock(_mutex);
  if (!_target_guild.empty() && _target_guild != guild_id) {
    // Bots can be in one voice channel per guild; we only want one overall.
    _pending_leave = true;
  }
  dropVoice();
  _leave_guild = _target_guild != guild_id ? _target_guild : Snowflake();
  _target_guild = guild_id;
  _target_channel = channel_id;
  _have_session = _have_server = false;
  _voice_error.clear();
  _voice_state = VoiceState::Joining;
  _pending_join = true;
}

void Client::leaveVoice() {
  std::scoped_lock lock(_mutex);
  dropVoice();
  if (!_target_guild.empty()) {
    _leave_guild = _target_guild;
    _pending_leave = true;
  }
  _target_guild.clear();
  _target_channel.clear();
  _voice_state = VoiceState::Idle;
}

void Client::setSelfMute(bool mute) {
  std::scoped_lock lock(_mutex);
  _self_mute = mute;
  _pending_state = true;
}

void Client::setSelfDeaf(bool deaf) {
  std::scoped_lock lock(_mutex);
  _self_deaf = deaf;
  _pending_state = true;
}

void Client::dropVoice() {
  if (_voice) {
    auto voice = std::move(_voice);
    _voice.reset();
    voice->stop();
  }
  _mixer.clear();
  _speaking = false;
}

void Client::maybeStartVoice() {
  if (!_have_session || !_have_server || _voice) return;
  VoiceConnection::Params p;
  p.guild_id = _target_guild;
  p.channel_id = _target_channel;
  p.user_id = _gateway.userId();
  p.session_id = _session_id;
  p.token = _server.token;
  p.endpoint = _server.endpoint;
  _voice = std::make_shared<VoiceConnection>(
      p, [this](uint32_t ssrc, const Snowflake &user, uint16_t seq, const uint8_t *opus, size_t len) {
        _mixer.push(ssrc, user, seq, opus, len);
      });
  _voice->start();
}

void Client::run() {
  while (_running) {
    _gateway.tick(50);

    std::scoped_lock lock(_mutex);
    if (_gateway.status() != Gateway::Status::Ready) continue;

    if (_pending_leave) {
      if (_gateway.updateVoiceState(_leave_guild, "", false, false)) _pending_leave = false;
    }
    if (_pending_join) {
      if (_gateway.updateVoiceState(_target_guild, _target_channel, _self_mute, _self_deaf)) {
        _pending_join = false;
        _pending_state = false;
      }
    } else if (_pending_state && !_target_channel.empty()) {
      if (_gateway.updateVoiceState(_target_guild, _target_channel, _self_mute, _self_deaf)) _pending_state = false;
    }

    if (_voice) {
      switch (_voice->state()) {
        case VoiceConnection::State::Connected:
          _voice_state = VoiceState::Connected;
          break;
        case VoiceConnection::State::Failed:
          _voice_error = _voice->error();
          _voice_state = VoiceState::Failed;
          log::info("client: voice failed: %s", _voice_error.c_str());
          dropVoice();
          break;
        default:
          break;
      }
    }
  }
}

void Client::feedMicrophone(const int16_t *pcm) {
  std::shared_ptr<VoiceConnection> voice;
  bool muted;
  {
    std::scoped_lock lock(_mutex);
    voice = _voice;
    muted = _self_mute || _self_deaf;
  }
  if (!voice || voice->state() != VoiceConnection::State::Connected) return;
  uint8_t opus[1275];
  int n = muted ? 0 : _encoder.encode(pcm, opus, sizeof(opus));
  if (n > 0) {
    voice->sendOpus(opus, static_cast<size_t>(n));
    _speaking = true;
  } else if (_speaking) {
    voice->stopSpeaking();
    _speaking = false;
  }
}

bool Client::mixSpeaker(int16_t *out) {
  bool deaf;
  {
    std::scoped_lock lock(_mutex);
    deaf = _self_deaf;
  }
  bool any = _mixer.mix(out);
  if (deaf) {
    std::fill(out, out + kFrameValues, 0);
    return false;
  }
  return any;
}

}  // namespace nxcord
