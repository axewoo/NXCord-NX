#pragma once

#include <cstdarg>

namespace nxcord {

// Minimal thread-safe logger. The platform layer decides where lines go
// (a file on the SD card on Switch, stderr on Linux).
namespace log {

using Sink = void (*)(const char *line);

void setSink(Sink sink);
void setVerbose(bool verbose);
bool verbose();

void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace log
}  // namespace nxcord
