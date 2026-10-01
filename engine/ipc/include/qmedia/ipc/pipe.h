// Windows named-pipe transport. Windows only.
//
// Server: one instance (FILE_FLAG_FIRST_PIPE_INSTANCE, max instances 1), remote clients rejected,
// DACL with a single allow-ACE for the current user SID (no Everyone, no Network, no Anonymous),
// verified again after creation. One client is accepted; after that no further instance exists.
// Client helper: opens the pipe with identification-level impersonation only and can verify the
// server process id.
#pragma once

#if !defined(_WIN32)
#error "pipe.h is Windows only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "qmedia/ipc/frame.h"
#include "qmedia/ipc/status.h"

namespace qmedia::ipc {

// Flags the server pipe is created with (exposed so a test can pin them down).
inline constexpr DWORD kPipeOpenMode =
    PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED;
inline constexpr DWORD kPipeMode =
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
inline constexpr DWORD kPipeMaxInstances = 1;

// Accepts "\\.\pipe\" followed by 1..128 characters of [A-Za-z0-9._-].
bool IsValidPipeName(std::wstring_view name);

// Overlapped-I/O stream over a pipe (or any overlapped file handle). Owns the handle.
// One reader thread and several writer threads may use it concurrently.
class HandleStream final : public ByteStream {
 public:
  explicit HandleStream(HANDLE h);
  ~HandleStream() override;
  HandleStream(const HandleStream&) = delete;
  HandleStream& operator=(const HandleStream&) = delete;

  IoResult ReadExact(uint8_t* dst, size_t n, uint32_t timeout_ms) override;
  IoResult WriteAll(const uint8_t* src, size_t n, uint32_t timeout_ms) override;

  // Cancels pending I/O so a blocked reader returns. The handle stays valid until destruction.
  void Cancel();
  HANDLE handle() const { return handle_; }

 private:
  HANDLE handle_;
  HANDLE read_event_;
  HANDLE write_event_;
  std::mutex read_mu_;
  std::mutex write_mu_;
};

class PipeServer {
 public:
  // Creates the single pipe instance. Fails if any instance of that name already exists.
  static Err Create(std::wstring_view name, std::unique_ptr<PipeServer>* out);
  ~PipeServer();
  PipeServer(const PipeServer&) = delete;
  PipeServer& operator=(const PipeServer&) = delete;

  // Waits for the one client. expected_client_pid 0 = do not check. On success the pipe handle
  // moves into the returned stream and the server object no longer holds it.
  Err WaitForClient(uint32_t timeout_ms, uint32_t expected_client_pid,
                    std::unique_ptr<HandleStream>* stream);

  HANDLE handle() const { return handle_; }

 private:
  explicit PipeServer(HANDLE h) : handle_(h) {}
  HANDLE handle_;
};

// Connects to a pipe created by PipeServer. expected_server_pid 0 = do not check.
Err ConnectPipe(std::wstring_view name, uint32_t timeout_ms, uint32_t expected_server_pid,
                std::unique_ptr<HandleStream>* out);

// True iff the DACL of the pipe handle is exactly one ACCESS_ALLOWED ACE for the current user.
bool VerifyPipeDacl(HANDLE pipe);

// Reads exactly kNonceBytes from a handle (typically stdin) within the timeout.
Err ReadNonceFromHandle(HANDLE h, uint8_t out[32], uint32_t timeout_ms);

}  // namespace qmedia::ipc
