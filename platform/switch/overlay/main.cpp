// NXCord-NX overlay for Ultrahand / Tesla: join voice channels, mute,
// deafen and adjust volumes while a game is running. All the Discord work
// happens in the sysmodule; this only talks to it over IPC.
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <sys/stat.h>

#include <cstdio>
#include <string>

#include "nxcord_service.hpp"

using namespace nxcord;
using nxcord::ovl::service;

namespace {

constexpr const char *kTitle = "NXCord";

// What the sysmodule takes from the system pool (code + heap + stack,
// measured from the build), and what we leave free for Horizon after that.
constexpr u64 kSysmoduleFootprint = NXCORD_SYS_FOOTPRINT;
constexpr u64 kMinFreeAfterStart = 0x180000;

std::string megabytes(u64 bytes) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
  return buf;
}

bool bootFlagSet() {
  struct stat st;
  return stat(ipc::kBootFlagPath, &st) == 0;
}

void setBootFlag(bool on) {
  if (on) {
    mkdir("sdmc:/atmosphere/contents/5600000000000000/flags", 0777);
    if (FILE *f = std::fopen(ipc::kBootFlagPath, "w")) std::fclose(f);
  } else {
    std::remove(ipc::kBootFlagPath);
  }
}

// Guis to pop after joining a channel. Popping inside a click listener would
// destroy the element that is running, so update() does it a frame later.
int g_pending_back = 0;

bool popPending() {
  if (g_pending_back <= 0) return false;
  g_pending_back--;
  tsl::goBack();
  return true;
}

std::string gatewayText(const ipc::Status &s) {
  switch (s.gateway) {
    case ipc::GatewayState::NoToken: return "No token";
    case ipc::GatewayState::Disconnected: return "Offline";
    case ipc::GatewayState::Connecting: return "Connecting...";
    case ipc::GatewayState::Ready: return "Online";
    case ipc::GatewayState::AuthFailed: return "Bad token";
  }
  return "?";
}

std::string voiceText(const ipc::Status &s) {
  switch (s.voice) {
    case ipc::VoiceState::Idle: return "Not in a channel";
    case ipc::VoiceState::Joining: return "Joining...";
    case ipc::VoiceState::Connected: return std::string(s.e2ee ? " " : "") + s.channel_name;
    case ipc::VoiceState::Failed: return "Failed";
  }
  return "?";
}

class GuiMain;

// Shown when the sysmodule isn't running. It doesn't start at boot by
// default, so this is where you turn it on.
class GuiNoService : public tsl::Gui {
 public:
  tsl::elm::Element *createUI() override {
    auto *frame = new tsl::elm::OverlayFrame(kTitle, NXCORD_VERSION);
    auto *list = new tsl::elm::List();
    list->addItem(new tsl::elm::CategoryHeader("NXCord is off"));
    _start = new tsl::elm::ListItem("Start NXCord");
    _start->setClickListener([this](u64 keys) {
      if (!(keys & HidNpadButton_A) || _launched) return false;
      start();
      return true;
    });
    list->addItem(_start);
    _memory = new tsl::elm::ListItem("System memory free", megabytes(ovl::systemPoolFree()));
    list->addItem(_memory);
    list->addItem(new tsl::elm::ListItem("NXCord needs", megabytes(kSysmoduleFootprint)));
    list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
                    r->drawString("Put your bot token in\n/config/nxcord/token.txt first.\n\n"
                                  "If it won't start, close other\nsysmodules to free memory.",
                                  false, x + 20, y + 30, 18, r->a(tsl::style::color::ColorText));
                  }),
                  130);
    frame->setContent(list);
    return frame;
  }

  void update() override {
    // Back from the main screen: close instead of showing this again.
    if (_shown_main) {
      tsl::goBack();
      return;
    }
    if (!_launched || ++_ticks % 15 != 0) return;
    if (R_SUCCEEDED(service().open())) {
      _shown_main = true;
      tsl::changeTo<GuiMain>();
    } else if (_ticks > 60 * 10) {
      _launched = false;
      _start->setText("Start NXCord");
      _start->setValue("didn't start", true);
    }
  }

 private:
  void start() {
    u64 free = ovl::systemPoolFree();
    _memory->setValue(megabytes(free));
    if (free < kSysmoduleFootprint + kMinFreeAfterStart) {
      _start->setValue("not enough memory", true);
      return;
    }
    if (R_FAILED(pmshellInitialize())) {
      _start->setValue("pm unavailable", true);
      return;
    }
    NcmProgramLocation loc{.program_id = ipc::kProgramId, .storageID = NcmStorageId_None};
    u64 pid = 0;
    Result rc = pmshellLaunchProgram(0, &loc, &pid);
    // Still running but stuck (its service is gone): end it and try again.
    if (R_FAILED(rc) && R_SUCCEEDED(pmshellTerminateProgram(ipc::kProgramId))) {
      svcSleepThread(500'000'000);
      rc = pmshellLaunchProgram(0, &loc, &pid);
    }
    pmshellExit();
    if (R_FAILED(rc)) {
      char err[32];
      std::snprintf(err, sizeof(err), "error 0x%x", rc);
      _start->setValue(err, true);
      return;
    }
    _launched = true;
    _ticks = 0;
    _start->setValue("starting...", true);
  }

  tsl::elm::ListItem *_start = nullptr, *_memory = nullptr;
  bool _launched = false, _shown_main = false;
  u32 _ticks = 0;
};

class GuiMembers : public tsl::Gui {
 public:
  GuiMembers(u64 guild, u64 channel, std::string name) : _guild(guild), _channel(channel), _name(std::move(name)) {}

  tsl::elm::Element *createUI() override {
    auto *frame = new tsl::elm::OverlayFrame(kTitle, _name);
    auto *list = new tsl::elm::List();
    auto members = service().members(_guild, _channel);
    if (members.empty()) list->addItem(new tsl::elm::CategoryHeader("Nobody is here"));
    for (auto &m : members) {
      auto *item = new tsl::elm::ListItem(m.name);
      _items.push_back({m.id, item, m.is_self != 0});
      if (!m.is_self) {
        list->addItem(new tsl::elm::CategoryHeader(std::string(m.name) + " volume"));
        list->addItem(item);
        auto *bar = new tsl::elm::StepTrackBar("", 9);  // 0%..200% in 25% steps
        bar->setProgress(static_cast<u8>(std::min<u32>(m.volume, 200) / 25));
        u64 id = m.id;
        bar->setValueChangedListener([id](u8 step) { service().setUserVolume(id, step * 25); });
        list->addItem(bar);
      } else {
        list->addItem(item);
      }
    }
    frame->setContent(list);
    refresh(members);
    return frame;
  }

  void update() override {
    if (++_ticks % 15 != 0) return;  // ~4 times a second
    refresh(service().members(_guild, _channel));
  }

 private:
  struct Row {
    u64 id;
    tsl::elm::ListItem *item;
    bool self;
  };

  void refresh(const std::vector<ipc::MemberEntry> &members) {
    for (auto &row : _items) {
      std::string value = "left";
      for (auto &m : members) {
        if (m.id != row.id) continue;
        value = m.speaking ? "Speaking" : "";
        if (m.self_deaf) value = "Deafened";
        else if (m.self_mute) value = "Muted";
        if (row.self && value.empty()) value = "You";
      }
      row.item->setValue(value, value != "Speaking");
    }
  }

  u64 _guild, _channel;
  std::string _name;
  std::vector<Row> _items;
  u32 _ticks = 0;
};

class GuiChannels : public tsl::Gui {
 public:
  GuiChannels(u64 guild, std::string name) : _guild(guild), _name(std::move(name)) {}

  void update() override { popPending(); }

  tsl::elm::Element *createUI() override {
    auto *frame = new tsl::elm::OverlayFrame(kTitle, _name);
    auto *list = new tsl::elm::List();
    auto channels = service().channels(_guild);
    list->addItem(new tsl::elm::CategoryHeader(channels.empty() ? "No voice channels" : "Voice channels"));
    for (auto &c : channels) {
      auto *item = new tsl::elm::ListItem(c.name, c.member_count ? std::to_string(c.member_count) + " in" : "");
      u64 guild = _guild, channel = c.id;
      item->setClickListener([guild, channel](u64 keys) {
        if (!(keys & HidNpadButton_A)) return false;
        service().join(guild, channel);
        g_pending_back = 2;
        return true;
      });
      list->addItem(item);
    }
    frame->setContent(list);
    return frame;
  }

 private:
  u64 _guild;
  std::string _name;
};

class GuiGuilds : public tsl::Gui {
 public:
  void update() override { popPending(); }

  tsl::elm::Element *createUI() override {
    auto *frame = new tsl::elm::OverlayFrame(kTitle, "Join a channel");
    auto *list = new tsl::elm::List();
    auto guilds = service().guilds();
    if (guilds.empty()) {
      list->addItem(new tsl::elm::CategoryHeader("No servers"));
      list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *r, s32 x, s32 y, s32 w, s32 h) {
                      r->drawString("Invite your bot to a server\nwith the OAuth2 link first.", false, x + 20, y + 30,
                                    18, r->a(tsl::style::color::ColorText));
                    }),
                    90);
    }
    for (auto &g : guilds) {
      auto *item = new tsl::elm::ListItem(g.name);
      u64 id = g.id;
      std::string name = g.name;
      item->setClickListener([id, name](u64 keys) {
        if (!(keys & HidNpadButton_A)) return false;
        tsl::changeTo<GuiChannels>(id, name);
        return true;
      });
      list->addItem(item);
    }
    frame->setContent(list);
    return frame;
  }
};

class GuiSettings : public tsl::Gui {
 public:
  tsl::elm::Element *createUI() override {
    service().settings(_settings);
    auto *frame = new tsl::elm::OverlayFrame(kTitle, "Settings");
    auto *list = new tsl::elm::List();

    list->addItem(new tsl::elm::CategoryHeader("Mic sensitivity (left = always on)"));
    _level = new tsl::elm::ListItem("Mic level");
    list->addItem(_level);
    auto *gate = new tsl::elm::StepTrackBar("", 11);  // threshold 0..2000 in steps of 200
    gate->setProgress(static_cast<u8>(std::min<u32>(_settings.mic_threshold, 2000) / 200));
    gate->setValueChangedListener([this](u8 step) {
      _settings.mic_threshold = step * 200;
      service().setSettings(_settings);
    });
    list->addItem(gate);

    list->addItem(new tsl::elm::CategoryHeader("Mic boost"));
    auto *gain = new tsl::elm::StepTrackBar("", 9);  // 0..400%
    gain->setProgress(static_cast<u8>(std::min<u32>(_settings.mic_gain, 400) / 50));
    gain->setValueChangedListener([this](u8 step) {
      _settings.mic_gain = step * 50;
      service().setSettings(_settings);
    });
    list->addItem(gain);

    list->addItem(new tsl::elm::CategoryHeader("Call volume"));
    auto *vol = new tsl::elm::StepTrackBar("", 9);  // 0..200%
    vol->setProgress(static_cast<u8>(std::min<u32>(_settings.output_volume, 200) / 25));
    vol->setValueChangedListener([this](u8 step) {
      _settings.output_volume = step * 25;
      service().setSettings(_settings);
    });
    list->addItem(vol);

    list->addItem(new tsl::elm::CategoryHeader("Connection"));
    auto *rejoin = new tsl::elm::ToggleListItem("Rejoin last channel", _settings.auto_rejoin);
    rejoin->setStateChangedListener([this](bool on) {
      _settings.auto_rejoin = on;
      service().setSettings(_settings);
    });
    list->addItem(rejoin);
    auto *reconnect = new tsl::elm::ListItem("Reload token & reconnect");
    reconnect->setClickListener([](u64 keys) {
      if (!(keys & HidNpadButton_A)) return false;
      service().reconnect();
      return true;
    });
    list->addItem(reconnect);
    auto *boot = new tsl::elm::ToggleListItem("Start at boot", bootFlagSet());
    boot->setStateChangedListener([](bool on) { setBootFlag(on); });
    list->addItem(boot);
    auto *off = new tsl::elm::ListItem("Turn NXCord off");
    off->setClickListener([](u64 keys) {
      if (!(keys & HidNpadButton_A)) return false;
      service().shutdown();
      service().close();
      tsl::Overlay::get()->close();
      return true;
    });
    list->addItem(off);

    list->addItem(new tsl::elm::CategoryHeader("Memory"));
    _memory = new tsl::elm::ListItem("NXCord heap");
    list->addItem(_memory);
    _free = new tsl::elm::ListItem("System free");
    list->addItem(_free);

    frame->setContent(list);
    return frame;
  }

  void update() override {
    if (++_ticks % 6 != 0) return;
    ipc::Status st;
    if (R_FAILED(service().status(st))) return;
    _memory->setValue(std::to_string(st.heap_used_kb) + " / " + std::to_string(st.heap_peak_kb) + " KB");
    _free->setValue(megabytes(ovl::systemPoolFree()));
    if (!st.mic_available) {
      _level->setValue("No mic", true);
      return;
    }
    // A little bar so you can see where the gate sits.
    int bars = std::min<int>(10, st.mic_level / 200);
    std::string meter(bars, '|');
    meter += std::string(10 - bars, '.');
    _level->setValue(meter + " " + std::to_string(st.mic_level), st.mic_level < _settings.mic_threshold);
  }

 private:
  ipc::Settings _settings{};
  tsl::elm::ListItem *_level = nullptr, *_memory = nullptr, *_free = nullptr;
  u32 _ticks = 0;
};

class GuiMain : public tsl::Gui {
 public:
  tsl::elm::Element *createUI() override {
    service().status(_status);
    _frame = new tsl::elm::OverlayFrame(kTitle, NXCORD_VERSION);
    auto *list = new tsl::elm::List();

    _discord = new tsl::elm::ListItem("Discord", gatewayText(_status));
    list->addItem(_discord);
    _voice = new tsl::elm::ListItem("Voice", voiceText(_status));
    _voice->setClickListener([this](u64 keys) {
      if (!(keys & HidNpadButton_A) || _status.voice != ipc::VoiceState::Connected) return false;
      tsl::changeTo<GuiMembers>(_status.voice_guild, _status.voice_channel, _status.channel_name);
      return true;
    });
    list->addItem(_voice);

    list->addItem(new tsl::elm::CategoryHeader("Call"));
    _mute = new tsl::elm::ToggleListItem("Mute mic", _status.self_mute);
    _mute->setStateChangedListener([](bool on) { service().setMute(on); });
    list->addItem(_mute);
    _deaf = new tsl::elm::ToggleListItem("Deafen", _status.self_deaf);
    _deaf->setStateChangedListener([](bool on) { service().setDeaf(on); });
    list->addItem(_deaf);

    auto *join = new tsl::elm::ListItem("Join a channel");
    join->setClickListener([](u64 keys) {
      if (!(keys & HidNpadButton_A)) return false;
      tsl::changeTo<GuiGuilds>();
      return true;
    });
    list->addItem(join);
    auto *leave = new tsl::elm::ListItem("Leave channel");
    leave->setClickListener([](u64 keys) {
      if (!(keys & HidNpadButton_A)) return false;
      service().leave();
      return true;
    });
    list->addItem(leave);

    list->addItem(new tsl::elm::CategoryHeader("More"));
    auto *settings = new tsl::elm::ListItem("Settings");
    settings->setClickListener([](u64 keys) {
      if (!(keys & HidNpadButton_A)) return false;
      tsl::changeTo<GuiSettings>();
      return true;
    });
    list->addItem(settings);

    _error = new tsl::elm::ListItem("Last error", "");
    list->addItem(_error);

    _frame->setContent(list);
    refresh();
    return _frame;
  }

  void update() override {
    if (++_ticks % 20 != 0) return;
    if (R_SUCCEEDED(service().status(_status))) {
      refresh();
    } else {
      _discord->setValue("NXCord stopped", true);
      _voice->setValue("", true);
    }
  }

 private:
  void refresh() {
    _discord->setValue(gatewayText(_status), _status.gateway != ipc::GatewayState::Ready);
    std::string v = voiceText(_status);
    if (_status.voice == ipc::VoiceState::Connected && !_status.mic_available) v += " (no mic)";
    _voice->setValue(v, _status.voice != ipc::VoiceState::Connected);
    _mute->setState(_status.self_mute);
    _deaf->setState(_status.self_deaf);
    std::string err = _status.error;
    _error->setValue(err.empty() ? "none" : err, true);
  }

  ipc::Status _status{};
  tsl::elm::OverlayFrame *_frame = nullptr;
  tsl::elm::ListItem *_discord = nullptr, *_voice = nullptr, *_error = nullptr;
  tsl::elm::ToggleListItem *_mute = nullptr, *_deaf = nullptr;
  u32 _ticks = 0;
};

class NxcordOverlay : public tsl::Overlay {
 public:
  void initServices() override { service().open(); }
  void exitServices() override { service().close(); }

  std::unique_ptr<tsl::Gui> loadInitialGui() override {
    if (!service().active()) return initially<GuiNoService>();
    return initially<GuiMain>();
  }
};

}  // namespace

int main(int argc, char **argv) { return tsl::loop<NxcordOverlay>(argc, argv); }
