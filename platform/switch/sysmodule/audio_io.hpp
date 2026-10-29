#pragma once

#include <nxcord/client.hpp>

#include <switch.h>

#include <atomic>

namespace nxcord {

// Speaker (audout) and microphone (audin) threads feeding the Client.
class AudioIo {
 public:
  explicit AudioIo(Client &client) : _client(client) {}
  void start();
  void stop();

  bool micAvailable() const { return _mic_ok; }
  bool speakerAvailable() const { return _speaker_ok; }

 private:
  static void speakerEntry(void *arg) { static_cast<AudioIo *>(arg)->speakerLoop(); }
  static void micEntry(void *arg) { static_cast<AudioIo *>(arg)->micLoop(); }
  void speakerLoop();
  void micLoop();

  Client &_client;
  Thread _speaker_thread{}, _mic_thread{};
  std::atomic<bool> _running{false};
  std::atomic<bool> _mic_ok{false}, _speaker_ok{false};
};

}  // namespace nxcord
