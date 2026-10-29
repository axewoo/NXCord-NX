#include <nxcord/util.hpp>

#include <chrono>
#include <thread>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/random.h>
#endif

namespace nxcord {

void randomBytes(void *out, size_t len) {
#ifdef __SWITCH__
  randomGet(out, len);
#else
  auto *p = static_cast<uint8_t *>(out);
  while (len > 0) {
    ssize_t n = getrandom(p, len, 0);
    if (n <= 0) continue;
    p += n;
    len -= static_cast<size_t>(n);
  }
#endif
}

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t unixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace nxcord
