// nxcord-cli: drives the same core as the Switch sysmodule from a terminal.
//
//   export NXCORD_TOKEN=...            (or --token-file FILE)
//   nxcord-cli --list
//   pw-record --format s16 --rate 48000 --channels 2 - \
//     | nxcord-cli --join GUILD_ID/CHANNEL_ID \
//     | pw-play --format s16 --rate 48000 --channels 2 -
//
// Microphone PCM is read from stdin (s16le, 48 kHz, stereo) and the mixed
// call audio is written to stdout in the same format. --tone sends a test
// tone instead of reading stdin.
#include <nxcord/client.hpp>
#include <nxcord/log.hpp>
#include <nxcord/util.hpp>

#include <unistd.h>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using namespace nxcord;

namespace {

std::atomic<bool> g_quit{false};

void usage() {
  std::fprintf(stderr,
               "usage: nxcord-cli [--token-file FILE] [--verbose] (--list | --join GUILD/CHANNEL [--tone] "
               "[--seconds N])\n");
}

bool readFrame(int16_t *pcm) {
  size_t want = kFrameValues * sizeof(int16_t);
  auto *p = reinterpret_cast<uint8_t *>(pcm);
  size_t got = 0;
  while (got < want) {
    ssize_t n = ::read(STDIN_FILENO, p + got, want - got);
    if (n <= 0) return false;
    got += static_cast<size_t>(n);
  }
  return true;
}

const char *voiceStateName(Client::VoiceState s) {
  switch (s) {
    case Client::VoiceState::Idle: return "idle";
    case Client::VoiceState::Joining: return "joining";
    case Client::VoiceState::Connected: return "connected";
    case Client::VoiceState::Failed: return "failed";
  }
  return "?";
}

}  // namespace

int main(int argc, char **argv) {
  std::string token_file, join;
  bool list = false, tone = false;
  int seconds = 0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--token-file" && i + 1 < argc) token_file = argv[++i];
    else if (a == "--list") list = true;
    else if (a == "--join" && i + 1 < argc) join = argv[++i];
    else if (a == "--tone") tone = true;
    else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
    else if (a == "--verbose") log::setVerbose(true);
    else {
      usage();
      return 2;
    }
  }
  if (!list && join.empty()) {
    usage();
    return 2;
  }

  std::string token;
  if (!token_file.empty()) {
    std::ifstream f(token_file);
    std::getline(f, token);
  } else if (const char *env = std::getenv("NXCORD_TOKEN")) {
    token = env;
  }
  while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.pop_back();
  if (token.empty()) {
    std::fprintf(stderr, "no bot token: set NXCORD_TOKEN or pass --token-file\n");
    return 2;
  }

  std::signal(SIGINT, [](int) { g_quit = true; });
  std::signal(SIGPIPE, SIG_IGN);

  Client client;
  client.start(token);

  int64_t deadline = nowMs() + 20000;
  while (client.status().gateway != Gateway::Status::Ready) {
    if (client.status().gateway == Gateway::Status::AuthFailed || nowMs() > deadline || g_quit) {
      std::fprintf(stderr, "could not log in: %s\n", client.status().gateway_error.c_str());
      return 1;
    }
    sleepMs(100);
  }
  sleepMs(1500);  // let GUILD_CREATE events arrive

  if (list) {
    for (auto &g : client.guilds()) {
      std::printf("%s  %s\n", g.id.c_str(), g.name.c_str());
      for (auto &c : g.channels) {
        if (c.type != static_cast<int>(ChannelType::Voice) && c.type != static_cast<int>(ChannelType::Stage))
          continue;
        std::printf("    %s/%s  🔊 %s\n", g.id.c_str(), c.id.c_str(), c.name.c_str());
        for (auto &[uid, vm] : g.voice) {
          if (vm.channel_id != c.id) continue;
          auto n = g.names.find(uid);
          std::printf("        - %s\n", n != g.names.end() ? n->second.c_str() : uid.c_str());
        }
      }
    }
    client.stop();
    return 0;
  }

  size_t slash = join.find('/');
  if (slash == std::string::npos) {
    usage();
    return 2;
  }
  client.joinVoice(join.substr(0, slash), join.substr(slash + 1));

  // Speaker: write one mixed frame every 20 ms. Only when stdout is piped,
  // so a terminal doesn't get raw PCM.
  bool to_stdout = !isatty(STDOUT_FILENO);
  std::thread speaker([&] {
    std::vector<int16_t> out(kFrameValues);
    int64_t next = nowMs();
    while (!g_quit) {
      client.mixSpeaker(out.data());
      if (to_stdout && ::write(STDOUT_FILENO, out.data(), out.size() * sizeof(int16_t)) < 0) break;
      next += 20;
      int64_t wait = next - nowMs();
      if (wait > 0) sleepMs(static_cast<int>(wait));
    }
  });

  // Microphone: stdin paces itself; the tone generator paces on the clock.
  std::thread mic([&] {
    std::vector<int16_t> pcm(kFrameValues);
    double phase = 0;
    int64_t next = nowMs();
    if (tone) client.encoder().setGateThreshold(0);
    while (!g_quit) {
      if (tone) {
        for (int i = 0; i < kFrameSamples; i++) {
          // 440 Hz beeping on and off every half second.
          bool on = (nowMs() / 500) % 2 == 0;
          int16_t v = on ? static_cast<int16_t>(6000 * std::sin(phase)) : 0;
          pcm[i * 2] = pcm[i * 2 + 1] = v;
          phase += 2 * M_PI * 440 / kSampleRate;
        }
        next += 20;
        int64_t wait = next - nowMs();
        if (wait > 0) sleepMs(static_cast<int>(wait));
      } else if (!readFrame(pcm.data())) {
        sleepMs(20);
        continue;
      }
      client.feedMicrophone(pcm.data());
    }
  });

  int64_t end = seconds > 0 ? nowMs() + seconds * 1000LL : 0;
  Client::VoiceState last = Client::VoiceState::Idle;
  bool last_e2ee = false;
  while (!g_quit && (end == 0 || nowMs() < end)) {
    auto st = client.status();
    if (st.voice != last || st.e2ee != last_e2ee) {
      std::fprintf(stderr, "voice: %s%s%s%s\n", voiceStateName(st.voice), st.e2ee ? " (end-to-end encrypted)" : "",
                   st.voice_error.empty() ? "" : " - ", st.voice_error.c_str());
      last = st.voice;
      last_e2ee = st.e2ee;
    }
    if (st.voice == Client::VoiceState::Failed) break;
    sleepMs(200);
  }
  g_quit = true;
  client.leaveVoice();
  sleepMs(300);
  speaker.join();
  // The mic thread may be blocked in read(); don't wait for it.
  mic.detach();
  client.stop();
  return 0;
}
