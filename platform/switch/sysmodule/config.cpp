#include "config.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace nxcord {

namespace {
std::string trim(std::string s) {
  const char *ws = " \t\r\n";
  s.erase(0, s.find_first_not_of(ws));
  s.erase(s.find_last_not_of(ws) + 1);
  return s;
}
}  // namespace

void Config::load() {
  std::ifstream f(kSettingsPath);
  std::string line;
  while (std::getline(f, line)) {
    size_t eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#' || line[0] == ';') continue;
    std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    if (key == "mic_threshold") mic_threshold = std::atoi(value.c_str());
    else if (key == "mic_gain") mic_gain = std::atoi(value.c_str());
    else if (key == "output_volume") output_volume = std::atoi(value.c_str());
    else if (key == "auto_rejoin") auto_rejoin = value == "1" || value == "true";
    else if (key == "verbose_log") verbose_log = value == "1" || value == "true";
    else if (key == "last_guild") last_guild = value;
    else if (key == "last_channel") last_channel = value;
  }
}

void Config::save() const {
  mkdir("sdmc:/config", 0777);
  mkdir(kConfigDir, 0777);
  FILE *f = std::fopen(kSettingsPath, "w");
  if (!f) return;
  std::fprintf(f,
               "# NXCord-NX settings (also editable from the overlay)\n"
               "mic_threshold=%d\nmic_gain=%d\noutput_volume=%d\nauto_rejoin=%d\nverbose_log=%d\n"
               "last_guild=%s\nlast_channel=%s\n",
               mic_threshold, mic_gain, output_volume, auto_rejoin ? 1 : 0, verbose_log ? 1 : 0,
               last_guild.c_str(), last_channel.c_str());
  std::fclose(f);
}

std::string readToken() {
  std::ifstream f(kTokenPath);
  std::string token;
  std::getline(f, token);
  token = trim(token);
  // Accept a pasted "Bot xxxx" too.
  if (token.rfind("Bot ", 0) == 0) token = token.substr(4);
  return token;
}

}  // namespace nxcord
