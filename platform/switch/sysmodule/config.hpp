#pragma once

#include <string>

namespace nxcord {

constexpr const char *kConfigDir = "sdmc:/config/nxcord";
constexpr const char *kTokenPath = "sdmc:/config/nxcord/token.txt";
constexpr const char *kSettingsPath = "sdmc:/config/nxcord/settings.ini";
constexpr const char *kLogPath = "sdmc:/config/nxcord/log.txt";

struct Config {
  int mic_threshold = 600;
  int mic_gain = 100;
  int output_volume = 100;
  bool auto_rejoin = true;
  bool verbose_log = false;
  std::string last_guild;
  std::string last_channel;

  void load();
  void save() const;
};

std::string readToken();

}  // namespace nxcord
