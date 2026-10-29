#pragma once

#include <switch.h>

#include <cstddef>
#include <functional>
#include <vector>

namespace nxcord {

// Minimal single-threaded CMIF server on top of svcReplyAndReceive.
// Supports raw in/out data and HipcMapAlias (type A/B) buffers, which is all
// the overlay needs. No domains, no pointer buffers.
class IpcServer {
 public:
  struct Request {
    u32 command;
    const u8 *in;
    size_t in_size;
    void *out_buffer;  // first type-B buffer, or null
    size_t out_buffer_size;
  };
  struct Reply {
    Result result = 0;
    u8 data[0xB0];  // the whole IPC message must fit in 0x100 bytes
    size_t size = 0;

    template <typename T>
    void set(const T &v) {
      static_assert(sizeof(T) <= sizeof(data));
      __builtin_memcpy(data, &v, sizeof(T));
      size = sizeof(T);
    }
  };
  using Handler = std::function<void(const Request &, Reply &)>;

  Result start(const char *name, int max_sessions, Handler handler);
  // Handles at most one request, waiting up to timeout_ns. Returns false on fatal error.
  bool process(u64 timeout_ns);
  void stop();

 private:
  void closeSession(size_t index);

  Handle _port = INVALID_HANDLE;
  SmServiceName _name{};
  std::vector<Handle> _sessions;
  Handler _handler;
  Handle _reply_to = INVALID_HANDLE;
};

}  // namespace nxcord
