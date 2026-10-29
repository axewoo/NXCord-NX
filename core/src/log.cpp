#include <nxcord/log.hpp>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace nxcord::log {

namespace {
std::mutex g_mutex;
Sink g_sink = nullptr;
bool g_verbose = false;

void defaultSink(const char *line) { std::fputs(line, stderr); }

void vwrite(const char *level, const char *fmt, va_list args) {
  char buf[1024];
  int n = std::snprintf(buf, sizeof(buf), "[%s] ", level);
  std::vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, args);
  size_t len = std::strlen(buf);
  if (len == 0 || buf[len - 1] != '\n') {
    buf[len] = '\n';
    buf[len + 1] = '\0';
  }
  std::scoped_lock lock(g_mutex);
  (g_sink ? g_sink : defaultSink)(buf);
}
}  // namespace

void setSink(Sink sink) { g_sink = sink; }
void setVerbose(bool v) { g_verbose = v; }
bool verbose() { return g_verbose; }

void info(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vwrite("I", fmt, args);
  va_end(args);
}

void debug(const char *fmt, ...) {
  if (!g_verbose) return;
  va_list args;
  va_start(args, fmt);
  vwrite("D", fmt, args);
  va_end(args);
}

void error(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vwrite("E", fmt, args);
  va_end(args);
}

}  // namespace nxcord::log
