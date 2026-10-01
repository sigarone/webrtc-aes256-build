// qaudion-media: generic media engine process (Windows).
//
// Usage: qaudion-media.exe --pipe \\.\pipe\<name> [--expect-client-pid <pid>]
// The parent writes exactly 32 random bytes (the session nonce) to this process' stdin, then the
// client connects to the pipe and sends "hello" with that nonce as its first message.
//
// Exit codes (the only diagnostics this process emits; it never logs):
//   0 clean shutdown or peer closed   2 bad command line      3 nonce missing, short or all zero
//   4 pipe creation failed            5 no client in time     6 client pid mismatch
//   7 handshake failed (bad nonce)    8 protocol violation    9 I/O error
//
// Linking the published libwebrtc is a later step: see cmake/webrtc.cmake. Until then every
// engine command other than ping and shutdown is answered with err "unsupported".

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "qmedia/ipc/frame.h"
#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/pipe.h"
#include "qmedia/ipc/secure.h"
#include "qmedia/ipc/session.h"

namespace {

using namespace qmedia::ipc;

bool ParseUint32(const char* s, uint32_t* out) {
  if (s == nullptr || *s == '\0') return false;
  uint64_t v = 0;
  for (; *s != '\0'; ++s) {
    if (*s < '0' || *s > '9') return false;
    v = v * 10 + static_cast<uint64_t>(*s - '0');
    if (v > 0xFFFFFFFFull) return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

bool IsAllZero(const uint8_t* p, size_t n) {
  uint8_t acc = 0;
  for (size_t i = 0; i < n; ++i) acc = static_cast<uint8_t>(acc | p[i]);
  return acc == 0;
}

// Process-wide hardening that does not depend on the command line.
void HardenProcess() {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
  // DLLs come from the executable's directory and System32 only (no current directory, no PATH).
  SetDllDirectoryW(L"");
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

}  // namespace

int main(int argc, char** argv) {
  HardenProcess();
  std::wstring pipe_name;
  uint32_t expect_pid = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--pipe") == 0 && i + 1 < argc) {
      const char* a = argv[++i];
      for (; *a != '\0'; ++a) {
        if (static_cast<unsigned char>(*a) >= 0x80) return 2;  // names are plain ASCII
        pipe_name.push_back(static_cast<wchar_t>(*a));
      }
    } else if (std::strcmp(argv[i], "--expect-client-pid") == 0 && i + 1 < argc) {
      if (!ParseUint32(argv[++i], &expect_pid)) return 2;
    } else {
      return 2;
    }
  }
  if (!IsValidPipeName(pipe_name)) return 2;

  uint8_t nonce[kNonceBytes];
  if (ReadNonceFromHandle(GetStdHandle(STD_INPUT_HANDLE), nonce, kNonceReadTimeoutMs) != Err::Ok ||
      IsAllZero(nonce, sizeof nonce)) {
    // Missing, short, or all zero (a host that sent an uninitialised buffer): no session.
    SecureZero(nonce, sizeof nonce);
    return 3;
  }

  int code = 0;
  std::unique_ptr<PipeServer> server;
  std::unique_ptr<HandleStream> stream;
  if (PipeServer::Create(pipe_name, &server) != Err::Ok) {
    code = 4;
  } else {
    const Err w = server->WaitForClient(kConnectTimeoutMs, expect_pid, &stream);
    if (w == Err::PipeClientPid) {
      code = 6;
    } else if (w != Err::Ok) {
      code = 5;
    }
  }

  if (code == 0) {
    FrameBuffer buf;
    StubHandler handler;
    switch (Serve(*stream, buf, std::span<const uint8_t>(nonce, kNonceBytes), kHelloTimeoutMs,
                  handler)) {
      case ServeResult::Shutdown:
      case ServeResult::PeerClosed: code = 0; break;
      case ServeResult::HandshakeFailed: code = 7; break;
      case ServeResult::ProtocolViolation: code = 8; break;
      case ServeResult::IoError: code = 9; break;
    }
  }
  SecureZero(nonce, sizeof nonce);
  return code;
}
