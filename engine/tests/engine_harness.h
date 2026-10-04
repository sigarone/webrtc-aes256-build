// Shared test harness of the engine drivers (call_test.cpp, audio_bench.cpp): one engine process
// with its pipe, the SDP helpers and the check counter. Header only; every includer gets its own copy.
#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/frame.h"
#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/message.h"
#include "qmedia/ipc/pipe.h"
#include "qmedia/ipc/schema.h"
#include "qmedia/ipc/secure.h"

using namespace qmedia::ipc;
using cbor::Buf;
using cbor::Value;
using Clock = std::chrono::steady_clock;


namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
  std::printf("[call] %-72s %s\n", what.c_str(), ok ? "ok" : "FAILED");
  std::fflush(stdout);
  if (!ok) ++g_failures;
}

class EngineProc;
std::vector<EngineProc*>& Procs() {
  static std::vector<EngineProc*> v;
  return v;
}
void DumpAllStderr();

[[noreturn]] void Abort(const std::string& why) {
  std::printf("[call] FAILED (stopping): %s\n", why.c_str());
  DumpAllStderr();
  std::fflush(stdout);
  std::_Exit(1);
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// ---- A decoded engine message ------------------------------------------------------------------

struct Parsed {
  Buf raw;
  Value root;
  ValidatedMessage msg;
  std::string kind() const { return std::string(msg.spec->kind); }
  uint32_t id() const { return msg.id; }
  uint64_t Uint(const char* name) const { return FieldUint(msg, name); }
  std::string Text(const char* name) const { return std::string(FieldText(msg, name)); }
  const Value* F(const char* name) const { return Field(msg, name); }
  Buf Bytes(const char* name) const {
    const Value* v = F(name);
    return v ? Buf(v->raw.begin(), v->raw.end()) : Buf();
  }
};
using Msg = std::shared_ptr<Parsed>;

// ---- One engine process and its pipe -----------------------------------------------------------

class EngineProc {
 public:
  ~EngineProc() {
    Procs().erase(std::remove(Procs().begin(), Procs().end(), this), Procs().end());
    stop_ = true;
    if (stream_) stream_->Cancel();
    if (reader_.joinable()) reader_.join();
    if (process_ != nullptr) {
      TerminateProcess(process_, 99);
      CloseHandle(process_);
    }
    if (stdin_write_ != nullptr) CloseHandle(stdin_write_);
  }

  // `exe` is the engine executable and `extra_args` the options after --pipe/--expect-client-pid
  // (each starting with a space), so the drivers can start the CI, the hardware bench or any other build.
  bool Start(const std::string& tag, const std::wstring& exe, const std::wstring& extra_args,
             const std::string& err_file) {
    tag_ = tag;
    err_file_ = err_file;
    Procs().push_back(this);
    pipe_name_ = L"\\\\.\\pipe\\qmedia-call-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()) + L"-" + Widen(tag);
    Buf nonce(kNonceBytes);
    if (!FillRandom(nonce.data(), nonce.size())) return false;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE in_r = nullptr;
    if (!CreatePipe(&in_r, &stdin_write_, &sa, 0)) return false;
    SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE err = CreateFileW(Widen(err_file).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = nul;
    si.hStdError = err != INVALID_HANDLE_VALUE ? err : nul;
    std::wstring cmd = L"\"" + exe + L"\" --pipe " + pipe_name_ + L" --expect-client-pid " +
                       std::to_wstring(GetCurrentProcessId()) + extra_args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    CloseHandle(in_r);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;

    DWORD put = 0;
    if (!WriteFile(stdin_write_, nonce.data(), static_cast<DWORD>(nonce.size()), &put, nullptr) ||
        put != nonce.size()) {
      return false;
    }
    if (ConnectPipe(pipe_name_, 20000, GetProcessId(process_), &stream_) != Err::Ok) return false;
    if (WriteFrame(*stream_, BuildHello(1, nonce), 5000) != Err::Ok) return false;
    FrameBuffer fb;
    if (ReadFrame(*stream_, fb, 10000) != Err::Ok) return false;
    Value v;
    ValidatedMessage m;
    if (cbor::Decode(fb.view(), &v) != Err::Ok || ValidateMessage(v, Dir::EngineToClient, &m) != Err::Ok ||
        m.spec->kind != "hello_ok") {
      return false;
    }
    SecureZero(nonce.data(), nonce.size());
    reader_ = std::thread([this] { ReadLoop(); });
    return true;
  }

  // Sends one request and waits for its reply (the message with the same id).
  Msg Call(const std::string& what, const std::function<Buf(uint32_t)>& build, int timeout_s = 30) {
    const uint32_t id = next_id_++;
    if (WriteFrame(*stream_, build(id), 5000) != Err::Ok) Abort(tag_ + ": write failed for " + what);
    std::unique_lock<std::mutex> l(mu_);
    Msg found;
    const bool got = cv_.wait_for(l, std::chrono::seconds(timeout_s), [&] {
      for (const Msg& m : replies_) {
        if (m->id() == id) {
          found = m;
          return true;
        }
      }
      return dead_;
    });
    if (!got || !found) Abort(tag_ + ": no reply for " + what + (dead_ ? " (engine connection ended)" : ""));
    return found;
  }

  // Replies that must be "ok".
  void CallOk(const std::string& what, const std::function<Buf(uint32_t)>& build) {
    Msg r = Call(what, build);
    if (r->kind() != "ok") {
      Abort(tag_ + ": " + what + " answered " + r->kind() + " " + r->Text("code") + " " + r->Text("detail"));
    }
  }

  // Events: all of them are kept, in arrival order.
  std::vector<Msg> Events() {
    std::lock_guard<std::mutex> l(mu_);
    return events_;
  }
  Msg FindEvent(const std::function<bool(const Parsed&)>& pred) {
    std::lock_guard<std::mutex> l(mu_);
    for (const Msg& e : events_) {
      if (pred(*e)) return e;
    }
    return nullptr;
  }
  Msg WaitEvent(const std::function<bool(const Parsed&)>& pred, int timeout_s) {
    std::unique_lock<std::mutex> l(mu_);
    Msg found;
    cv_.wait_for(l, std::chrono::seconds(timeout_s), [&] {
      for (const Msg& e : events_) {
        if (pred(*e)) {
          found = e;
          return true;
        }
      }
      return dead_;
    });
    return found;
  }
  size_t EventCount() {
    std::lock_guard<std::mutex> l(mu_);
    return events_.size();
  }
  Msg EventAt(size_t i) {
    std::lock_guard<std::mutex> l(mu_);
    return i < events_.size() ? events_[i] : nullptr;
  }

  bool invalid_message() const { return invalid_; }

  int WaitExit(DWORD timeout_ms) {
    if (WaitForSingleObject(process_, timeout_ms) != WAIT_OBJECT_0) return -1;
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    return static_cast<int>(code);
  }

  void DumpStderr() {
    std::FILE* f = std::fopen(err_file_.c_str(), "rb");
    if (!f) return;
    std::vector<char> data(1 << 16);
    const size_t n = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (n == 0) return;
    std::printf("[call] --- %s engine stderr (first %zu bytes) ---\n", tag_.c_str(), n);
    std::fwrite(data.data(), 1, n, stdout);
    std::printf("\n[call] ---\n");
  }

  const std::string& tag() const { return tag_; }

 private:
  void ReadLoop() {
    for (;;) {
      FrameBuffer fb;
      const Err e = ReadFrame(*stream_, fb, kNoTimeout);
      if (e != Err::Ok) break;
      auto p = std::make_shared<Parsed>();
      p->raw.assign(fb.view().begin(), fb.view().end());
      if (cbor::Decode(p->raw, &p->root) != Err::Ok ||
          ValidateMessage(p->root, Dir::EngineToClient, &p->msg) != Err::Ok) {
        invalid_ = true;
        break;
      }
      std::lock_guard<std::mutex> l(mu_);
      if (p->id() == 0) {
        events_.push_back(p);
      } else {
        replies_.push_back(p);
      }
      cv_.notify_all();
    }
    std::lock_guard<std::mutex> l(mu_);
    dead_ = true;
    cv_.notify_all();
  }

  std::string tag_;
  std::string err_file_;
  std::wstring pipe_name_;
  HANDLE process_ = nullptr;
  HANDLE stdin_write_ = nullptr;
  std::unique_ptr<HandleStream> stream_;
  std::thread reader_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> invalid_{false};
  std::atomic<uint32_t> next_id_{2};
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<Msg> events_;
  std::vector<Msg> replies_;
  bool dead_ = false;
};

void DumpAllStderr() {
  for (EngineProc* p : Procs()) p->DumpStderr();
}

// ---- Request builders (client to engine) -------------------------------------------------------

Buf Req(const char* kind, uint32_t id) { return BeginMessage(kind, id).Finish(); }

// ---- SDP helpers (the host's job in a real call; here only what the test needs) ----------------

std::string MungeOpus(const std::string& sdp) {
  // The mobile apps pin Opus to 60 ms packets, 32 kbps, CBR, in-band FEC (AudioSdpPolicy).
  std::string pt;
  size_t pos = sdp.find("a=rtpmap:");
  while (pos != std::string::npos) {
    const size_t eol = sdp.find("\r\n", pos);
    const std::string line = sdp.substr(pos, eol - pos);
    if (line.find(" opus/48000") != std::string::npos) {
      pt = line.substr(9, line.find(' ') - 9);
      break;
    }
    pos = sdp.find("a=rtpmap:", pos + 1);
  }
  if (pt.empty()) return sdp;
  std::string out;
  size_t i = 0;
  const std::string fmtp = "a=fmtp:" + pt + " ";
  while (i < sdp.size()) {
    size_t eol = sdp.find("\r\n", i);
    if (eol == std::string::npos) eol = sdp.size();
    std::string line = sdp.substr(i, eol - i);
    if (line.rfind(fmtp, 0) == 0) {
      for (const char* kv : {"cbr=1", "useinbandfec=1", "maxaveragebitrate=32000", "minptime=60"}) {
        const std::string key = std::string(kv).substr(0, std::string(kv).find('='));
        // Replace an existing value or append the parameter.
        const size_t at = line.find(key + "=");
        if (at != std::string::npos) {
          size_t end = line.find(';', at);
          if (end == std::string::npos) end = line.size();
          line.replace(at, end - at, kv);
        } else {
          line += std::string(";") + kv;
        }
      }
    }
    out += line + "\r\n";
    i = eol + 2;
  }
  out += "a=ptime:60\r\na=maxptime:60\r\n";
  return out;
}

std::string MidOf(const std::string& sdp) {
  const size_t p = sdp.find("a=mid:");
  if (p == std::string::npos) return "";
  const size_t e = sdp.find("\r\n", p);
  return sdp.substr(p + 6, e - p - 6);
}

// The DTLS fingerprint an SDP announces: "a=fingerprint:sha-256 AA:BB:..." as 32 raw bytes.
Buf FingerprintOfSdp(const std::string& sdp) {
  Buf out;
  const std::string key = "a=fingerprint:sha-256 ";
  const size_t p = sdp.find(key);
  if (p == std::string::npos) return out;
  size_t i = p + key.size();
  while (i + 1 < sdp.size() && sdp[i] != '\r' && sdp[i] != '\n') {
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    const int hi = hex(sdp[i]);
    const int lo = hex(sdp[i + 1]);
    if (hi < 0 || lo < 0) break;
    out.push_back(static_cast<uint8_t>(hi * 16 + lo));
    i += 2;
    if (i < sdp.size() && sdp[i] == ':') ++i;
  }
  return out;
}

}  // namespace
