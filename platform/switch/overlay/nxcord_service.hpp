#pragma once

#include <switch.h>

#include <algorithm>
#include <vector>

#include "../common/nxcord_ipc.hpp"

namespace nxcord::ovl {

// Thin client for the sysmodule's "nxcord" service.
class NxcordService {
 public:
  Result open() {
    if (serviceIsActive(&_srv)) return 0;
    return smGetService(&_srv, ipc::kServiceName);
  }
  void close() { serviceClose(&_srv); }
  bool active() { return serviceIsActive(&_srv); }

  Result status(ipc::Status &out) { return serviceDispatchOut(&_srv, u32(ipc::Cmd::GetStatus), out); }
  Result settings(ipc::Settings &out) { return serviceDispatchOut(&_srv, u32(ipc::Cmd::GetSettings), out); }
  Result setSettings(const ipc::Settings &in) { return serviceDispatchIn(&_srv, u32(ipc::Cmd::SetSettings), in); }
  Result join(u64 guild, u64 channel) {
    struct {
      u64 guild, channel;
    } in{guild, channel};
    return serviceDispatchIn(&_srv, u32(ipc::Cmd::Join), in);
  }
  Result leave() { return serviceDispatch(&_srv, u32(ipc::Cmd::Leave)); }
  Result setMute(bool v) {
    u8 in = v;
    return serviceDispatchIn(&_srv, u32(ipc::Cmd::SetMute), in);
  }
  Result setDeaf(bool v) {
    u8 in = v;
    return serviceDispatchIn(&_srv, u32(ipc::Cmd::SetDeaf), in);
  }
  Result setUserVolume(u64 user, u32 percent) {
    struct {
      u64 user;
      u32 percent;
    } in{user, percent};
    return serviceDispatchIn(&_srv, u32(ipc::Cmd::SetUserVolume), in);
  }
  Result reconnect() { return serviceDispatch(&_srv, u32(ipc::Cmd::Reconnect)); }

  std::vector<ipc::GuildEntry> guilds() { return list<ipc::GuildEntry>(ipc::Cmd::GetGuilds, nullptr, 0, 100); }
  std::vector<ipc::ChannelEntry> channels(u64 guild) {
    return list<ipc::ChannelEntry>(ipc::Cmd::GetChannels, &guild, sizeof(guild), 100);
  }
  std::vector<ipc::MemberEntry> members(u64 guild, u64 channel) {
    u64 in[2] = {guild, channel};
    return list<ipc::MemberEntry>(ipc::Cmd::GetMembers, in, sizeof(in), 64);
  }

 private:
  template <typename T>
  std::vector<T> list(ipc::Cmd cmd, const void *in, size_t in_size, size_t max) {
    std::vector<T> out(max);
    u32 count = 0;
    Result rc = serviceDispatchImpl(&_srv, u32(cmd), in, in_size, &count, sizeof(count),
                                    SfDispatchParams{
                                        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                        .buffers = {{out.data(), out.size() * sizeof(T)}},
                                    });
    if (R_FAILED(rc)) count = 0;
    out.resize(std::min<size_t>(count, max));
    return out;
  }

  Service _srv{};
};

inline NxcordService &service() {
  static NxcordService s;
  return s;
}

}  // namespace nxcord::ovl
