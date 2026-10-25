#include <nxcord/audio.hpp>
#include <nxcord/log.hpp>
#include <nxcord/util.hpp>

#include <opus/opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nxcord {

namespace {
constexpr size_t kMaxBuffered = 8;  // 160 ms
constexpr size_t kStartFrames = 2;  // 40 ms of jitter buffer before playing
constexpr int kMaxConcealed = 3;
constexpr int kHangoverFrames = 15;  // keep the gate open 300 ms after speech
}  // namespace

AudioMixer::~AudioMixer() { clear(); }

void AudioMixer::clear() {
  std::scoped_lock lock(_mutex);
  for (auto &[ssrc, s] : _streams) {
    if (s.decoder) opus_decoder_destroy(s.decoder);
  }
  _streams.clear();
}

void AudioMixer::push(uint32_t ssrc, const Snowflake &user_id, uint16_t seq, const uint8_t *opus, size_t len) {
  std::scoped_lock lock(_mutex);
  Stream &s = _streams[ssrc];
  s.user_id = user_id;
  if (!s.decoder) {
    int err = 0;
    s.decoder = opus_decoder_create(kSampleRate, kChannels, &err);
    if (err != OPUS_OK) {
      log::error("audio: opus_decoder_create failed (%d)", err);
      _streams.erase(ssrc);
      return;
    }
  }

  if (s.have_seq) {
    int16_t gap = static_cast<int16_t>(seq - s.last_seq);
    if (gap <= 0) return;  // duplicate or late packet
    for (int i = 1; i < gap && i <= kMaxConcealed; i++) {
      std::vector<int16_t> pcm(kFrameValues);
      int n = opus_decode(s.decoder, nullptr, 0, pcm.data(), kFrameSamples, 0);
      if (n > 0) {
        pcm.resize(static_cast<size_t>(n) * kChannels);
        s.frames.push_back(std::move(pcm));
      }
    }
  }
  s.have_seq = true;
  s.last_seq = seq;

  std::vector<int16_t> pcm(5760 * kChannels);
  int n = opus_decode(s.decoder, opus, static_cast<opus_int32>(len), pcm.data(), 5760, 0);
  if (n <= 0) return;
  pcm.resize(static_cast<size_t>(n) * kChannels);
  // Discord's silence frames decode to silence; still count as a frame.
  s.frames.push_back(std::move(pcm));
  while (s.frames.size() > kMaxBuffered) s.frames.pop_front();
  if (len > 3) s.last_heard = nowMs();
}

bool AudioMixer::mix(int16_t *out) {
  std::vector<int32_t> acc(kFrameValues, 0);
  bool any = false;
  {
    std::scoped_lock lock(_mutex);
    for (auto &[ssrc, s] : _streams) {
      if (!s.started) {
        if (s.frames.size() < kStartFrames) continue;
        s.started = true;
      }
      if (s.frames.empty()) {
        s.started = false;  // underrun: rebuffer
        continue;
      }
      std::vector<int16_t> frame = std::move(s.frames.front());
      s.frames.pop_front();
      float vol = _master;
      if (auto it = _volumes.find(s.user_id); it != _volumes.end()) vol *= it->second;
      size_t n = std::min<size_t>(frame.size(), kFrameValues);
      for (size_t i = 0; i < n; i++) acc[i] += static_cast<int32_t>(frame[i] * vol);
      any = true;
    }
  }
  for (int i = 0; i < kFrameValues; i++) out[i] = static_cast<int16_t>(std::clamp(acc[i], -32768, 32767));
  return any;
}

void AudioMixer::setUserVolume(const Snowflake &user_id, float volume) {
  std::scoped_lock lock(_mutex);
  _volumes[user_id] = std::clamp(volume, 0.0f, 2.0f);
}

float AudioMixer::userVolume(const Snowflake &user_id) const {
  std::scoped_lock lock(_mutex);
  auto it = _volumes.find(user_id);
  return it == _volumes.end() ? 1.0f : it->second;
}

std::set<Snowflake> AudioMixer::speakingUsers() const {
  std::scoped_lock lock(_mutex);
  std::set<Snowflake> out;
  int64_t now = nowMs();
  for (auto &[ssrc, s] : _streams) {
    if (now - s.last_heard < 300) out.insert(s.user_id);
  }
  return out;
}

MicEncoder::MicEncoder() : _scaled(kFrameValues) {
  int err = 0;
  _encoder = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);
  if (err != OPUS_OK) {
    log::error("audio: opus_encoder_create failed (%d)", err);
    _encoder = nullptr;
    return;
  }
  opus_encoder_ctl(_encoder, OPUS_SET_BITRATE(64000));
  opus_encoder_ctl(_encoder, OPUS_SET_INBAND_FEC(1));
  opus_encoder_ctl(_encoder, OPUS_SET_PACKET_LOSS_PERC(5));
  // The Switch CPU is shared with the game: keep the encoder cheap.
  opus_encoder_ctl(_encoder, OPUS_SET_COMPLEXITY(3));
}

MicEncoder::~MicEncoder() {
  if (_encoder) opus_encoder_destroy(_encoder);
}

int MicEncoder::encode(const int16_t *pcm, uint8_t *out, size_t out_cap) {
  if (!_encoder) return -1;
  double sum = 0;
  for (int i = 0; i < kFrameValues; i++) {
    int v = static_cast<int>(pcm[i] * _gain);
    v = std::clamp(v, -32768, 32767);
    _scaled[i] = static_cast<int16_t>(v);
    sum += double(v) * v;
  }
  _last_level = static_cast<int>(std::sqrt(sum / kFrameValues));
  if (_threshold > 0) {
    if (_last_level >= _threshold) {
      _hangover = kHangoverFrames;
    } else if (_hangover > 0) {
      _hangover--;
    } else {
      return 0;
    }
  }
  int n = opus_encode(_encoder, _scaled.data(), kFrameSamples, out, static_cast<opus_int32>(out_cap));
  return n < 0 ? -1 : n;
}

}  // namespace nxcord
