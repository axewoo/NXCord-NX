#pragma once

#include <nxcord/audio.hpp>
#include <nxcord/gateway.hpp>
#include <nxcord/voice.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace nxcord {

// Ties the gateway, the voice connection and audio together.
// Platform code feeds microphone frames in and pulls speaker frames out.
class Client {
 public:
  enum class VoiceState { Idle, Joining, Connected, Failed };

  struct Status {
    Gateway::Status gateway = Gateway::Status::Disconnected;
    std::string gateway_error;
    Snowflake user_id;
    VoiceState voice = VoiceState::Idle;
    std::string voice_error;
    Snowflake voice_guild;
    Snowflake voice_channel;
    bool e2ee = false;
    bool self_mute = false;
    bool self_deaf = false;
    int mic_level = 0;
  };

  Client();
  ~Client();

  void start(const std::string &token);
  void stop();

  Status status() const;
  std::vector<Guild> guilds() const { return _gateway.guilds(); }
  bool guild(const Snowflake &id, Guild &out) const { return _gateway.guild(id, out); }

  void joinVoice(const Snowflake &guild_id, const Snowflake &channel_id);
  void leaveVoice();
  void setSelfMute(bool mute);
  void setSelfDeaf(bool deaf);

  // Audio threads. Frames are 20 ms of 48 kHz stereo int16 (kFrameValues).
  void feedMicrophone(const int16_t *pcm);
  bool mixSpeaker(int16_t *out);

  AudioMixer &mixer() { return _mixer; }
  MicEncoder &encoder() { return _encoder; }

 private:
  void run();
  void maybeStartVoice();
  void dropVoice();

  Gateway _gateway;
  AudioMixer _mixer;
  MicEncoder _encoder;
  std::thread _thread;
  std::atomic<bool> _running{false};

  mutable std::mutex _mutex;
  std::shared_ptr<VoiceConnection> _voice;
  VoiceState _voice_state = VoiceState::Idle;
  std::string _voice_error;
  Snowflake _target_guild, _target_channel;
  std::string _session_id;
  Gateway::VoiceServer _server;
  bool _have_session = false, _have_server = false;
  bool _pending_join = false, _pending_leave = false, _pending_state = false;
  bool _self_mute = false, _self_deaf = false;
  Snowflake _leave_guild;
  std::atomic<bool> _speaking{false};
};

}  // namespace nxcord
