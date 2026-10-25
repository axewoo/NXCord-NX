#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nxcord {

void randomBytes(void *out, size_t len);
// Monotonic milliseconds.
int64_t nowMs();
// Wall-clock milliseconds since the Unix epoch.
int64_t unixMs();
void sleepMs(int ms);

}  // namespace nxcord
