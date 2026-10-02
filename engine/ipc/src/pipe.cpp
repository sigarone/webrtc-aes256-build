#include "qmedia/ipc/pipe.h"

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <vector>

#include "qmedia/ipc/limits.h"

namespace qmedia::ipc {

namespace {

constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\";
constexpr size_t kPipePrefixLen = 9;
constexpr DWORD kPipeBuffer = 64 * 1024;

struct LocalFreeGuard {
  void* p;
  ~LocalFreeGuard() {
    if (p != nullptr) LocalFree(p);
  }
};

// Token user of the current process.
class UserSid {
 public:
  bool Load() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD need = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &need);
    if (need == 0) {
      CloseHandle(token);
      return false;
    }
    buf_.assign(need, 0);
    const BOOL ok = GetTokenInformation(token, TokenUser, buf_.data(), need, &need);
    CloseHandle(token);
    return ok != FALSE;
  }
  PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(buf_.data())->User.Sid; }
  bool ToString(std::wstring* out) const {
    LPWSTR s = nullptr;
    if (!ConvertSidToStringSidW(sid(), &s)) return false;
    LocalFreeGuard g{s};
    *out = s;
    return true;
  }

 private:
  std::vector<BYTE> buf_;
};

uint64_t NowMs() { return GetTickCount64(); }

// Waits for a started overlapped operation. Returns Ok with *got set, or Eof/Timeout/Error.
IoResult Finish(HANDLE h, OVERLAPPED& ov, BOOL started, uint64_t deadline_ms, bool infinite,
                DWORD* got) {
  *got = 0;
  if (!started) {
    const DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_HANDLE_EOF) {
      return IoResult::Eof;
    }
    if (err != ERROR_IO_PENDING) return IoResult::Error;
    DWORD wait_ms = INFINITE;
    if (!infinite) {
      const uint64_t now = NowMs();
      wait_ms = now >= deadline_ms ? 0 : static_cast<DWORD>(std::min<uint64_t>(deadline_ms - now, 0xFFFFFFFEull));
    }
    const DWORD w = WaitForSingleObject(ov.hEvent, wait_ms);
    if (w == WAIT_TIMEOUT) {
      CancelIoEx(h, &ov);
      DWORD dummy = 0;
      GetOverlappedResult(h, &ov, &dummy, TRUE);
      return IoResult::Timeout;
    }
    if (w != WAIT_OBJECT_0) {
      CancelIoEx(h, &ov);
      DWORD dummy = 0;
      GetOverlappedResult(h, &ov, &dummy, TRUE);
      return IoResult::Error;
    }
  }
  if (!GetOverlappedResult(h, &ov, got, FALSE)) {
    const DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_HANDLE_EOF) {
      return IoResult::Eof;
    }
    return IoResult::Error;
  }
  return IoResult::Ok;
}

bool UsersDaclMatches(HANDLE pipe, PSID expected) {
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                      nullptr, &sd) != ERROR_SUCCESS) {
    return false;
  }
  LocalFreeGuard g{sd};
  if (dacl == nullptr) return false;  // NULL DACL grants everyone full access
  ACL_SIZE_INFORMATION info{};
  if (!GetAclInformation(dacl, &info, sizeof info, AclSizeInformation)) return false;
  if (info.AceCount != 1) return false;
  void* ace = nullptr;
  if (!GetAce(dacl, 0, &ace) || ace == nullptr) return false;
  const ACE_HEADER* hdr = static_cast<const ACE_HEADER*>(ace);
  if (hdr->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
  const ACCESS_ALLOWED_ACE* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(ace);
  return EqualSid(reinterpret_cast<PSID>(const_cast<DWORD*>(&allowed->SidStart)), expected) != FALSE;
}

}  // namespace

bool IsValidPipeName(std::wstring_view name) {
  if (name.size() <= kPipePrefixLen || name.size() > kPipePrefixLen + 128) return false;
  if (name.substr(0, kPipePrefixLen) != std::wstring_view(kPipePrefix, kPipePrefixLen)) return false;
  for (wchar_t c : name.substr(kPipePrefixLen)) {
    const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                    (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-';
    if (!ok) return false;
  }
  return true;
}

// ---- HandleStream -------------------------------------------------------------------------

HandleStream::HandleStream(HANDLE h)
    : handle_(h),
      read_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      write_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

HandleStream::~HandleStream() {
  if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  if (read_event_ != nullptr) CloseHandle(read_event_);
  if (write_event_ != nullptr) CloseHandle(write_event_);
}

void HandleStream::Cancel() {
  if (handle_ != INVALID_HANDLE_VALUE) CancelIoEx(handle_, nullptr);
}

IoResult HandleStream::ReadExact(uint8_t* dst, size_t n, uint32_t timeout_ms) {
  if (read_event_ == nullptr || handle_ == INVALID_HANDLE_VALUE) return IoResult::Error;
  std::lock_guard<std::mutex> lock(read_mu_);
  const bool infinite = timeout_ms == kNoTimeout;
  const uint64_t deadline = NowMs() + (infinite ? 0 : timeout_ms);
  size_t done = 0;
  while (done < n) {
    OVERLAPPED ov{};
    ov.hEvent = read_event_;
    ResetEvent(read_event_);
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(n - done, 1u << 20));
    const BOOL started = ReadFile(handle_, dst + done, chunk, nullptr, &ov);
    DWORD got = 0;
    const IoResult r = Finish(handle_, ov, started, deadline, infinite, &got);
    if (r == IoResult::Eof) return done == 0 ? IoResult::Eof : IoResult::Error;
    if (r != IoResult::Ok) return r;
    if (got == 0) return IoResult::Error;
    done += got;
  }
  return IoResult::Ok;
}

IoResult HandleStream::WriteAll(const uint8_t* src, size_t n, uint32_t timeout_ms) {
  if (write_event_ == nullptr || handle_ == INVALID_HANDLE_VALUE) return IoResult::Error;
  std::lock_guard<std::mutex> lock(write_mu_);
  const bool infinite = timeout_ms == kNoTimeout;
  const uint64_t deadline = NowMs() + (infinite ? 0 : timeout_ms);
  size_t done = 0;
  while (done < n) {
    OVERLAPPED ov{};
    ov.hEvent = write_event_;
    ResetEvent(write_event_);
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(n - done, 1u << 20));
    const BOOL started = WriteFile(handle_, src + done, chunk, nullptr, &ov);
    DWORD put = 0;
    const IoResult r = Finish(handle_, ov, started, deadline, infinite, &put);
    if (r == IoResult::Eof) return IoResult::Error;
    if (r != IoResult::Ok) return r;
    if (put == 0) return IoResult::Error;
    done += put;
  }
  return IoResult::Ok;
}

// ---- PipeServer ---------------------------------------------------------------------------

Err PipeServer::Create(std::wstring_view name, std::unique_ptr<PipeServer>* out) {
  if (!IsValidPipeName(name)) return Err::PipeBadName;

  UserSid user;
  std::wstring sid_text;
  if (!user.Load() || !user.ToString(&sid_text)) return Err::PipeSecurity;

  // Protected DACL, one ACE: the current user may read and write. Nobody else.
  const std::wstring sddl = L"D:P(A;;FRFW;;;" + sid_text + L")";
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                            nullptr)) {
    return Err::PipeSecurity;
  }
  LocalFreeGuard sd_guard{sd};
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  sa.lpSecurityDescriptor = sd;
  sa.bInheritHandle = FALSE;

  const std::wstring name_z(name);
  // A single instance: a second client gets ERROR_PIPE_BUSY, and nobody can add an instance.
  const HANDLE h = CreateNamedPipeW(name_z.c_str(), kPipeOpenMode, kPipeMode, kPipeMaxInstances,
                                    kPipeBuffer, kPipeBuffer, 0, &sa);
  if (h == INVALID_HANDLE_VALUE) {
    return GetLastError() == ERROR_ACCESS_DENIED ? Err::PipeAccessDenied : Err::PipeCreate;
  }
  if (!UsersDaclMatches(h, user.sid())) {
    CloseHandle(h);
    return Err::PipeSecurity;
  }
  out->reset(new PipeServer(h));
  return Err::Ok;
}

PipeServer::~PipeServer() {
  if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
}

Err PipeServer::WaitForClient(uint32_t timeout_ms, uint32_t expected_client_pid,
                              std::unique_ptr<HandleStream>* stream) {
  if (handle_ == INVALID_HANDLE_VALUE) return Err::PipeCreate;
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (ev == nullptr) return Err::IoError;
  OVERLAPPED ov{};
  ov.hEvent = ev;
  Err result = Err::Ok;

  const BOOL connected = ConnectNamedPipe(handle_, &ov);
  if (!connected) {
    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
      // The client connected between creation and this call: success.
    } else if (err == ERROR_IO_PENDING) {
      const DWORD w = WaitForSingleObject(ev, timeout_ms == kNoTimeout ? INFINITE : timeout_ms);
      if (w != WAIT_OBJECT_0) {
        CancelIoEx(handle_, &ov);
        DWORD dummy = 0;
        GetOverlappedResult(handle_, &ov, &dummy, TRUE);
        result = w == WAIT_TIMEOUT ? Err::IoTimeout : Err::IoError;
      }
    } else {
      result = Err::IoError;
    }
  }
  CloseHandle(ev);
  if (result != Err::Ok) return result;

  if (expected_client_pid != 0) {
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(handle_, &pid) || pid != expected_client_pid) {
      DisconnectNamedPipe(handle_);
      return Err::PipeClientPid;
    }
  }
  stream->reset(new HandleStream(handle_));
  handle_ = INVALID_HANDLE_VALUE;
  return Err::Ok;
}

// ---- Client -------------------------------------------------------------------------------

Err ConnectPipe(std::wstring_view name, uint32_t timeout_ms, uint32_t expected_server_pid,
                std::unique_ptr<HandleStream>* out) {
  if (!IsValidPipeName(name)) return Err::PipeBadName;
  const std::wstring name_z(name);
  const uint64_t deadline = NowMs() + (timeout_ms == kNoTimeout ? 0 : timeout_ms);
  const bool infinite = timeout_ms == kNoTimeout;
  Err last = Err::PipeNotFound;
  for (;;) {
    const HANDLE h = CreateFileW(name_z.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                 nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      if (expected_server_pid != 0) {
        ULONG pid = 0;
        if (!GetNamedPipeServerProcessId(h, &pid) || pid != expected_server_pid) {
          CloseHandle(h);
          return Err::PipeServerPid;
        }
      }
      out->reset(new HandleStream(h));
      return Err::Ok;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED) return Err::PipeAccessDenied;
    if (err == ERROR_PIPE_BUSY) {
      last = Err::PipeBusy;
    } else if (err == ERROR_FILE_NOT_FOUND) {
      last = Err::PipeNotFound;
    } else {
      return Err::IoError;
    }
    if (!infinite && NowMs() >= deadline) return last;
    if (err == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(name_z.c_str(), 100);
    } else {
      Sleep(10);
    }
  }
}

bool VerifyPipeDacl(HANDLE pipe) {
  UserSid user;
  return user.Load() && UsersDaclMatches(pipe, user.sid());
}

Err ReadNonceFromHandle(HANDLE h, uint8_t out[32], uint32_t timeout_ms) {
  if (h == nullptr || h == INVALID_HANDLE_VALUE) return Err::PipeNonceRead;
  const DWORD type = GetFileType(h);
  if (type == FILE_TYPE_DISK) {
    // Redirected from a file: the data is already there, a read cannot block.
    DWORD got = 0;
    if (!ReadFile(h, out, static_cast<DWORD>(kNonceBytes), &got, nullptr) || got != kNonceBytes) {
      return Err::PipeNonceRead;
    }
    return Err::Ok;
  }
  // Consoles, NUL and sockets are not a way to hand over a nonce (a console read would block
  // without any timeout). Only a pipe is accepted.
  if (type != FILE_TYPE_PIPE) return Err::PipeNonceRead;

  const uint64_t deadline = NowMs() + timeout_ms;
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
      return Err::PipeNonceRead;  // broken pipe: the parent closed without sending 32 bytes
    }
    if (avail >= kNonceBytes) break;
    if (NowMs() >= deadline) return Err::PipeNonceRead;
    Sleep(5);
  }
  DWORD got = 0;
  if (!ReadFile(h, out, static_cast<DWORD>(kNonceBytes), &got, nullptr) || got != kNonceBytes) {
    return Err::PipeNonceRead;
  }
  return Err::Ok;
}

}  // namespace qmedia::ipc
