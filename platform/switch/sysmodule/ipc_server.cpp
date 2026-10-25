#include "ipc_server.hpp"

#include <cstring>

namespace nxcord {

namespace {
constexpr u32 kCmifInMagic = 0x49434653;   // "SFCI"
constexpr u32 kCmifOutMagic = 0x4F434653;  // "SFCO"
constexpr u32 kResultUnknownCommand = MAKERESULT(Module_Libnx, LibnxError_NotFound);
constexpr u32 kResultBadRequest = MAKERESULT(Module_Libnx, LibnxError_BadInput);

// Writes a CMIF reply into the thread's IPC buffer.
void writeReply(const IpcServer::Reply &reply) {
  void *base = armGetTls();
  HipcRequest hipc = hipcMakeRequest(
      base, HipcMetadata{
                .type = 0,
                .num_send_statics = 0,
                .num_send_buffers = 0,
                .num_recv_buffers = 0,
                .num_exch_buffers = 0,
                .num_data_words = static_cast<u32>((0x10 + sizeof(CmifOutHeader) + reply.size + 3) / 4),
                .num_recv_statics = 0,
                .send_pid = 0,
                .num_copy_handles = 0,
                .num_move_handles = 0,
            });
  auto *out = static_cast<CmifOutHeader *>(cmifGetAlignedDataStart(hipc.data_words, base));
  out->magic = kCmifOutMagic;
  out->version = 0;
  out->result = reply.result;
  out->token = 0;
  if (reply.size) std::memcpy(out + 1, reply.data, reply.size);
}

void writeCloseReply() {
  void *base = armGetTls();
  hipcMakeRequest(base, HipcMetadata{});
}
}  // namespace

Result IpcServer::start(const char *name, int max_sessions, Handler handler) {
  _handler = std::move(handler);
  _name = smEncodeName(name);
  return smRegisterService(&_port, _name, false, max_sessions);
}

void IpcServer::stop() {
  for (Handle h : _sessions) svcCloseHandle(h);
  _sessions.clear();
  if (_port != INVALID_HANDLE) {
    svcCloseHandle(_port);
    smUnregisterService(_name);
    _port = INVALID_HANDLE;
  }
}

void IpcServer::closeSession(size_t index) {
  if (_sessions[index] == _reply_to) _reply_to = INVALID_HANDLE;
  svcCloseHandle(_sessions[index]);
  _sessions.erase(_sessions.begin() + index);
}

bool IpcServer::process(u64 timeout_ns) {
  std::vector<Handle> handles;
  handles.push_back(_port);
  handles.insert(handles.end(), _sessions.begin(), _sessions.end());

  s32 index = -1;
  Result rc = svcReplyAndReceive(&index, handles.data(), static_cast<s32>(handles.size()), _reply_to, timeout_ns);
  Handle replied = _reply_to;
  _reply_to = INVALID_HANDLE;

  if (R_FAILED(rc)) {
    if (rc == KERNELRESULT(ConnectionClosed) || rc == KERNELRESULT(NotFound)) {
      // The session we replied to (or the one that signalled) is gone.
      for (size_t i = 0; i < _sessions.size(); i++) {
        Handle h = index > 0 ? handles[index] : replied;
        if (_sessions[i] == h) {
          closeSession(i);
          break;
        }
      }
      return true;
    }
    if (rc == KERNELRESULT(TimedOut) || rc == KERNELRESULT(Cancelled)) return true;
    return false;
  }

  if (index == 0) {
    Handle session;
    if (R_SUCCEEDED(svcAcceptSession(&session, _port))) _sessions.push_back(session);
    return true;
  }

  size_t session_index = static_cast<size_t>(index - 1);
  Handle session = _sessions[session_index];
  void *base = armGetTls();
  HipcParsedRequest req = hipcParseRequest(base);

  if (req.meta.type == CmifCommandType_Close) {
    writeCloseReply();
    closeSession(session_index);
    return true;
  }

  Reply reply;
  if (req.meta.type != CmifCommandType_Request && req.meta.type != CmifCommandType_RequestWithContext) {
    reply.result = kResultBadRequest;
  } else {
    auto *in = static_cast<const CmifInHeader *>(cmifGetAlignedDataStart(req.data.data_words, base));
    size_t data_bytes = req.meta.num_data_words * 4;
    size_t header_offset = reinterpret_cast<const u8 *>(in) - reinterpret_cast<const u8 *>(req.data.data_words);
    if (in->magic != kCmifInMagic || data_bytes < header_offset + sizeof(CmifInHeader)) {
      reply.result = kResultBadRequest;
    } else {
      Request r{};
      r.command = in->command_id;
      r.in = reinterpret_cast<const u8 *>(in + 1);
      r.in_size = data_bytes - header_offset - sizeof(CmifInHeader);
      if (req.meta.num_recv_buffers > 0) {
        r.out_buffer = hipcGetBufferAddress(&req.data.recv_buffers[0]);
        r.out_buffer_size = hipcGetBufferSize(&req.data.recv_buffers[0]);
      }
      reply.result = kResultUnknownCommand;
      _handler(r, reply);
    }
  }
  writeReply(reply);
  _reply_to = session;
  return true;
}

}  // namespace nxcord
