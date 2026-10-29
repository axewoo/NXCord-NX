#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nxcord {

// Snowflakes are kept as strings: that's how Discord sends them and how
// libdave wants user ids.
using Snowflake = std::string;

enum class ChannelType : int { Text = 0, Voice = 2, Category = 4, Stage = 13 };

struct Channel {
  Snowflake id;
  std::string name;
  int type = 0;
  int position = 0;
  Snowflake parent_id;
};

struct VoiceMember {
  Snowflake user_id;
  Snowflake channel_id;
  bool self_mute = false;
  bool self_deaf = false;
  bool mute = false;
  bool deaf = false;
};

struct Guild {
  Snowflake id;
  std::string name;
  std::vector<Channel> channels;           // sorted by position
  std::map<Snowflake, VoiceMember> voice;  // user_id -> voice state
  std::map<Snowflake, std::string> names;  // user_id -> display name
};

}  // namespace nxcord
