#pragma once

// IPC between the NXCord sysmodule (server) and the overlay (client).
// Service "nxcord". Small requests/replies go in raw data; lists come back
// through one HipcMapAlias out-buffer.

#include <switch.h>

#include <cstdint>

namespace nxcord::ipc {

constexpr const char *kServiceName = "nxcord";
constexpr u32 kProtocolVersion = 1;

enum class Cmd : u32 {
  GetStatus = 0,       // out: Status
  GetGuilds = 1,       // out buffer: GuildEntry[], out: u32 count
  GetChannels = 2,     // in: u64 guild; out buffer: ChannelEntry[], out: u32 count
  GetMembers = 3,      // in: u64 guild, u64 channel; out buffer: MemberEntry[], out: u32 count
  Join = 4,            // in: u64 guild, u64 channel
  Leave = 5,
  SetMute = 6,         // in: u8
  SetDeaf = 7,         // in: u8
  SetUserVolume = 8,   // in: u64 user, u32 percent (0-200)
  GetSettings = 9,     // out: Settings
  SetSettings = 10,    // in: Settings
  Reconnect = 11,      // re-reads the token file and reconnects
};

enum class GatewayState : u8 { NoToken, Disconnected, Connecting, Ready, AuthFailed };
enum class VoiceState : u8 { Idle, Joining, Connected, Failed };

struct Status {
  u32 protocol_version;
  GatewayState gateway;
  VoiceState voice;
  u8 e2ee;
  u8 self_mute;
  u8 self_deaf;
  u8 mic_available;
  u8 speaker_available;
  u8 reserved;
  u32 mic_level;  // RMS 0..32767 of the last mic frame
  u64 voice_guild;
  u64 voice_channel;
  char error[64];
  char guild_name[32];
  char channel_name[32];
};

struct Settings {
  u32 mic_threshold;  // RMS gate, 0 = always transmit
  u32 mic_gain;       // percent
  u32 output_volume;  // percent
  u8 auto_rejoin;     // rejoin the last channel after boot / reconnect
  u8 reserved[3];
};

struct GuildEntry {
  u64 id;
  char name[64];
};

struct ChannelEntry {
  u64 id;
  char name[64];
  u32 member_count;
};

struct MemberEntry {
  u64 id;
  char name[48];
  u8 speaking;
  u8 self_mute;
  u8 self_deaf;
  u8 is_self;
  u32 volume;  // percent
};

static_assert(sizeof(Status) <= 0xB0, "Status must fit in a raw IPC reply");

}  // namespace nxcord::ipc
