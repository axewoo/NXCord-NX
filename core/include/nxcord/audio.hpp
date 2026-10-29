#pragma once

#include <nxcord/model.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <vector>

struct OpusDecoder;
struct OpusEncoder;

namespace nxcord {

// Discord audio is always 48 kHz stereo in 20 ms frames.
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrameSamples = 960;                       // per channel
constexpr int kFrameValues = kFrameSamples * kChannels;  // interleaved int16 values

// Decodes everyone's Opus streams into small jitter buffers and mixes them.
class AudioMixer {
 public:
  ~AudioMixer();

  // Voice thread: one received Opus frame.
  void push(uint32_t ssrc, const Snowflake &user_id, uint16_t seq, const uint8_t *opus, size_t len);
  // Playback thread: mixes one 20 ms frame into out (kFrameValues int16s).
  // Returns false if nobody is talking (out is zeroed).
  bool mix(int16_t *out);
  void clear();

  void setUserVolume(const Snowflake &user_id, float volume);  // 0..2
  float userVolume(const Snowflake &user_id) const;
  void setMasterVolume(float volume) { _master = volume; }
  // Users heard in roughly the last 300 ms.
  std::set<Snowflake> speakingUsers() const;

 private:
  static constexpr size_t kMaxBuffered = 8;  // 160 ms
  static constexpr int kMaxPacketSamples = 5760;  // 120 ms, the largest Opus packet

  // Fixed-size buffers only: the sysmodule heap is small, and allocating
  // per packet fragments it until allocations fail mid-call.
  struct Stream {
    Snowflake user_id;
    OpusDecoder *decoder = nullptr;
    std::array<std::array<int16_t, kFrameValues>, kMaxBuffered> ring;
    size_t head = 0, count = 0;
    bool started = false;
    bool have_seq = false;
    uint16_t last_seq = 0;
    int64_t last_heard = 0;
  };

  static void enqueue(Stream &s, const int16_t *frame);

  mutable std::mutex _mutex;
  std::map<uint32_t, Stream> _streams;
  std::array<int16_t, kMaxPacketSamples * kChannels> _decoded;
  std::array<int32_t, kFrameValues> _acc;
  std::map<Snowflake, float> _volumes;
  float _master = 1.0f;
};

// Opus encoder plus a simple level gate, so silence isn't transmitted.
class MicEncoder {
 public:
  MicEncoder();
  ~MicEncoder();

  // Encodes one 20 ms frame. Returns the Opus size, 0 if the gate is closed
  // (caller should stop speaking), or -1 on error.
  int encode(const int16_t *pcm, uint8_t *out, size_t out_cap);
  // 0 = always transmit, otherwise RMS threshold on a 0..32767 scale.
  void setGateThreshold(int threshold) { _threshold = threshold; }
  void setGain(float gain) { _gain = gain; }
  int lastLevel() const { return _last_level; }

 private:
  OpusEncoder *_encoder = nullptr;
  int _threshold = 600;
  float _gain = 1.0f;
  int _hangover = 0;
  int _last_level = 0;
  std::vector<int16_t> _scaled;
};

}  // namespace nxcord
