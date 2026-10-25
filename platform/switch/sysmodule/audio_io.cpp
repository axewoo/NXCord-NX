#include "audio_io.hpp"

#include <nxcord/log.hpp>

#include <malloc.h>

#include <cstring>

namespace nxcord {

namespace {
constexpr int kBuffers = 3;
constexpr size_t kFrameBytes = kFrameValues * sizeof(int16_t);
constexpr size_t kBufferSize = (kFrameBytes + 0xfff) & ~size_t(0xfff);
}  // namespace

void AudioIo::start() {
  _running = true;
  // Audio threads sit just above the network threads so playback doesn't stutter.
  // The mic thread runs the Opus encoder, which needs a big stack.
  if (R_SUCCEEDED(threadCreate(&_speaker_thread, speakerEntry, this, nullptr, 0x8000, 0x2B, -2)))
    threadStart(&_speaker_thread);
  if (R_SUCCEEDED(threadCreate(&_mic_thread, micEntry, this, nullptr, 0x20000, 0x2B, -2)))
    threadStart(&_mic_thread);
}

void AudioIo::stop() {
  _running = false;
  threadWaitForExit(&_speaker_thread);
  threadClose(&_speaker_thread);
  threadWaitForExit(&_mic_thread);
  threadClose(&_mic_thread);
}

void AudioIo::speakerLoop() {
  Result rc = audoutInitialize();
  if (R_FAILED(rc)) {
    log::error("audout init failed: 0x%x", rc);
    return;
  }
  if (audoutGetSampleRate() != kSampleRate || audoutGetChannelCount() != kChannels) {
    log::error("audout format %u Hz x%u not supported", audoutGetSampleRate(), audoutGetChannelCount());
    audoutExit();
    return;
  }
  AudioOutBuffer bufs[kBuffers] = {};
  for (auto &b : bufs) {
    b.buffer = memalign(0x1000, kBufferSize);
    std::memset(b.buffer, 0, kBufferSize);
    b.buffer_size = kBufferSize;
    b.data_size = kFrameBytes;
    audoutAppendAudioOutBuffer(&b);
  }
  audoutStartAudioOut();
  _speaker_ok = true;
  log::info("speaker ready");

  while (_running) {
    AudioOutBuffer *released = nullptr;
    u32 count = 0;
    if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100'000'000)) || !released) continue;
    _client.mixSpeaker(static_cast<int16_t *>(released->buffer));
    armDCacheFlush(released->buffer, kFrameBytes);
    audoutAppendAudioOutBuffer(released);
  }

  audoutStopAudioOut();
  bool flushed = false;
  audoutFlushAudioOutBuffers(&flushed);
  audoutExit();
  for (auto &b : bufs) free(b.buffer);
  _speaker_ok = false;
}

void AudioIo::micLoop() {
  // A mic only exists while a USB headset is plugged in, so keep retrying.
  while (_running) {
    Result rc = audinInitialize();
    if (R_FAILED(rc)) {
      for (int i = 0; i < 50 && _running; i++) svcSleepThread(100'000'000);
      continue;
    }
    u32 rate = audinGetSampleRate(), channels = audinGetChannelCount();
    if (rate != kSampleRate || (channels != 1 && channels != 2)) {
      log::error("audin format %u Hz x%u not supported", rate, channels);
      audinExit();
      return;
    }
    size_t in_bytes = kFrameSamples * channels * sizeof(int16_t);
    AudioInBuffer bufs[kBuffers] = {};
    for (auto &b : bufs) {
      b.buffer = memalign(0x1000, kBufferSize);
      std::memset(b.buffer, 0, kBufferSize);
      b.buffer_size = kBufferSize;
      b.data_size = in_bytes;
      audinAppendAudioInBuffer(&b);
    }
    if (R_FAILED(audinStartAudioIn())) {
      audinExit();
      for (auto &b : bufs) free(b.buffer);
      svcSleepThread(5'000'000'000);
      continue;
    }
    _mic_ok = true;
    log::info("microphone ready (%u ch)", channels);

    int16_t stereo[kFrameValues];
    int failures = 0;
    while (_running && failures < 20) {
      AudioInBuffer *released = nullptr;
      u32 count = 0;
      if (R_FAILED(audinWaitCaptureFinish(&released, &count, 100'000'000)) || !released) {
        failures++;  // headset unplugged: no more buffers come back
        continue;
      }
      failures = 0;
      armDCacheFlush(released->buffer, in_bytes);
      auto *pcm = static_cast<const int16_t *>(released->buffer);
      if (channels == 1) {
        for (int i = 0; i < kFrameSamples; i++) stereo[2 * i] = stereo[2 * i + 1] = pcm[i];
        _client.feedMicrophone(stereo);
      } else {
        _client.feedMicrophone(pcm);
      }
      audinAppendAudioInBuffer(released);
    }
    _mic_ok = false;
    audinStopAudioIn();
    audinExit();
    for (auto &b : bufs) free(b.buffer);
    if (_running) log::info("microphone lost, waiting for a headset");
  }
}

}  // namespace nxcord
