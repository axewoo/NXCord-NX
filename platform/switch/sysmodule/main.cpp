// NXCord-NX sysmodule: keeps the Discord connection and voice call alive in
// the background while games run. The overlay talks to it over IPC.
#include <nxcord/client.hpp>
#include <nxcord/log.hpp>
#include <nxcord/util.hpp>

#include <switch.h>
#include <malloc.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../common/nxcord_ipc.hpp"
#include "audio_io.hpp"
#include "config.hpp"
#include "ipc_server.hpp"

extern "C" {
void __libnx_init_time(void);

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

// Everything (TLS, JSON, Opus, MLS, thread stacks) lives in this heap. It
// comes out of the system memory pool, which Horizon itself needs too, so
// keep it small.
#define INNER_HEAP_SIZE 0x1C0000
size_t nx_inner_heap_size = INNER_HEAP_SIZE;
char nx_inner_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void) {
  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = nx_inner_heap;
  fake_heap_end = nx_inner_heap + nx_inner_heap_size;
}

void __appInit(void) {
  Result rc = smInitialize();
  if (R_FAILED(rc)) diagAbortWithResult(rc);

  if (R_SUCCEEDED(setsysInitialize())) {
    SetSysFirmwareVersion fw;
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
    setsysExit();
  }

  rc = fsInitialize();
  if (R_FAILED(rc)) diagAbortWithResult(rc);
  fsdevMountSdmc();

  // TLS certificate checks need the real date.
  if (R_SUCCEEDED(timeInitialize())) __libnx_init_time();

  static const SocketInitConfig socket_config = {
      .tcp_tx_buf_size = 0x1000,
      .tcp_rx_buf_size = 0x2000,
      .tcp_tx_buf_max_size = 0x4000,
      .tcp_rx_buf_max_size = 0x10000,
      .udp_tx_buf_size = 0x2400,
      .udp_rx_buf_size = 0xA500,
      .sb_efficiency = 1,
      .num_bsd_sessions = 3,
      .bsd_service_type = BsdServiceType_Auto,
  };
  rc = socketInitialize(&socket_config);
  if (R_FAILED(rc)) diagAbortWithResult(rc);
}

void __appExit(void) {
  socketExit();
  timeExit();
  fsdevUnmountAll();
  fsExit();
  smExit();
}

// Stops the main thread from being killed on a crash in another thread
// without at least a log line.
alignas(16) u8 __nx_exception_stack[0x1000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  nxcord::log::error("crash: desc 0x%x pc 0x%lx lr 0x%lx far 0x%lx", ctx->error_desc, ctx->pc.x, ctx->lr.x,
                     ctx->far.x);
}
}

namespace {

using namespace nxcord;

FILE *g_log = nullptr;

void fileSink(const char *line) {
  if (!g_log) return;
  std::fputs(line, g_log);
  std::fflush(g_log);
}

void openLog() {
  mkdir("sdmc:/config", 0777);
  mkdir(kConfigDir, 0777);
  struct stat st;
  bool too_big = stat(kLogPath, &st) == 0 && st.st_size > 512 * 1024;
  g_log = std::fopen(kLogPath, too_big ? "w" : "a");
  log::setSink(fileSink);
}

void copyString(char *dst, size_t cap, const std::string &src) {
  size_t n = std::min(cap - 1, src.size());
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
  // Don't leave half a UTF-8 sequence at the end.
  while (n > 0 && (static_cast<unsigned char>(dst[n - 1]) & 0xC0) == 0x80) n--;
  if (n > 0 && (static_cast<unsigned char>(dst[n - 1]) & 0x80)) dst[n - 1] = '\0';
}

u64 toU64(const Snowflake &s) { return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 10); }
Snowflake toSnowflake(u64 v) { return v ? std::to_string(v) : Snowflake(); }

template <typename T>
bool readIn(const IpcServer::Request &r, T &out) {
  if (r.in_size < sizeof(T)) return false;
  std::memcpy(&out, r.in, sizeof(T));
  return true;
}

class NxcordService {
 public:
  NxcordService() : _audio(_client) {}

  void run() {
    _config.load();
    log::setVerbose(_config.verbose_log);
    applySettings();
    _token = readToken();
    if (_token.empty()) {
      log::info("no token in %s yet", kTokenPath);
    } else {
      _client.start(_token);
    }
    _audio.start();

    Result rc = _server.start(ipc::kServiceName, 4, [this](const IpcServer::Request &r, IpcServer::Reply &reply) {
      handle(r, reply);
    });
    if (R_FAILED(rc)) {
      log::error("could not register the nxcord service: 0x%x", rc);
      return;
    }
    log::info("NXCord-NX " NXCORD_VERSION " sysmodule running");

    // Rejoin the last channel once the gateway is up.
    bool rejoin_pending = _config.auto_rejoin && !_config.last_channel.empty();
    int64_t last_heap_log = 0;
    while (!_shutdown) {
      if (rejoin_pending && _client.status().gateway == Gateway::Status::Ready) {
        log::info("rejoining last voice channel");
        _client.joinVoice(_config.last_guild, _config.last_channel);
        rejoin_pending = false;
      }
      if (nowMs() - last_heap_log > 60'000) {
        last_heap_log = nowMs();
        struct mallinfo mi = mallinfo();
        log::info("heap: %u KB used, %u KB peak of %u KB", unsigned(mi.uordblks / 1024), unsigned(mi.arena / 1024),
                  unsigned(INNER_HEAP_SIZE / 1024));
      }
      if (!_server.process(500'000'000)) break;
    }

    log::info("shutting down");
    _client.leaveVoice();
    sleepMs(500);  // let the gateway send the leave
    _client.stop();
    _audio.stop();
  }

 private:
  void applySettings() {
    _client.encoder().setGateThreshold(_config.mic_threshold);
    _client.encoder().setGain(_config.mic_gain / 100.0f);
    _client.mixer().setMasterVolume(_config.output_volume / 100.0f);
  }

  void handle(const IpcServer::Request &r, IpcServer::Reply &reply) {
    reply.result = 0;
    switch (static_cast<ipc::Cmd>(r.command)) {
      case ipc::Cmd::GetStatus:
        reply.set(status());
        break;
      case ipc::Cmd::GetGuilds: {
        auto *out = static_cast<ipc::GuildEntry *>(r.out_buffer);
        size_t cap = r.out_buffer_size / sizeof(ipc::GuildEntry);
        u32 n = 0;
        for (auto &g : _client.guilds()) {
          if (n >= cap) break;
          out[n].id = toU64(g.id);
          copyString(out[n].name, sizeof(out[n].name), g.name.empty() ? g.id : g.name);
          n++;
        }
        reply.set(n);
        break;
      }
      case ipc::Cmd::GetChannels: {
        u64 guild_id;
        Guild g;
        if (!readIn(r, guild_id) || !_client.guild(toSnowflake(guild_id), g)) {
          reply.set(u32(0));
          break;
        }
        auto *out = static_cast<ipc::ChannelEntry *>(r.out_buffer);
        size_t cap = r.out_buffer_size / sizeof(ipc::ChannelEntry);
        u32 n = 0;
        for (auto &c : g.channels) {
          if (c.type != int(ChannelType::Voice) && c.type != int(ChannelType::Stage)) continue;
          if (n >= cap) break;
          out[n].id = toU64(c.id);
          copyString(out[n].name, sizeof(out[n].name), c.name);
          out[n].member_count = static_cast<u32>(std::count_if(
              g.voice.begin(), g.voice.end(), [&](auto &kv) { return kv.second.channel_id == c.id; }));
          n++;
        }
        reply.set(n);
        break;
      }
      case ipc::Cmd::GetMembers: {
        struct {
          u64 guild, channel;
        } in;
        Guild g;
        if (!readIn(r, in) || !_client.guild(toSnowflake(in.guild), g)) {
          reply.set(u32(0));
          break;
        }
        Snowflake channel = toSnowflake(in.channel);
        Snowflake self = _client.status().user_id;
        auto speaking = _client.mixer().speakingUsers();
        auto *out = static_cast<ipc::MemberEntry *>(r.out_buffer);
        size_t cap = r.out_buffer_size / sizeof(ipc::MemberEntry);
        u32 n = 0;
        for (auto &[uid, vm] : g.voice) {
          if (vm.channel_id != channel) continue;
          if (n >= cap) break;
          auto name = g.names.find(uid);
          out[n] = {};
          out[n].id = toU64(uid);
          copyString(out[n].name, sizeof(out[n].name), name != g.names.end() ? name->second : uid);
          out[n].speaking = speaking.count(uid) ? 1 : 0;
          out[n].self_mute = vm.self_mute || vm.mute;
          out[n].self_deaf = vm.self_deaf || vm.deaf;
          out[n].is_self = uid == self;
          out[n].volume = static_cast<u32>(_client.mixer().userVolume(uid) * 100 + 0.5f);
          n++;
        }
        reply.set(n);
        break;
      }
      case ipc::Cmd::Join: {
        struct {
          u64 guild, channel;
        } in;
        if (!readIn(r, in)) break;
        _config.last_guild = toSnowflake(in.guild);
        _config.last_channel = toSnowflake(in.channel);
        _config.save();
        _client.joinVoice(_config.last_guild, _config.last_channel);
        break;
      }
      case ipc::Cmd::Leave:
        _config.last_guild.clear();
        _config.last_channel.clear();
        _config.save();
        _client.leaveVoice();
        break;
      case ipc::Cmd::SetMute: {
        u8 v = 0;
        readIn(r, v);
        _client.setSelfMute(v);
        break;
      }
      case ipc::Cmd::SetDeaf: {
        u8 v = 0;
        readIn(r, v);
        _client.setSelfDeaf(v);
        break;
      }
      case ipc::Cmd::SetUserVolume: {
        struct {
          u64 user;
          u32 percent;
        } in;
        if (readIn(r, in)) _client.mixer().setUserVolume(toSnowflake(in.user), in.percent / 100.0f);
        break;
      }
      case ipc::Cmd::GetSettings: {
        ipc::Settings s{};
        s.mic_threshold = static_cast<u32>(_config.mic_threshold);
        s.mic_gain = static_cast<u32>(_config.mic_gain);
        s.output_volume = static_cast<u32>(_config.output_volume);
        s.auto_rejoin = _config.auto_rejoin;
        reply.set(s);
        break;
      }
      case ipc::Cmd::SetSettings: {
        ipc::Settings s;
        if (!readIn(r, s)) break;
        _config.mic_threshold = static_cast<int>(std::min<u32>(s.mic_threshold, 32767));
        _config.mic_gain = static_cast<int>(std::min<u32>(s.mic_gain, 400));
        _config.output_volume = static_cast<int>(std::min<u32>(s.output_volume, 200));
        _config.auto_rejoin = s.auto_rejoin;
        _config.save();
        applySettings();
        break;
      }
      case ipc::Cmd::Shutdown:
        _shutdown = true;
        break;
      case ipc::Cmd::Reconnect:
        _token = readToken();
        if (_token.empty()) {
          _client.stop();
        } else {
          _client.start(_token);
        }
        break;
      default:
        reply.result = MAKERESULT(Module_Libnx, LibnxError_NotFound);
        break;
    }
  }

  ipc::Status status() {
    auto st = _client.status();
    ipc::Status s{};
    s.protocol_version = ipc::kProtocolVersion;
    if (_token.empty()) {
      s.gateway = ipc::GatewayState::NoToken;
    } else {
      switch (st.gateway) {
        case Gateway::Status::Disconnected: s.gateway = ipc::GatewayState::Disconnected; break;
        case Gateway::Status::Connecting: s.gateway = ipc::GatewayState::Connecting; break;
        case Gateway::Status::Ready: s.gateway = ipc::GatewayState::Ready; break;
        case Gateway::Status::AuthFailed: s.gateway = ipc::GatewayState::AuthFailed; break;
      }
    }
    s.voice = static_cast<ipc::VoiceState>(st.voice);
    s.e2ee = st.e2ee;
    s.self_mute = st.self_mute;
    s.self_deaf = st.self_deaf;
    s.mic_available = _audio.micAvailable();
    s.speaker_available = _audio.speakerAvailable();
    s.mic_level = static_cast<u32>(st.mic_level);
    s.voice_guild = toU64(st.voice_guild);
    s.voice_channel = toU64(st.voice_channel);
    struct mallinfo mi = mallinfo();
    s.heap_used_kb = static_cast<u32>(mi.uordblks / 1024);
    s.heap_peak_kb = static_cast<u32>(mi.arena / 1024);
    copyString(s.error, sizeof(s.error), !st.voice_error.empty() ? st.voice_error : st.gateway_error);
    Guild g;
    if (!st.voice_guild.empty() && _client.guild(st.voice_guild, g)) {
      copyString(s.guild_name, sizeof(s.guild_name), g.name);
      for (auto &c : g.channels) {
        if (c.id == st.voice_channel) copyString(s.channel_name, sizeof(s.channel_name), c.name);
      }
    }
    return s;
  }

  Client _client;
  AudioIo _audio;
  IpcServer _server;
  Config _config;
  std::string _token;
  bool _shutdown = false;
};

}  // namespace

int main() {
  openLog();
  static NxcordService service;
  service.run();
  return 0;
}
