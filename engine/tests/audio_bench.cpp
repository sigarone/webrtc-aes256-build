// Real-audio bench driver (hwlab): two engine processes call each other over loopback, the first
// captures from a real microphone and the second plays to real speakers, for N seconds. It writes a
// JSON report (levels, concealment, latency estimate, transport verdict).
//
//   qmedia_audio_bench --list
//       Lists the capture and render devices of the Windows audio device module (names, default
//       flags, endpoint format). Starts one hardware engine and does NOT open a microphone or speakers.
//   qmedia_audio_bench --selfcheck
//       Device listing plus a session/certificate round trip on the hardware engine. No stream is
//       opened. Nothing is recorded or played.
//   qmedia_audio_bench --dry --seconds 8 --out <json> --work <dir>
//       The whole call with the CI engine and file devices instead of the sound card: proves the
//       bench logic, the strict transport checks and the report without touching the hardware.
//   qmedia_audio_bench --real --capture <substr> --render <substr> --seconds N --out <json> --work <dir>
//       The real test. Opens the microphone and the speakers. Empty substrings select the system
//       defaults. Run it only when someone is there to speak.
//
// The offer is munged to sendonly, so the capturing engine opens no speakers and the playing engine
// opens no microphone. Frame encryption keys, strict DTLS/SRTP and the fingerprint checks are the
// ones of qmedia_call_test. The output carries verdicts and levels only: no SDP, fingerprints,
// addresses or keys. Device names are written as the system reports them; the PowerShell wrapper
// scrubs personal names before the report is stored.
#include "engine_harness.h"

#include <mmreg.h>
#include <mmdeviceapi.h>
#include <propidl.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>

#define QM_W2(x) L##x
#define QM_W(x) QM_W2(x)

namespace {

// ---- Small helpers -----------------------------------------------------------------------------

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool ContainsCi(const std::string& hay, const std::string& needle) {
  return Lower(hay).find(Lower(needle)) != std::string::npos;
}

std::string Narrow(const wchar_t* w) {
  if (w == nullptr) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

double ToDb(double rms) { return rms > 1e-9 ? 20.0 * std::log10(rms) : -120.0; }

// A tiny JSON writer. Strings are escaped; control characters become \u00XX.
class JW {
 public:
  void Obj(const char* k = nullptr) { Open(k, '{', '}'); }
  void Arr(const char* k = nullptr) { Open(k, '[', ']'); }
  void End() {
    o_ += close_.back();
    close_.pop_back();
    need_ = true;
  }
  void S(const char* k, const std::string& v) { Key(k); Str(v); need_ = true; }
  void N(const char* k, double v) {
    Key(k);
    if (std::isfinite(v)) {
      char b[48];
      std::snprintf(b, sizeof b, "%.6g", v);
      o_ += b;
    } else {
      o_ += "null";
    }
    need_ = true;
  }
  void B(const char* k, bool v) { Key(k); o_ += v ? "true" : "false"; need_ = true; }
  void Null(const char* k) { Key(k); o_ += "null"; need_ = true; }
  const std::string& str() const { return o_; }

 private:
  void Open(const char* k, char c, char e) { Key(k); o_ += c; close_.push_back(e); need_ = false; }
  void Key(const char* k) {
    if (need_) o_ += ',';
    if (k != nullptr) { Str(k); o_ += ':'; }
  }
  void Str(const std::string& v) {
    o_ += '"';
    for (unsigned char c : v) {
      if (c == '"' || c == '\\') { o_ += '\\'; o_ += static_cast<char>(c); }
      else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o_ += b; }
      else o_ += static_cast<char>(c);
    }
    o_ += '"';
  }
  std::string o_;
  std::vector<char> close_;
  bool need_ = false;
};

// ---- Core Audio endpoint format (read from the property store: opens no stream) ------------------

struct EndpointInfo {
  bool found = false;
  std::string name;
  uint32_t rate = 0;
  uint16_t channels = 0;
};

const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kPkeyDeviceFormat = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

EndpointInfo ReadEndpoint(IMMDevice* dev) {
  EndpointInfo e;
  IPropertyStore* ps = nullptr;
  if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps)) || ps == nullptr) return e;
  PROPVARIANT v;
  PropVariantInit(&v);
  if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR) e.name = Narrow(v.pwszVal);
  PropVariantClear(&v);
  if (SUCCEEDED(ps->GetValue(kPkeyDeviceFormat, &v)) && v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
    const auto* wf = reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData);
    e.rate = wf->nSamplesPerSec;
    e.channels = wf->nChannels;
  }
  PropVariantClear(&v);
  ps->Release();
  e.found = !e.name.empty();
  return e;
}

// substr empty: the default endpoint (console role); otherwise the first active endpoint whose
// friendly name contains it.
EndpointInfo FindEndpoint(bool capture, const std::string& substr) {
  EndpointInfo out;
  IMMDeviceEnumerator* en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&en)))) {
    return out;
  }
  const EDataFlow flow = capture ? eCapture : eRender;
  if (substr.empty()) {
    IMMDevice* d = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &d)) && d != nullptr) {
      out = ReadEndpoint(d);
      d->Release();
    }
  } else {
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) && col != nullptr) {
      UINT n = 0;
      col->GetCount(&n);
      for (UINT i = 0; i < n && !out.found; ++i) {
        IMMDevice* d = nullptr;
        if (FAILED(col->Item(i, &d)) || d == nullptr) continue;
        EndpointInfo e = ReadEndpoint(d);
        d->Release();
        if (e.found && ContainsCi(e.name, substr)) out = e;
      }
      col->Release();
    }
  }
  en->Release();
  return out;
}

// ---- Engine devices ----------------------------------------------------------------------------

struct Device {
  std::string id, name, kind;
  bool is_default = false;
  bool is_communications = false;
};

std::vector<Device> ListDevices(EngineProc& e) {
  std::vector<Device> out;
  Msg r = e.Call("list_devices", [](uint32_t id) { return Req("list_devices", id); });
  if (r->kind() != "devices") {
    std::printf("[bench] list_devices answered %s %s %s\n", r->kind().c_str(), r->Text("code").c_str(), r->Text("detail").c_str());
    return out;
  }
  const Value* arr = r->F("devices");
  if (!arr) return out;
  for (const Value& v : arr->items) {
    Device d;
    if (const Value* x = cbor::MapGet(v, "id")) d.id = std::string(x->text());
    if (const Value* x = cbor::MapGet(v, "name")) d.name = std::string(x->text());
    if (const Value* x = cbor::MapGet(v, "kind")) d.kind = std::string(x->text());
    if (const Value* x = cbor::MapGet(v, "is_default")) d.is_default = x->u != 0;
    if (const Value* x = cbor::MapGet(v, "is_communications")) d.is_communications = x->u != 0;
    out.push_back(std::move(d));
  }
  return out;
}

// The device to use for `kind`: by name substring (a specific endpoint first, then the aliases), or
// the system default when the substring is empty.
std::optional<Device> PickDevice(const std::vector<Device>& all, const char* kind, const std::string& substr) {
  const Device* alias = nullptr;
  const Device* any = nullptr;
  for (const Device& d : all) {
    if (d.kind != kind) continue;
    if (any == nullptr) any = &d;
    if (substr.empty()) {
      if (d.id == "default") return d;
      continue;
    }
    if (!ContainsCi(d.name, substr)) continue;
    if (d.id == "default" || d.id == "communications") {
      if (alias == nullptr) alias = &d;
    } else {
      return d;
    }
  }
  if (alias != nullptr) return *alias;
  if (substr.empty() && any != nullptr) return *any;
  return std::nullopt;
}

// ---- Stats -------------------------------------------------------------------------------------

using Fields = std::map<std::string, double>;
struct Snapshot {
  bool ok = false;
  std::map<std::string, Fields> by_type;  // one entry per type (candidate-pair: the nominated one)
  double Get(const char* type, const char* name, double dflt = NAN) const {
    auto t = by_type.find(type);
    if (t == by_type.end()) return dflt;
    auto f = t->second.find(name);
    return f == t->second.end() ? dflt : f->second;
  }
};

Snapshot Stats(EngineProc& e, uint32_t pc, const char* scope) {
  Snapshot s;
  Msg r = e.Call("get_stats", [&](uint32_t id) {
    return BeginMessage("get_stats", id).Uint("pc", pc).Str("scope", scope).Finish();
  });
  if (r->kind() != "stats") return s;
  const Value* entries = r->F("entries");
  if (!entries) return s;
  s.ok = true;
  for (const Value& entry : entries->items) {
    const Value* type = cbor::MapGet(entry, "type");
    const Value* values = cbor::MapGet(entry, "values");
    if (!type || !values) continue;
    Fields f;
    for (size_t i = 0; i + 1 < values->items.size(); i += 2) {
      const Value& k = values->items[i];
      const Value& v = values->items[i + 1];
      double d = NAN;
      if (v.type == Value::Type::Uint || v.type == Value::Type::Bool) d = static_cast<double>(v.u);
      else if (v.type == Value::Type::Text) {
        char* end = nullptr;
        const std::string t(v.text());
        d = std::strtod(t.c_str(), &end);
        if (end == t.c_str()) d = NAN;
      }
      f[std::string(k.text())] = d;
    }
    const std::string ty(type->text());
    if (ty == "candidate-pair" && s.by_type.count(ty) && !(f.count("nominated") && f["nominated"] != 0)) continue;
    s.by_type[ty] = std::move(f);
  }
  return s;
}

// ---- Dry-run microphone file -------------------------------------------------------------------

bool WriteToneFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  double phase = 0;
  for (int n = 0; n < 12 * 48000; ++n) {
    const double t = n / 48000.0;
    const double env = std::fmax(0.0, std::sin(2.0 * 3.14159265358979 * 3.3 * t));
    const double pitch = 130.0 + 40.0 * std::sin(2.0 * 3.14159265358979 * 0.7 * t);
    phase += 2.0 * 3.14159265358979 * pitch / 48000.0;
    double s = 0;
    for (int h = 1; h <= 8; ++h) s += std::sin(h * phase) / h;
    const int16_t v = static_cast<int16_t>(std::fmax(-30000.0, std::fmin(30000.0, 9000.0 * env * s)));
    const int16_t fr[2] = {v, v};
    std::fwrite(fr, sizeof(int16_t), 2, f);
  }
  std::fclose(f);
  return true;
}

// ---- The call ----------------------------------------------------------------------------------

struct Config {
  bool dry = false;
  std::string capture, render, out, work;
  int seconds = 10;
};

struct Side {
  EngineProc proc;
  uint32_t session = 0, cert = 0, pc = 0;
  Buf fingerprint;
  size_t ice_cursor = 0;
};

Buf RandomKey() {
  Buf k(kKeyBytes);
  if (!FillRandom(k.data(), k.size())) Abort("no random bytes");
  return k;
}

std::wstring HwExe() { return QM_W(QMEDIA_ENGINE_HW_EXE); }
std::wstring CiExe() { return QM_W(QMEDIA_ENGINE_CI_EXE); }

void StartSide(Side& s, const Config& c, const std::string& name) {
  std::wstring exe = HwExe();
  std::wstring extra = L" --hw-allow-loopback";
  if (c.dry) {
    const std::string mic = c.work + "\\" + name + "-mic.raw";
    if (!WriteToneFile(mic)) Abort("cannot write the dry-run microphone file");
    exe = CiExe();
    extra = L" --ci-audio-in \"" + Widen(mic) + L"\" --ci-audio-out \"" + Widen(c.work + "\\" + name + "-spk.raw") +
            L"\" --ci-allow-loopback";
  }
  if (!s.proc.Start(name, exe, extra, c.work + "\\" + name + "-engine.err")) Abort(name + ": engine did not start");
}

void CreatePc(Side& s, const std::string& name) {
  Msg r = s.proc.Call("session_create", [](uint32_t id) { return Req("session_create", id); });
  if (r->kind() != "session_created") Abort(name + ": session_create failed " + r->Text("code") + " " + r->Text("detail"));
  s.session = static_cast<uint32_t>(r->Uint("session"));
  r = s.proc.Call("cert_create", [&](uint32_t id) { return BeginMessage("cert_create", id).Uint("session", s.session).Finish(); });
  if (r->kind() != "cert_created") Abort(name + ": cert_create failed " + r->Text("code") + " " + r->Text("detail"));
  s.cert = static_cast<uint32_t>(r->Uint("cert"));
  s.fingerprint = r->Bytes("fingerprint");
  r = s.proc.Call("pc_create", [&](uint32_t id) {
    return BeginMessage("pc_create", id).Uint("session", s.session).Uint("cert", s.cert).Str("ice_policy", "all").Finish();
  });
  if (r->kind() != "pc_created") Abort(name + ": pc_create failed " + r->Text("code") + " " + r->Text("detail"));
  s.pc = static_cast<uint32_t>(r->Uint("pc"));
}

void InstallKey(Side& s, const char* participant, const Buf& key, bool send) {
  s.proc.CallOk("install_key", [&](uint32_t id) {
    return BeginMessage("install_key", id).Uint("session", s.session).Str("participant", participant)
        .Uint("slot", 0).Bin("key", key).Str("direction", send ? "send" : "recv").Finish();
  });
}

void BindOne(Side& s, const std::string& mid, bool send) {
  s.proc.CallOk("bind_media", [&](uint32_t id) {
    return BeginMessage("bind_media", id).Uint("pc", s.pc).Str("mid", mid)
        .Str("participant", send ? "local" : "remote").Str("direction", send ? "send" : "recv").Finish();
  });
}

void SetDesc(Side& s, bool local, const char* type, const std::string& sdp) {
  s.proc.CallOk(local ? "set_local_description" : "set_remote_description", [&](uint32_t id) {
    return BeginMessage(local ? "set_local_description" : "set_remote_description", id)
        .Uint("pc", s.pc).Str("type", type).Str("sdp", sdp).Finish();
  });
}

void ForwardIce(Side& from, Side& to) {
  for (;;) {
    Msg e = from.proc.EventAt(from.ice_cursor);
    if (!e) return;
    ++from.ice_cursor;
    if (e->kind() != "ice_candidate") continue;
    const std::string cand = e->Text("candidate");
    const std::string mid = e->Text("mid");
    const Value* idx = e->F("mline_index");
    to.proc.Call("add_ice_candidate", [&](uint32_t id) {
      cbor::MapBuilder mb = BeginMessage("add_ice_candidate", id);
      mb.Uint("pc", to.pc).Str("candidate", cand);
      if (!mid.empty()) mb.Str("mid", mid);
      if (idx) mb.Uint("mline_index", idx->u);
      return mb.Finish();
    });
  }
}

bool WaitConnected(Side& a, Side& b, int timeout_s) {
  const auto end = Clock::now() + std::chrono::seconds(timeout_s);
  auto state = [](Side& s, const char* name) {
    return s.proc.FindEvent([&](const Parsed& e) {
             return e.kind() == "pc_state" && e.Uint("pc") == s.pc && e.Text("state") == name;
           }) != nullptr;
  };
  while (Clock::now() < end) {
    ForwardIce(a, b);
    ForwardIce(b, a);
    if (state(a, "connected") && state(b, "connected")) return true;
    if (state(a, "failed") || state(b, "failed")) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

struct Transport {
  bool seen = false, strict_ok = false;
  std::string tls, cipher, group, srtp;
};

// The same assertions as qmedia_call_test's CheckTransport, folded into one verdict per side.
Transport CheckSide(Side& s, const Side& peer) {
  Transport t;
  Msg m = s.proc.WaitEvent([&](const Parsed& e) { return e.kind() == "transport_info" && e.Uint("pc") == s.pc; }, 20);
  if (!m) return t;
  t.seen = true;
  t.tls = m->Text("tls_version");
  t.cipher = m->Text("dtls_cipher");
  t.group = m->Text("group");
  t.srtp = m->Text("srtp_cipher");
  const Buf remote_fp = m->Bytes("remote_cert_fingerprint");
  const Buf local_fp = m->Bytes("local_cert_fingerprint");
  t.strict_ok = t.tls == "DTLS1.3" && t.cipher == "TLS_AES_256_GCM_SHA384" && t.group == "X25519MLKEM768" &&
                t.srtp == "AEAD_AES_256_GCM" && !s.fingerprint.empty() && s.fingerprint != peer.fingerprint &&
                remote_fp.size() == kFingerprintBytes && local_fp.size() == kFingerprintBytes &&
                remote_fp == peer.fingerprint && local_fp == s.fingerprint &&
                s.proc.FindEvent([](const Parsed& e) { return e.kind() == "transport_violation"; }) == nullptr;
  return t;
}

bool WaitCryptor(Side& s, const char* participant, int timeout_s) {
  return s.proc.WaitEvent([&](const Parsed& e) {
           return e.kind() == "cryptor_state" && e.Text("participant") == participant && e.Text("state") == "ok";
         }, timeout_s) != nullptr;
}

std::string Stamp() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  char b[32];
  std::snprintf(b, sizeof b, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  return b;
}

void WriteJson(const std::string& path, const std::string& json) {
  if (path.empty()) return;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) { std::printf("[bench] cannot write %s\n", path.c_str()); return; }
  std::fwrite(json.data(), 1, json.size(), f);
  std::fclose(f);
}

struct Second {
  double cap_rms = NAN, play_rms = NAN;
};

int RunCall(const Config& c) {
  JW j;
  j.Obj();
  j.S("kind", "engine-audio-bench");
  j.S("mode", c.dry ? "dry-file-devices" : "real-hardware");
  j.S("stamp", Stamp());
  j.N("requested_seconds", c.seconds);
  std::vector<std::string> notes, reasons;

  Side a, b;
  StartSide(a, c, "bench-a");
  StartSide(b, c, "bench-b");

  // Devices (real mode): A captures from the chosen microphone, B plays to the chosen speakers.
  std::string cap_name = "(file)", ren_name = "(file)";
  EndpointInfo cap_ep, ren_ep;
  std::vector<Device> list_a, list_b;
  if (!c.dry) {
    list_a = ListDevices(a.proc);
    list_b = ListDevices(b.proc);
    auto cap = PickDevice(list_a, "audio_in", c.capture);
    auto ren = PickDevice(list_b, "audio_out", c.render);
    if (!cap || !ren) {
      std::printf("[bench] FAILED: no %s device matches\n", !cap ? "capture" : "render");
      for (const Device& d : (!cap ? list_a : list_b)) {
        if (d.kind == (!cap ? "audio_in" : "audio_out")) std::printf("[bench]   available: %s\n", d.name.c_str());
      }
      a.proc.Call("shutdown", [](uint32_t id) { return Req("shutdown", id); });
      b.proc.Call("shutdown", [](uint32_t id) { return Req("shutdown", id); });
      return 3;
    }
    cap_name = cap->name;
    ren_name = ren->name;
    a.proc.CallOk("select_device", [&](uint32_t id) {
      return BeginMessage("select_device", id).Str("kind", "audio_in").Str("device", cap->id).Finish();
    });
    b.proc.CallOk("select_device", [&](uint32_t id) {
      return BeginMessage("select_device", id).Str("kind", "audio_out").Str("device", ren->id).Finish();
    });
    // The endpoint format of the shared-mode mix (property store only; nothing is opened).
    cap_ep = FindEndpoint(true, c.capture);
    ren_ep = FindEndpoint(false, c.render);
  }

  CreatePc(a, "bench-a");
  CreatePc(b, "bench-b");
  const Buf o2a = RandomKey();
  const Buf a2o = RandomKey();
  InstallKey(a, "local", o2a, true);
  InstallKey(a, "remote", a2o, false);
  InstallKey(b, "local", a2o, true);
  InstallKey(b, "remote", o2a, false);
  for (Side* s : {&a, &b}) {
    s->proc.CallOk("select_send_slot", [&](uint32_t id) {
      return BeginMessage("select_send_slot", id).Uint("session", s->session).Str("participant", "local").Uint("slot", 0).Finish();
    });
  }

  Msg offer = a.proc.Call("create_offer", [&](uint32_t id) { return BeginMessage("create_offer", id).Uint("pc", a.pc).Finish(); });
  if (offer->kind() != "sdp_ready") Abort("create_offer failed " + offer->Text("code") + " " + offer->Text("detail"));
  std::string offer_sdp = MungeOpus(offer->Text("sdp"));
  const bool offer_fp_ok = FingerprintOfSdp(offer_sdp) == a.fingerprint;
  // One-way call: A only sends, B only receives. The speakers of A and the microphone of B stay closed.
  for (size_t p; (p = offer_sdp.find("a=sendrecv")) != std::string::npos;) offer_sdp.replace(p, 10, "a=sendonly");
  SetDesc(a, true, "offer", offer_sdp);
  const std::string mid = MidOf(offer_sdp);
  if (mid.empty()) Abort("the offer has no mid");
  BindOne(a, mid, true);
  SetDesc(b, false, "offer", offer_sdp);
  BindOne(b, mid, false);
  Msg answer = b.proc.Call("create_answer", [&](uint32_t id) { return BeginMessage("create_answer", id).Uint("pc", b.pc).Finish(); });
  if (answer->kind() != "sdp_ready") Abort("create_answer failed " + answer->Text("code") + " " + answer->Text("detail"));
  const std::string answer_sdp = MungeOpus(answer->Text("sdp"));
  const bool answer_fp_ok = FingerprintOfSdp(answer_sdp) == b.fingerprint;
  SetDesc(b, true, "answer", answer_sdp);
  SetDesc(a, false, "answer", answer_sdp);
  a.proc.CallOk("set_audio_tuning", [&](uint32_t id) {
    return BeginMessage("set_audio_tuning", id).Uint("pc", a.pc).Uint("bitrate_bps", 32000).Uint("fec_floor_pct", 10).Finish();
  });

  const bool up = WaitConnected(a, b, 60);
  Transport ta, tb;
  bool cryptors = false;
  if (up) {
    ta = CheckSide(a, b);
    tb = CheckSide(b, a);
    cryptors = WaitCryptor(a, "local", 30) && WaitCryptor(b, "remote", 30);
  }
  const bool transport_ok = up && offer_fp_ok && answer_fp_ok && ta.strict_ok && tb.strict_ok && cryptors;

  // Audio window.
  std::vector<Second> secs;
  Snapshot base_a, base_b, last_a, last_b, tr_a;
  double packets_before = 0, packets_after = 0;
  if (up) {
    // Wait until packets arrive at B (the microphone may be silent, so energy is not required).
    const auto end = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < end) {
      Snapshot s = Stats(b.proc, b.pc, "audio");
      if (s.Get("inbound-rtp", "packetsReceived", 0) >= 20) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    base_a = Stats(a.proc, a.pc, "audio");
    base_b = Stats(b.proc, b.pc, "audio");
    packets_before = base_b.Get("inbound-rtp", "packetsReceived", 0);
    Snapshot prev_a = base_a, prev_b = base_b;
    for (int i = 0; i < c.seconds; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      Snapshot sa = Stats(a.proc, a.pc, "audio");
      Snapshot sb = Stats(b.proc, b.pc, "audio");
      Second sec;
      const double dea = sa.Get("media-source", "totalAudioEnergy") - prev_a.Get("media-source", "totalAudioEnergy");
      const double dda = sa.Get("media-source", "totalSamplesDuration") - prev_a.Get("media-source", "totalSamplesDuration");
      if (dda > 0.05) sec.cap_rms = std::sqrt(std::fmax(0.0, dea / dda));
      const double deb = sb.Get("inbound-rtp", "totalAudioEnergy") - prev_b.Get("inbound-rtp", "totalAudioEnergy");
      const double ddb = sb.Get("inbound-rtp", "totalSamplesDuration") - prev_b.Get("inbound-rtp", "totalSamplesDuration");
      if (ddb > 0.05) sec.play_rms = std::sqrt(std::fmax(0.0, deb / ddb));
      secs.push_back(sec);
      prev_a = sa;
      prev_b = sb;
      last_a = sa;
      last_b = sb;
      std::printf("[bench] t=%2d capture %.1f dBFS  playout %.1f dBFS\n", i + 1, ToDb(sec.cap_rms), ToDb(sec.play_rms));
      std::fflush(stdout);
    }
    packets_after = last_b.Get("inbound-rtp", "packetsReceived", 0);
    tr_a = Stats(a.proc, a.pc, "transport");
  }

  // Levels.
  auto summarize = [&](bool cap, double* avg_db, double* peak_db, double* active_frac, int* n) {
    double sum_sq = 0, peak = 0;
    int active = 0;
    *n = 0;
    for (const Second& s : secs) {
      const double r = cap ? s.cap_rms : s.play_rms;
      if (!std::isfinite(r)) continue;
      ++*n;
      sum_sq += r * r;
      peak = std::fmax(peak, r);
      if (ToDb(r) > -55.0) ++active;
    }
    *avg_db = *n ? ToDb(std::sqrt(sum_sq / *n)) : NAN;
    *peak_db = *n ? ToDb(peak) : NAN;
    *active_frac = *n ? static_cast<double>(active) / *n : 0.0;
  };
  double cap_avg, cap_peak, cap_act, ply_avg, ply_peak, ply_act;
  int cap_n, ply_n;
  summarize(true, &cap_avg, &cap_peak, &cap_act, &cap_n);
  summarize(false, &ply_avg, &ply_peak, &ply_act, &ply_n);

  // Concealment: the closest the engine's statistics get to underruns and glitches. The raw ADM
  // underrun counters are not part of the reduced statistics report.
  auto delta = [&](const char* name) { return last_b.Get("inbound-rtp", name, 0) - base_b.Get("inbound-rtp", name, 0); };
  const double d_samples = delta("totalSamplesReceived");
  const double d_conceal = delta("concealedSamples");
  const double glitch_ratio = d_samples > 0 ? d_conceal / d_samples : NAN;
  const double jb_delay = delta("jitterBufferDelay");
  const double jb_count = delta("jitterBufferEmittedCount");
  const double jb_ms = jb_count > 0 ? 1000.0 * jb_delay / jb_count : NAN;
  const double rtt_ms = 1000.0 * tr_a.Get("candidate-pair", "currentRoundTripTime");

  const bool flowing = up && (packets_after - packets_before) >= 5.0 * c.seconds;
  const bool cap_signal = cap_act >= 0.2;
  const bool ply_signal = ply_act >= 0.2;
  const bool glitch_ok = !std::isfinite(glitch_ratio) || glitch_ratio < 0.05;
  if (!transport_ok) reasons.push_back("transport or key check failed (see transport)");
  if (up && !flowing) reasons.push_back("fewer audio packets than expected reached the playing side");
  if (flowing && !cap_signal) reasons.push_back("the capture side stayed below -55 dBFS for most of the run (was anybody speaking?)");
  if (flowing && cap_signal && !ply_signal) reasons.push_back("capture had signal but the playout side did not");
  if (!glitch_ok) reasons.push_back("more than 5 percent of played samples were concealed");
  std::string verdict = "fail";
  if (transport_ok && flowing && cap_signal && ply_signal && glitch_ok) verdict = "pass";
  else if (transport_ok && flowing && !cap_signal) verdict = "transport-ok-no-signal";

  // Observations.
  auto hfp = [&](const EndpointInfo& e, const std::string& adm_name, const char* what) {
    const bool by_rate = e.found && e.rate != 0 && e.rate <= 16000;
    const bool by_name = ContainsCi(adm_name, "hands-free") || ContainsCi(adm_name, "hands free") || ContainsCi(adm_name, "hfp");
    if (by_rate || by_name) {
      notes.push_back(std::string(what) + " endpoint looks like the Bluetooth hands-free profile (HFP: " +
                      (e.found ? std::to_string(e.rate) + " Hz, " + std::to_string(e.channels) + " channel(s)" : std::string("format unknown")) +
                      "). This is a platform limit of the profile (narrow band, mono), not an engine setting; the engine resamples to 48 kHz.");
    }
  };
  if (!c.dry) {
    hfp(cap_ep, cap_name, "capture");
    hfp(ren_ep, ren_name, "render");
  }
  notes.push_back("Capture level is measured on the capture source of the engine (after echo cancellation, noise suppression and gain control); playout level on the decoded stream before the sound card.");
  notes.push_back("Underruns of the sound card are not exposed by the engine statistics; concealed samples are the closest measure of audible glitches.");
  notes.push_back("End-to-end latency here is network round trip plus jitter buffer. Device buffers and Bluetooth transport delay are not included; an acoustic loopback measurement would be needed for those.");

  // ---- JSON
  j.B("transport_ok", transport_ok);
  j.S("verdict", verdict);
  j.Arr("verdict_reasons"); for (auto& r : reasons) { j.S(nullptr, r); } j.End();
  j.Obj("devices");
  j.S("capture_used", cap_name);
  j.S("render_used", ren_name);
  j.Obj("capture_endpoint"); j.B("found", cap_ep.found); j.N("sample_rate_hz", cap_ep.rate); j.N("channels", cap_ep.channels); j.End();
  j.Obj("render_endpoint"); j.B("found", ren_ep.found); j.N("sample_rate_hz", ren_ep.rate); j.N("channels", ren_ep.channels); j.End();
  j.Arr("capture_list"); for (auto& d : list_a) if (d.kind == "audio_in") { j.Obj(); j.S("name", d.name); j.B("is_default", d.is_default); j.B("is_communications", d.is_communications); j.End(); } j.End();
  j.Arr("render_list"); for (auto& d : list_b) if (d.kind == "audio_out") { j.Obj(); j.S("name", d.name); j.B("is_default", d.is_default); j.B("is_communications", d.is_communications); j.End(); } j.End();
  j.End();
  j.Obj("transport");
  j.B("connected", up);
  j.B("offer_fingerprint_matches_cert_create", offer_fp_ok);
  j.B("answer_fingerprint_matches_cert_create", answer_fp_ok);
  j.B("sender_dtls_srtp_strict_ok", ta.strict_ok);
  j.B("receiver_dtls_srtp_strict_ok", tb.strict_ok);
  j.B("frame_cryptors_ok", cryptors);
  j.S("tls_version", ta.tls); j.S("dtls_cipher", ta.cipher); j.S("group", ta.group); j.S("srtp_cipher", ta.srtp);
  j.End();
  j.Obj("levels");
  j.N("seconds_measured", cap_n);
  j.Obj("capture_dbfs"); j.N("avg", cap_avg); j.N("peak_second", cap_peak); j.N("active_fraction", cap_act); j.End();
  j.Obj("playout_dbfs"); j.N("avg", ply_avg); j.N("peak_second", ply_peak); j.N("active_fraction", ply_act); j.End();
  j.Arr("per_second");
  for (const Second& s : secs) { j.Obj(); j.N("capture_dbfs", ToDb(s.cap_rms)); j.N("playout_dbfs", ToDb(s.play_rms)); j.End(); }
  j.End();
  j.End();
  j.Obj("glitches");
  j.N("concealed_samples", d_conceal);
  j.N("played_samples", d_samples);
  j.N("concealed_ratio", glitch_ratio);
  j.N("concealment_events", delta("concealmentEvents"));
  j.N("inserted_samples_for_deceleration", delta("insertedSamplesForDeceleration"));
  j.N("removed_samples_for_acceleration", delta("removedSamplesForAcceleration"));
  j.N("packets_lost", delta("packetsLost"));
  j.N("packets_discarded", delta("packetsDiscarded"));
  j.N("adm_underruns", NAN);
  j.End();
  j.Obj("latency");
  j.N("rtt_ms", rtt_ms);
  j.N("jitter_buffer_ms", jb_ms);
  j.N("estimated_one_way_ms", (std::isfinite(rtt_ms) ? rtt_ms / 2 : 0) + (std::isfinite(jb_ms) ? jb_ms : 0));
  j.End();
  j.N("packets_received_in_window", packets_after - packets_before);
  j.Arr("observations"); for (auto& n : notes) { j.S(nullptr, n); } j.End();
  j.End();

  for (Side* s : {&a, &b}) {
    s->proc.Call("session_close", [&](uint32_t id) { return BeginMessage("session_close", id).Uint("session", s->session).Finish(); });
    s->proc.Call("shutdown", [](uint32_t id) { return Req("shutdown", id); });
  }
  const bool exit_ok = a.proc.WaitExit(20000) == 0 && b.proc.WaitExit(20000) == 0;
  if (!exit_ok) std::printf("[bench] note: an engine did not exit cleanly\n");
  WriteJson(c.out, j.str());
  std::printf("[bench] verdict: %s (transport %s, %d s measured)\n", verdict.c_str(), transport_ok ? "strict ok" : "NOT ok", cap_n);
  for (auto& r : reasons) std::printf("[bench]   reason: %s\n", r.c_str());
  return verdict == "pass" || verdict == "transport-ok-no-signal" ? 0 : 1;
}

// ---- list / selfcheck --------------------------------------------------------------------------

int RunList(const Config& c, bool selfcheck) {
  Config cc = c;
  Side a;
  StartSide(a, cc, "bench-list");
  const std::vector<Device> devs = ListDevices(a.proc);
  std::printf("[bench] devices of the Windows audio device module (as listed by the engine, nothing is opened)\n");
  for (const Device& d : devs) {
    std::printf("[bench]   %-9s default=%d communications=%d  %s\n", d.kind.c_str(), d.is_default ? 1 : 0,
                d.is_communications ? 1 : 0, d.name.c_str());
  }
  for (bool cap : {true, false}) {
    const EndpointInfo e = FindEndpoint(cap, "");
    if (e.found) {
      std::printf("[bench]   default %s endpoint: %s, %u Hz, %u channel(s)\n", cap ? "capture" : "render", e.name.c_str(), e.rate, e.channels);
    }
  }
  bool ok = !devs.empty();
  if (selfcheck) {
    Msg r = a.proc.Call("session_create", [](uint32_t id) { return Req("session_create", id); });
    ok = ok && r->kind() == "session_created";
    if (r->kind() != "session_created") std::printf("[bench] session_create answered %s %s %s\n", r->kind().c_str(), r->Text("code").c_str(), r->Text("detail").c_str());
    if (ok) {
      const uint32_t sid = static_cast<uint32_t>(r->Uint("session"));
      Msg ce = a.proc.Call("cert_create", [&](uint32_t id) { return BeginMessage("cert_create", id).Uint("session", sid).Finish(); });
      ok = ce->kind() == "cert_created" && ce->Bytes("fingerprint").size() == kFingerprintBytes;
    }
    std::printf("[bench] selfcheck engine session and certificate: %s\n", ok ? "ok" : "FAILED");
  }
  a.proc.Call("shutdown", [](uint32_t id) { return Req("shutdown", id); });
  const bool exited = a.proc.WaitExit(20000) == 0;
  std::printf("[bench] engine exit: %s\n", exited ? "clean" : "NOT clean");
  return ok && exited ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Config c;
  enum { kNone, kList, kSelf, kDry, kReal } mode = kNone;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--list") mode = kList;
    else if (a == "--selfcheck") mode = kSelf;
    else if (a == "--dry") { mode = kDry; c.dry = true; }
    else if (a == "--real") mode = kReal;
    else if (a == "--capture") c.capture = val();
    else if (a == "--render") c.render = val();
    else if (a == "--seconds") c.seconds = std::atoi(val().c_str());
    else if (a == "--out") c.out = val();
    else if (a == "--work") c.work = val();
    else { std::printf("[bench] unknown option\n"); return 2; }
  }
  if (mode == kNone || c.seconds < 3 || c.seconds > 600 || c.work.empty()) {
    std::printf("usage: qmedia_audio_bench (--list | --selfcheck | --dry | --real) --work <dir> [--capture <name part>] "
                "[--render <name part>] [--seconds N] [--out <json>]\n");
    return 2;
  }
  CreateDirectoryA(c.work.c_str(), nullptr);
  if (mode == kList) return RunList(c, false);
  if (mode == kSelf) return RunList(c, true);
  return RunCall(c);
}
