#include <nxcord/audio.hpp>
#include <nxcord/log.hpp>
#include <nxcord/util.hpp>

#include <opus/opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nxcord {

namespace {
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

void AudioMixer::enqueue(Stream &s, const int16_t *frame) {
  if (s.count == kMaxBuffered) {  // full: drop the oldest frame
    s.head = (s.head + 1) % kMaxBuffered;
    s.count--;
  }
  std::memcpy(s.ring[(s.head + s.count) % kMaxBuffered].data(), frame, sizeof(int16_t) * kFrameValues);
  s.count++;
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
      if (opus_decode(s.decoder, nullptr, 0, _decoded.data(), kFrameSamples, 0) == kFrameSamples)
        enqueue(s, _decoded.data());
    }
  }
  s.have_seq = true;
  s.last_seq = seq;

  int n = opus_decode(s.decoder, opus, static_cast<opus_int32>(len), _decoded.data(), kMaxPacketSamples, 0);
  if (n <= 0) return;
  // Discord sends 20 ms packets; split anything longer into 20 ms frames.
  for (int off = 0; off < n; off += kFrameSamples) {
    int take = std::min(kFrameSamples, n - off);
    int16_t *frame = _decoded.data() + off * kChannels;
    if (take < kFrameSamples) std::fill(frame + take * kChannels, frame + kFrameValues, 0);
    enqueue(s, frame);
  }
  // Discord's silence frames decode to silence; they still count as frames.
  if (len > 3) s.last_heard = nowMs();
}

bool AudioMixer::mix(int16_t *out) {
  bool any = false;
  std::scoped_lock lock(_mutex);
  _acc.fill(0);
  for (auto &[ssrc, s] : _streams) {
    if (!s.started) {
      if (s.count < kStartFrames) continue;
      s.started = true;
    }
    if (s.count == 0) {
      s.started = false;  // underrun: rebuffer
      continue;
    }
    const auto &frame = s.ring[s.head];
    s.head = (s.head + 1) % kMaxBuffered;
    s.count--;
    float vol = _master;
    if (auto it = _volumes.find(s.user_id); it != _volumes.end()) vol *= it->second;
    for (int i = 0; i < kFrameValues; i++) _acc[i] += static_cast<int32_t>(frame[i] * vol);
    any = true;
  }
  for (int i = 0; i < kFrameValues; i++) out[i] = static_cast<int16_t>(std::clamp(_acc[i], -32768, 32767));
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
