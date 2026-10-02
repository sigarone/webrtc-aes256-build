// Windows named-pipe transport tests: per-user ACL, single instance, single client, nonce check,
// oversized frame rejection, and (when the engine executable is built) the real process.
#include <aclapi.h>
#include <sddl.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "qmedia/ipc/frame.h"
#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/message.h"
#include "qmedia/ipc/pipe.h"
#include "qmedia/ipc/secure.h"
#include "qmedia/ipc/session.h"
#include "testing.h"

using namespace qmedia::ipc;
using cbor::Buf;
using cbor::Value;

namespace {

std::atomic<int> g_counter{0};

std::wstring UniqueName() {
  return L"\\\\.\\pipe\\qmedia-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(g_counter.fetch_add(1));
}

Buf MakeNonce(uint8_t seed) {
  Buf n(kNonceBytes);
  for (size_t i = 0; i < n.size(); ++i) n[i] = static_cast<uint8_t>(seed * 7 + i);
  return n;
}

struct Reply {
  Err err = Err::IoError;
  std::string kind;
  std::string detail;
  uint32_t id = 0;
};

Reply Recv(ByteStream& s, uint32_t timeout_ms = 5000) {
  Reply r;
  FrameBuffer buf;
  r.err = ReadFrame(s, buf, timeout_ms);
  if (r.err != Err::Ok) return r;
  Value v;
  r.err = cbor::Decode(buf.view(), &v);
  if (r.err != Err::Ok) return r;
  ValidatedMessage m;
  r.err = ValidateMessage(v, Dir::EngineToClient, &m);
  if (r.err != Err::Ok) return r;
  r.kind = std::string(m.spec->kind);
  r.detail = std::string(FieldText(m, "detail"));
  r.id = m.id;
  return r;
}

Err Send(ByteStream& s, const Buf& payload) { return WriteFrame(s, payload, 5000); }

Buf Simple(std::string_view kind, uint32_t id) { return BeginMessage(kind, id).Finish(); }

// Runs Serve on a pipe in a background thread (the "engine" side of an in-process test).
class InProcServer {
 public:
  ~InProcServer() { Join(); }

  bool Start(const std::wstring& name, const Buf& nonce, uint32_t hello_timeout_ms = 3000) {
    if (PipeServer::Create(name, &server_) != Err::Ok) return false;
    thread_ = std::thread([this, nonce, hello_timeout_ms] {
      std::unique_ptr<HandleStream> stream;
      wait_err = server_->WaitForClient(8000, 0, &stream);
      if (wait_err != Err::Ok) return;
      FrameBuffer buf;
      StubHandler handler;
      result = Serve(*stream, buf, nonce, hello_timeout_ms, handler);
      // The stream is destroyed here, which closes the pipe.
    });
    return true;
  }
  void Join() {
    if (thread_.joinable()) thread_.join();
  }
  PipeServer& server() { return *server_; }

  Err wait_err = Err::Ok;
  ServeResult result = ServeResult::IoError;

 private:
  std::unique_ptr<PipeServer> server_;
  std::thread thread_;
};

// SDDL text of the DACL of a pipe handle.
std::wstring DaclText(HANDLE h) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (GetSecurityInfo(h, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr,
                      &sd) != ERROR_SUCCESS) {
    return {};
  }
  LPWSTR text = nullptr;
  std::wstring out;
  if (ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION,
                                                           &text, nullptr)) {
    out = text;
    LocalFree(text);
  }
  LocalFree(sd);
  return out;
}

size_t CountOf(const std::wstring& s, wchar_t c) {
  size_t n = 0;
  for (wchar_t x : s) n += x == c ? 1 : 0;
  return n;
}

bool Contains(const std::wstring& s, const wchar_t* sub) { return s.find(sub) != std::wstring::npos; }

// The SID of the (single) ACE of an SDDL text, which may be written as a full SID or as an alias.
bool AceSidMatchesCurrentUser(const std::wstring& dacl) {
  const size_t semi = dacl.rfind(L';');
  const size_t close = dacl.rfind(L')');
  if (semi == std::wstring::npos || close == std::wstring::npos || close <= semi) return false;
  const std::wstring token = dacl.substr(semi + 1, close - semi - 1);
  PSID ace_sid = nullptr;
  if (!ConvertStringSidToSidW(token.c_str(), &ace_sid)) return false;
  HANDLE t = nullptr;
  bool equal = false;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
    DWORD need = 0;
    GetTokenInformation(t, TokenUser, nullptr, 0, &need);
    std::vector<BYTE> buf(need);
    if (GetTokenInformation(t, TokenUser, buf.data(), need, &need)) {
      equal = EqualSid(ace_sid, reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid) != FALSE;
    }
    CloseHandle(t);
  }
  LocalFree(ace_sid);
  return equal;
}

void ExpectOnlyCurrentUser(const std::wstring& dacl) {
  std::fprintf(stderr, "  dacl: %ls\n", dacl.c_str());
  CHECK(dacl.rfind(L"D:P", 0) == 0);        // protected: no inherited entries
  CHECK_EQ(CountOf(dacl, L'('), 1u);        // exactly one ACE
  CHECK(Contains(dacl, L"(A;"));            // an allow entry
  CHECK(AceSidMatchesCurrentUser(dacl));    // for the current user and nobody else
  CHECK(!Contains(dacl, L"S-1-1-0"));       // Everyone
  CHECK(!Contains(dacl, L";WD)"));          // Everyone (alias)
  CHECK(!Contains(dacl, L";NU)"));          // Network
  CHECK(!Contains(dacl, L";AN)"));          // Anonymous
  CHECK(!Contains(dacl, L";BU)"));          // Users group
  CHECK(!Contains(dacl, L";AU)"));          // Authenticated users
  CHECK(!Contains(dacl, L";BA)"));          // Administrators
  CHECK(!Contains(dacl, L";SY)"));          // Local system
}

}  // namespace

QTEST(pipe_name_validation) {
  CHECK(IsValidPipeName(L"\\\\.\\pipe\\qmedia-1.a_b"));
  CHECK(IsValidPipeName(L"\\\\.\\pipe\\x"));
  CHECK(!IsValidPipeName(L""));
  CHECK(!IsValidPipeName(L"\\\\.\\pipe\\"));
  CHECK(!IsValidPipeName(L"pipe"));
  CHECK(!IsValidPipeName(L"\\\\server\\pipe\\x"));          // remote host
  CHECK(!IsValidPipeName(L"\\\\.\\pipe\\a\\b"));            // sub path
  CHECK(!IsValidPipeName(L"\\\\.\\pipe\\a b"));             // space
  CHECK(!IsValidPipeName(L"\\\\.\\pipe\\..\\x"));           // traversal characters
  CHECK(!IsValidPipeName(L"\\\\.\\PIPE\\x"));               // exact prefix only
  CHECK(!IsValidPipeName(L"\\\\.\\pipe\\" + std::wstring(129, L'a')));
  CHECK(IsValidPipeName(L"\\\\.\\pipe\\" + std::wstring(128, L'a')));
  std::unique_ptr<PipeServer> s;
  CHECK_EQ(PipeServer::Create(L"\\\\.\\pipe\\bad name", &s), Err::PipeBadName);
  std::unique_ptr<HandleStream> c;
  CHECK_EQ(ConnectPipe(L"\\\\.\\pipe\\bad name", 100, 0, &c), Err::PipeBadName);
}

QTEST(pipe_flags_are_the_hardened_ones) {
  // Compile-time checks: a change to the flags must be a conscious edit of this test.
  static_assert((kPipeOpenMode & FILE_FLAG_FIRST_PIPE_INSTANCE) != 0);
  static_assert((kPipeOpenMode & FILE_FLAG_OVERLAPPED) != 0);
  static_assert((kPipeMode & PIPE_REJECT_REMOTE_CLIENTS) != 0);
  static_assert((kPipeMode & PIPE_TYPE_MESSAGE) == 0);  // byte stream, our own framing
  static_assert(kPipeMaxInstances == 1);
}

QTEST(pipe_acl_is_current_user_only) {
  const std::wstring name = UniqueName();
  std::unique_ptr<PipeServer> server;
  CHECK_EQ(PipeServer::Create(name, &server), Err::Ok);
  if (!server) return;
  CHECK(VerifyPipeDacl(server->handle()));
  const std::wstring dacl = DaclText(server->handle());
  if (dacl.empty()) std::fprintf(stderr, "  could not read the DACL\n");
  CHECK(!dacl.empty());
  ExpectOnlyCurrentUser(dacl);
}

namespace {

std::wstring CurrentUserSidText() {
  std::wstring out;
  HANDLE t = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return out;
  DWORD need = 0;
  GetTokenInformation(t, TokenUser, nullptr, 0, &need);
  std::vector<BYTE> buf(need);
  if (GetTokenInformation(t, TokenUser, buf.data(), need, &need)) {
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) {
      out = text;
      LocalFree(text);
    }
  }
  CloseHandle(t);
  return out;
}

// Creates a pipe the way a buggy or hostile setup could: with the given DACL text, or with the
// process default DACL (null), or with a NULL DACL (everyone gets everything).
enum class TestDacl { Default, NullDacl, Sddl };

HANDLE CreatePipeWithDacl(TestDacl kind, const std::wstring& sddl) {
  const std::wstring name = UniqueName();
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  SECURITY_DESCRIPTOR null_dacl_sd{};
  PSECURITY_DESCRIPTOR sd = nullptr;
  SECURITY_ATTRIBUTES* attrs = nullptr;
  if (kind == TestDacl::NullDacl) {
    if (!InitializeSecurityDescriptor(&null_dacl_sd, SECURITY_DESCRIPTOR_REVISION)) return INVALID_HANDLE_VALUE;
    if (!SetSecurityDescriptorDacl(&null_dacl_sd, TRUE, nullptr, FALSE)) return INVALID_HANDLE_VALUE;
    sa.lpSecurityDescriptor = &null_dacl_sd;
    attrs = &sa;
  } else if (kind == TestDacl::Sddl) {
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
      return INVALID_HANDLE_VALUE;
    }
    sa.lpSecurityDescriptor = sd;
    attrs = &sa;
  }
  const HANDLE h = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, attrs);
  if (sd != nullptr) LocalFree(sd);
  return h;
}

}  // namespace

QTEST(pipe_dacl_verifier_accepts_only_exactly_one_allow_entry_for_the_user) {
  const std::wstring me = CurrentUserSidText();
  CHECK(!me.empty());

  // Positive control: exactly what the server creates.
  HANDLE good = CreatePipeWithDacl(TestDacl::Sddl, L"D:P(A;;FRFW;;;" + me + L")");
  CHECK(good != INVALID_HANDLE_VALUE);
  if (good != INVALID_HANDLE_VALUE) {
    CHECK(VerifyPipeDacl(good));
    CloseHandle(good);
  }

  struct Bad {
    const char* what;
    TestDacl kind;
    std::wstring sddl;
  };
  const Bad bad[] = {
      {"process default DACL", TestDacl::Default, L""},
      {"NULL DACL", TestDacl::NullDacl, L""},
      {"Everyone only", TestDacl::Sddl, L"D:P(A;;FRFW;;;WD)"},
      {"System only", TestDacl::Sddl, L"D:P(A;;FRFW;;;SY)"},
      {"user plus Everyone", TestDacl::Sddl, L"D:P(A;;FRFW;;;" + me + L")(A;;FRFW;;;WD)"},
      {"user plus Network", TestDacl::Sddl, L"D:P(A;;FRFW;;;" + me + L")(A;;FRFW;;;NU)"},
      {"deny entry for the user", TestDacl::Sddl, L"D:P(D;;FRFW;;;" + me + L")"},
      {"empty DACL", TestDacl::Sddl, L"D:P"},
  };
  for (const Bad& b : bad) {
    HANDLE h = CreatePipeWithDacl(b.kind, b.sddl);
    if (h == INVALID_HANDLE_VALUE) {
      std::fprintf(stderr, "  could not create the test pipe for: %s\n", b.what);
      CHECK(false);
      continue;
    }
    if (VerifyPipeDacl(h)) std::fprintf(stderr, "  accepted a bad DACL: %s\n", b.what);
    CHECK(!VerifyPipeDacl(h));
    CloseHandle(h);
  }
}

QTEST(pipe_second_server_with_the_same_name_is_refused) {
  const std::wstring name = UniqueName();
  std::unique_ptr<PipeServer> a, b;
  CHECK_EQ(PipeServer::Create(name, &a), Err::Ok);
  const Err e = PipeServer::Create(name, &b);
  CHECK(e == Err::PipeAccessDenied || e == Err::PipeCreate);
  CHECK(!b);
}

QTEST(pipe_serves_exactly_one_client) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(1);
  InProcServer srv;
  CHECK(srv.Start(name, nonce));

  std::unique_ptr<HandleStream> a;
  CHECK_EQ(ConnectPipe(name, 5000, 0, &a), Err::Ok);
  std::unique_ptr<HandleStream> b;
  CHECK_EQ(ConnectPipe(name, 400, 0, &b), Err::PipeBusy);  // the only instance is taken
  CHECK(!b);

  // The first client keeps working.
  CHECK_EQ(Send(*a, BuildHello(1, nonce)), Err::Ok);
  CHECK(Recv(*a).kind == "hello_ok");
  CHECK_EQ(Send(*a, Simple("shutdown", 2)), Err::Ok);
  CHECK(Recv(*a).kind == "ok");
  srv.Join();
  CHECK(srv.result == ServeResult::Shutdown);
}

QTEST(pipe_client_can_check_the_server_process) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(2);
  InProcServer srv;
  CHECK(srv.Start(name, nonce));
  std::unique_ptr<HandleStream> wrong;
  CHECK_EQ(ConnectPipe(name, 3000, GetCurrentProcessId() + 4, &wrong), Err::PipeServerPid);
  CHECK(!wrong);
  // The refused connection consumed the single instance; the server thread sees EOF and ends.
  srv.Join();
}

QTEST(pipe_end_to_end_ping_and_shutdown) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(3);
  InProcServer srv;
  CHECK(srv.Start(name, nonce));
  std::unique_ptr<HandleStream> c;
  CHECK_EQ(ConnectPipe(name, 5000, GetCurrentProcessId(), &c), Err::Ok);
  CHECK(c != nullptr);
  if (!c) return;
  CHECK(VerifyPipeDacl(c->handle()));  // the client sees the same DACL on its end

  CHECK_EQ(Send(*c, BuildHello(10, nonce)), Err::Ok);
  Reply r = Recv(*c);
  CHECK(r.kind == "hello_ok" && r.id == 10);
  CHECK_EQ(Send(*c, Simple("ping", 11)), Err::Ok);
  r = Recv(*c);
  CHECK(r.kind == "pong" && r.id == 11);
  CHECK_EQ(Send(*c, Simple("session_create", 12)), Err::Ok);
  r = Recv(*c);
  CHECK(r.kind == "err" && r.id == 12 && r.detail == "engine_core_not_linked");
  CHECK_EQ(Send(*c, Simple("shutdown", 13)), Err::Ok);
  r = Recv(*c);
  CHECK(r.kind == "ok" && r.id == 13);
  srv.Join();
  CHECK(srv.result == ServeResult::Shutdown);
}

QTEST(pipe_wrong_nonce_sends_nothing_and_closes) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(4);
  Buf wrong = nonce;
  wrong[5] ^= 0x10;
  InProcServer srv;
  CHECK(srv.Start(name, nonce));
  std::unique_ptr<HandleStream> c;
  CHECK_EQ(ConnectPipe(name, 5000, 0, &c), Err::Ok);
  if (!c) return;
  CHECK_EQ(Send(*c, BuildHello(1, wrong)), Err::Ok);
  const Reply r = Recv(*c, 5000);
  CHECK(r.err == Err::IoEof);  // the connection ends without a single byte
  srv.Join();
  CHECK(srv.result == ServeResult::HandshakeFailed);
}

QTEST(pipe_first_message_not_hello_is_refused) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(5);
  InProcServer srv;
  CHECK(srv.Start(name, nonce));
  std::unique_ptr<HandleStream> c;
  CHECK_EQ(ConnectPipe(name, 5000, 0, &c), Err::Ok);
  if (!c) return;
  CHECK_EQ(Send(*c, Simple("ping", 1)), Err::Ok);
  CHECK(Recv(*c).err == Err::IoEof);
  srv.Join();
  CHECK(srv.result == ServeResult::HandshakeFailed);
}

QTEST(pipe_oversized_frame_is_rejected) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(6);
  InProcServer srv;
  CHECK(srv.Start(name, nonce));
  std::unique_ptr<HandleStream> c;
  CHECK_EQ(ConnectPipe(name, 5000, 0, &c), Err::Ok);
  if (!c) return;
  CHECK_EQ(Send(*c, BuildHello(1, nonce)), Err::Ok);
  CHECK(Recv(*c).kind == "hello_ok");
  // A header announcing 1 MiB + 1 bytes, no payload at all: refused from the header alone.
  const uint8_t header[4] = {0x00, 0x10, 0x00, 0x01};
  CHECK(c->WriteAll(header, sizeof header, 5000) == IoResult::Ok);
  const Reply r = Recv(*c);
  CHECK(r.kind == "err");
  CHECK(r.detail == "frame_too_large");
  CHECK(Recv(*c).err == Err::IoEof);
  srv.Join();
  CHECK(srv.result == ServeResult::ProtocolViolation);
}

QTEST(pipe_oversized_frame_before_hello_is_silent) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(7);
  InProcServer srv;
  CHECK(srv.Start(name, nonce));
  std::unique_ptr<HandleStream> c;
  CHECK_EQ(ConnectPipe(name, 5000, 0, &c), Err::Ok);
  if (!c) return;
  const uint8_t header[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(c->WriteAll(header, sizeof header, 5000) == IoResult::Ok);
  CHECK(Recv(*c).err == Err::IoEof);
  srv.Join();
  CHECK(srv.result == ServeResult::HandshakeFailed);
}

QTEST(pipe_silent_client_hits_the_hello_timeout) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(8);
  InProcServer srv;
  CHECK(srv.Start(name, nonce, 300));
  std::unique_ptr<HandleStream> c;
  CHECK_EQ(ConnectPipe(name, 5000, 0, &c), Err::Ok);
  if (!c) return;
  CHECK(Recv(*c, 5000).err == Err::IoEof);  // the engine gave up and closed
  srv.Join();
  CHECK(srv.result == ServeResult::HandshakeFailed);
}

// ---- Nonce handover ------------------------------------------------------------------------

QTEST(nonce_reader_accepts_32_bytes_from_a_pipe) {
  HANDLE r = nullptr, w = nullptr;
  CHECK(CreatePipe(&r, &w, nullptr, 0));
  const Buf nonce = MakeNonce(9);
  DWORD put = 0;
  CHECK(WriteFile(w, nonce.data(), static_cast<DWORD>(nonce.size()), &put, nullptr));
  uint8_t got[32] = {0};
  CHECK_EQ(ReadNonceFromHandle(r, got, 1000), Err::Ok);
  CHECK(std::memcmp(got, nonce.data(), 32) == 0);
  CloseHandle(r);
  CloseHandle(w);
}

QTEST(nonce_reader_rejects_short_closed_and_non_pipe_inputs) {
  uint8_t got[32] = {0};
  {  // 10 bytes, writer still open: timeout
    HANDLE r = nullptr, w = nullptr;
    CHECK(CreatePipe(&r, &w, nullptr, 0));
    const uint8_t part[10] = {1};
    DWORD put = 0;
    CHECK(WriteFile(w, part, sizeof part, &put, nullptr));
    CHECK_EQ(ReadNonceFromHandle(r, got, 200), Err::PipeNonceRead);
    CloseHandle(r);
    CloseHandle(w);
  }
  {  // writer closed with nothing written
    HANDLE r = nullptr, w = nullptr;
    CHECK(CreatePipe(&r, &w, nullptr, 0));
    CloseHandle(w);
    CHECK_EQ(ReadNonceFromHandle(r, got, 1000), Err::PipeNonceRead);
    CloseHandle(r);
  }
  {  // writer closed after 10 bytes
    HANDLE r = nullptr, w = nullptr;
    CHECK(CreatePipe(&r, &w, nullptr, 0));
    const uint8_t part[10] = {1};
    DWORD put = 0;
    CHECK(WriteFile(w, part, sizeof part, &put, nullptr));
    CloseHandle(w);
    CHECK_EQ(ReadNonceFromHandle(r, got, 1000), Err::PipeNonceRead);
    CloseHandle(r);
  }
  {  // NUL device: not a pipe
    const HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(nul != INVALID_HANDLE_VALUE);
    CHECK_EQ(ReadNonceFromHandle(nul, got, 200), Err::PipeNonceRead);
    CloseHandle(nul);
  }
  CHECK_EQ(ReadNonceFromHandle(INVALID_HANDLE_VALUE, got, 100), Err::PipeNonceRead);
  CHECK_EQ(ReadNonceFromHandle(nullptr, got, 100), Err::PipeNonceRead);
}

// ---- The real engine process ---------------------------------------------------------------

#ifdef QMEDIA_ENGINE_EXE

#define QM_WIDEN2(x) L##x
#define QM_WIDEN(x) QM_WIDEN2(x)

namespace {

class Engine {
 public:
  ~Engine() {
    if (process_ != nullptr) {
      TerminateProcess(process_, 99);
      CloseHandle(process_);
    }
    if (stdin_write_ != nullptr) CloseHandle(stdin_write_);
  }

  // Starts qaudion-media with the given extra arguments. Returns false if it could not start.
  bool Start(const std::wstring& args) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE in_r = nullptr;
    if (!CreatePipe(&in_r, &stdin_write_, &sa, 0)) return false;
    SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);  // the child must not hold our end
    const HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = nul;
    si.hStdError = nul;
    std::wstring cmd = L"\"" + std::wstring(QM_WIDEN(QMEDIA_ENGINE_EXE)) + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    CloseHandle(in_r);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    return true;
  }

  bool SendNonce(const Buf& nonce) {
    DWORD put = 0;
    return WriteFile(stdin_write_, nonce.data(), static_cast<DWORD>(nonce.size()), &put, nullptr) &&
           put == nonce.size();
  }
  void CloseStdin() {
    if (stdin_write_ != nullptr) CloseHandle(stdin_write_);
    stdin_write_ = nullptr;
  }
  // Waits for the process to exit and returns its exit code, or -1 on timeout.
  int WaitExit(DWORD timeout_ms) {
    if (WaitForSingleObject(process_, timeout_ms) != WAIT_OBJECT_0) return -1;
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    return static_cast<int>(code);
  }
  DWORD pid() const { return GetProcessId(process_); }

 private:
  HANDLE process_ = nullptr;
  HANDLE stdin_write_ = nullptr;
};

std::wstring PipeArg(const std::wstring& name) { return L"--pipe " + name; }

}  // namespace

QTEST(engine_process_full_session) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(20);
  Engine e;
  CHECK(e.Start(PipeArg(name) + L" --expect-client-pid " + std::to_wstring(GetCurrentProcessId())));
  CHECK(e.SendNonce(nonce));
  std::unique_ptr<HandleStream> c;
  CHECK_EQ(ConnectPipe(name, 10000, e.pid(), &c), Err::Ok);
  if (!c) return;
  CHECK(VerifyPipeDacl(c->handle()));
  ExpectOnlyCurrentUser(DaclText(c->handle()));
  CHECK_EQ(Send(*c, BuildHello(1, nonce)), Err::Ok);
  CHECK(Recv(*c).kind == "hello_ok");
  CHECK_EQ(Send(*c, Simple("ping", 2)), Err::Ok);
  CHECK(Recv(*c).kind == "pong");
  CHECK_EQ(Send(*c, Simple("shutdown", 3)), Err::Ok);
  CHECK(Recv(*c).kind == "ok");
  CHECK_EQ(e.WaitExit(10000), 0);
}

QTEST(engine_process_exits_on_wrong_nonce) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(21);
  Buf wrong = nonce;
  wrong[0] ^= 1;
  Engine e;
  CHECK(e.Start(PipeArg(name)));
  CHECK(e.SendNonce(nonce));
  std::unique_ptr<HandleStream> c;
  CHECK_EQ(ConnectPipe(name, 10000, 0, &c), Err::Ok);
  if (!c) return;
  CHECK_EQ(Send(*c, BuildHello(1, wrong)), Err::Ok);
  CHECK(Recv(*c).err == Err::IoEof);
  CHECK_EQ(e.WaitExit(10000), 7);
}

QTEST(engine_process_exits_when_the_first_message_is_not_hello) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(22);
  Engine e;
  CHECK(e.Start(PipeArg(name)));
  CHECK(e.SendNonce(nonce));
  std::unique_ptr<HandleStream> c;
  CHECK_EQ(ConnectPipe(name, 10000, 0, &c), Err::Ok);
  if (!c) return;
  CHECK_EQ(Send(*c, Simple("shutdown", 1)), Err::Ok);
  CHECK(Recv(*c).err == Err::IoEof);
  CHECK_EQ(e.WaitExit(10000), 7);
}

QTEST(engine_process_exits_when_no_nonce_arrives) {
  const std::wstring name = UniqueName();
  Engine e;
  CHECK(e.Start(PipeArg(name)));
  e.CloseStdin();  // the parent goes away without handing over a nonce
  CHECK_EQ(e.WaitExit(10000), 3);
}

QTEST(engine_process_exits_on_an_all_zero_nonce) {
  const std::wstring name = UniqueName();
  Engine e;
  CHECK(e.Start(PipeArg(name)));
  CHECK(e.SendNonce(Buf(32, 0)));  // a host that forgot to fill the buffer
  CHECK_EQ(e.WaitExit(10000), 3);
}

QTEST(engine_process_exits_on_a_short_nonce) {
  const std::wstring name = UniqueName();
  Engine e;
  CHECK(e.Start(PipeArg(name)));
  CHECK(e.SendNonce(Buf(31, 1)));  // one byte short
  e.CloseStdin();
  CHECK_EQ(e.WaitExit(10000), 3);
}

QTEST(engine_process_rejects_a_bad_command_line) {
  {
    Engine e;
    CHECK(e.Start(L""));  // no --pipe
    CHECK_EQ(e.WaitExit(10000), 2);
  }
  {
    Engine e;
    CHECK(e.Start(L"--pipe \\\\server\\pipe\\x"));  // remote name
    CHECK_EQ(e.WaitExit(10000), 2);
  }
  {
    Engine e;
    CHECK(e.Start(PipeArg(UniqueName()) + L" --unknown"));
    CHECK_EQ(e.WaitExit(10000), 2);
  }
}

QTEST(engine_process_refuses_an_unexpected_client_process) {
  const std::wstring name = UniqueName();
  const Buf nonce = MakeNonce(23);
  Engine e;
  CHECK(e.Start(PipeArg(name) + L" --expect-client-pid " + std::to_wstring(GetCurrentProcessId() + 4)));
  CHECK(e.SendNonce(nonce));
  std::unique_ptr<HandleStream> c;
  CHECK_EQ(ConnectPipe(name, 10000, 0, &c), Err::Ok);
  if (c) {
    // Nothing is answered, even to a correct hello.
    (void)Send(*c, BuildHello(1, nonce));
    CHECK(Recv(*c).err != Err::Ok);
  }
  CHECK_EQ(e.WaitExit(10000), 6);
}

#endif  // QMEDIA_ENGINE_EXE
