// End-to-end call test: two engine processes (the CI build of qaudion-media, linked against the
// published libwebrtc) are driven over their pipes through a complete 1:1 audio call.
//
//   call 1  both sides share the per-direction frame keys. Asserts, on both ends: DTLS 1.3,
//           TLS_AES_256_GCM_SHA384, X25519MLKEM768, AEAD_AES_256_GCM, that the remote certificate
//           fingerprint equals the peer's cert_create fingerprint and that the local certificate
//           fingerprint equals the side's own cert_create fingerprint (and neither is the
//           other's). Asserts that the
//           cryptors report OK, that audio packets arrive and are decoded to non-silent audio on
//           both sides (stats and the played-out file). Then retires the receive slot of one side
//           and asserts that decryption fails and the decoded audio stops.
//   call 2  the receiver holds a wrong key: decryption fails and no audio comes out.
//   call 2  (continued) both sides then rebuild their peer connection in the same session with the same
//           certificate handle and connect again: each side again reports its own certificate as local.
//   call 3  the caller is given an answer whose fingerprint is not the callee's certificate: the
//           caller's DTLS must fail, so it never reaches "connected" and never reports a
//           transport_info that could pass the host's remote check.
//
// Audio device: the engine's CI build replaces the sound card with libwebrtc's FileAudioDevice,
// which reads the "microphone" from a raw 48 kHz stereo file (generated here) and writes the
// played-out audio to a raw file.
//
// The output carries verdicts only: no SDP, no fingerprints, no addresses, no keys.
#include "engine_harness.h"

#define QM_W2(x) L##x
#define QM_W(x) QM_W2(x)
#define QM_EXE_W QM_W(QMEDIA_ENGINE_CI_EXE)

namespace {

// ---- The microphone file -----------------------------------------------------------------------

bool WriteMicFile(const std::string& path, double seed) {
  // 48 kHz stereo 16-bit raw. Voiced-speech-like: harmonics of a pitch that moves, under a
  // syllable-rate envelope with pauses, so noise suppression does not treat it as stationary noise.
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const int seconds = 10;
  const int rate = 48000;
  std::vector<int16_t> frame(2);
  double phase = seed;
  for (int n = 0; n < seconds * rate; ++n) {
    const double t = static_cast<double>(n) / rate;
    const double syll = std::fmax(0.0, std::sin(2.0 * 3.14159265358979 * 3.3 * t + seed));
    const double pitch = 110.0 + 40.0 * std::sin(2.0 * 3.14159265358979 * 0.7 * t + seed);
    phase += 2.0 * 3.14159265358979 * pitch / rate;
    double s = 0;
    for (int h = 1; h <= 8; ++h) s += std::sin(h * phase) / h;
    const double v = 9000.0 * syll * s;
    frame[0] = frame[1] = static_cast<int16_t>(std::fmax(-30000.0, std::fmin(30000.0, v)));
    std::fwrite(frame.data(), sizeof(int16_t), 2, f);
  }
  std::fclose(f);
  return true;
}

int MaxAbsSample(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return -1;
  int max_abs = 0;
  std::vector<int16_t> buf(4096);
  size_t n;
  while ((n = std::fread(buf.data(), sizeof(int16_t), buf.size(), f)) > 0) {
    for (size_t i = 0; i < n; ++i) max_abs = std::max(max_abs, std::abs(static_cast<int>(buf[i])));
  }
  std::fclose(f);
  return max_abs;
}

// ---- Stats helpers -----------------------------------------------------------------------------

struct AudioIn {
  bool found = false;
  uint64_t packets = 0;
  double energy = 0;
};

AudioIn ReadInbound(EngineProc& e, uint32_t pc) {
  Msg r = e.Call("get_stats", [&](uint32_t id) {
    return BeginMessage("get_stats", id).Uint("pc", pc).Str("scope", "audio").Finish();
  });
  AudioIn out;
  if (r->kind() != "stats") return out;
  const Value* entries = r->F("entries");
  if (!entries) return out;
  for (const Value& entry : entries->items) {
    const Value* type = cbor::MapGet(entry, "type");
    if (!type || type->text() != "inbound-rtp") continue;
    const Value* values = cbor::MapGet(entry, "values");
    if (!values) continue;
    out.found = true;
    if (const Value* p = cbor::MapGet(*values, "packetsReceived")) out.packets = p->u;
    if (const Value* en = cbor::MapGet(*values, "totalAudioEnergy")) {
      out.energy = en->type == Value::Type::Text ? std::strtod(std::string(en->text()).c_str(), nullptr)
                                                  : static_cast<double>(en->u);
    }
  }
  return out;
}

// ---- One call ----------------------------------------------------------------------------------

struct Side {
  EngineProc proc;
  uint32_t session = 0;
  uint32_t cert = 0;
  uint32_t pc = 0;
  Buf fingerprint;
  size_t ice_cursor = 0;
};

Buf RandomKey() {
  Buf k(kKeyBytes);
  if (!FillRandom(k.data(), k.size())) Abort("no random bytes");
  return k;
}

void SetupSide(Side& s, const std::string& dir, const std::string& name, double seed) {
  const std::string mic = dir + "\\" + name + "-mic.raw";
  const std::string spk = dir + "\\" + name + "-spk.raw";
  if (!WriteMicFile(mic, seed)) Abort("cannot write the microphone file");
  const std::wstring extra = L" --ci-audio-in \"" + Widen(mic) + L"\" --ci-audio-out \"" + Widen(spk) +
                             L"\" --ci-allow-loopback";
  if (!s.proc.Start(name, QM_EXE_W, extra, dir + "\\" + name + "-engine.err")) Abort(name + ": engine did not start");
  Msg r = s.proc.Call("session_create", [](uint32_t id) { return Req("session_create", id); });
  if (r->kind() != "session_created") Abort(name + ": session_create failed " + r->Text("code") + " " + r->Text("detail"));
  s.session = static_cast<uint32_t>(r->Uint("session"));
  r = s.proc.Call("cert_create", [&](uint32_t id) {
    return BeginMessage("cert_create", id).Uint("session", s.session).Finish();
  });
  if (r->kind() != "cert_created") Abort(name + ": cert_create answered " + r->kind() + " " + r->Text("code") + " " + r->Text("detail"));
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
    return BeginMessage("install_key", id)
        .Uint("session", s.session)
        .Str("participant", participant)
        .Uint("slot", 0)
        .Bin("key", key)
        .Str("direction", send ? "send" : "recv")
        .Finish();
  });
}

void Bind(Side& s, const std::string& mid) {
  for (bool send : {true, false}) {
    s.proc.CallOk("bind_media", [&](uint32_t id) {
      return BeginMessage("bind_media", id)
          .Uint("pc", s.pc)
          .Str("mid", mid)
          .Str("participant", send ? "local" : "remote")
          .Str("direction", send ? "send" : "recv")
          .Finish();
    });
  }
}

void SetDesc(Side& s, bool local, const char* type, const std::string& sdp) {
  s.proc.CallOk(local ? "set_local_description" : "set_remote_description", [&](uint32_t id) {
    return BeginMessage(local ? "set_local_description" : "set_remote_description", id)
        .Uint("pc", s.pc)
        .Str("type", type)
        .Str("sdp", sdp)
        .Finish();
  });
}

// Forwards the ICE candidates `from` has produced so far to `to`.
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
    });  // a candidate that arrives too early or twice may be refused: not a failure
  }
}

bool WaitConnected(Side& a, Side& b, int timeout_s) {
  const auto end = Clock::now() + std::chrono::seconds(timeout_s);
  // Only the events of the side's current peer connection count (a side can rebuild its own).
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

// `pc` (when not 0) restricts the check to the transport_info of that peer connection.
void CheckTransport(Side& s, const Side& peer, const std::string& label, uint32_t pc = 0) {
  // The negative checks below only mean something when the two certificates differ.
  Check(!s.fingerprint.empty() && s.fingerprint != peer.fingerprint,
        label + ": the two sides hold different certificates");
  Msg t = s.proc.WaitEvent([pc](const Parsed& e) { return e.kind() == "transport_info" && (pc == 0 || e.Uint("pc") == pc); }, 20);
  Check(t != nullptr, label + ": transport_info event");
  if (!t) return;
  Check(t->Text("tls_version") == "DTLS1.3", label + ": DTLS 1.3");
  Check(t->Text("dtls_cipher") == "TLS_AES_256_GCM_SHA384", label + ": TLS_AES_256_GCM_SHA384");
  Check(t->Text("group") == "X25519MLKEM768", label + ": X25519MLKEM768");
  Check(t->Text("srtp_cipher") == "AEAD_AES_256_GCM", label + ": AEAD_AES_256_GCM");
  const Buf remote_fp = t->Bytes("remote_cert_fingerprint");
  const Buf local_fp = t->Bytes("local_cert_fingerprint");
  Check(remote_fp.size() == kFingerprintBytes && local_fp.size() == kFingerprintBytes,
        label + ": both certificate fingerprints are 32 bytes");
  Check(remote_fp == peer.fingerprint, label + ": remote fingerprint equals the peer's cert_create fingerprint");
  Check(local_fp == s.fingerprint, label + ": local fingerprint equals this side's own cert_create fingerprint");
  Check(remote_fp != s.fingerprint, label + ": remote fingerprint is not this side's own certificate");
  Check(local_fp != peer.fingerprint, label + ": local fingerprint is not the peer's certificate");
  Check(local_fp != remote_fp, label + ": local and remote fingerprints differ");
  Check(s.proc.FindEvent([](const Parsed& e) { return e.kind() == "transport_violation"; }) == nullptr,
        label + ": no transport_violation");
}

bool HasCryptorState(Side& s, const char* participant, const char* state) {
  return s.proc.FindEvent([&](const Parsed& e) {
           return e.kind() == "cryptor_state" && e.Text("participant") == participant && e.Text("state") == state;
         }) != nullptr;
}

bool WaitCryptorState(Side& s, const char* participant, const char* state, int timeout_s) {
  return s.proc.WaitEvent([&](const Parsed& e) {
           return e.kind() == "cryptor_state" && e.Text("participant") == participant && e.Text("state") == state;
         }, timeout_s) != nullptr;
}

// The SDP with the first hex digit of its a=fingerprint line changed, so it no longer matches the
// certificate of the side that wrote it.
std::string WithWrongFingerprint(const std::string& sdp) {
  const std::string key = "a=fingerprint:sha-256 ";
  const size_t p = sdp.find(key);
  if (p == std::string::npos) return sdp;
  std::string out = sdp;
  char& c = out[p + key.size()];
  c = (c == '0') ? '1' : '0';
  return out;
}

void Handshake(Side& a, Side& b, bool corrupt_answer_fp);

// Negotiates and connects two sides. `b_recv_key` is what B holds for A's direction. With
// `corrupt_answer_fp` the caller is handed an answer whose fingerprint is not the callee's.
void Negotiate(Side& a, Side& b, const Buf& o2a, const Buf& a2o, const Buf& b_recv_key,
               bool corrupt_answer_fp = false) {
  InstallKey(a, "local", o2a, true);
  InstallKey(a, "remote", a2o, false);
  InstallKey(b, "local", a2o, true);
  InstallKey(b, "remote", b_recv_key, false);
  for (Side* s : {&a, &b}) {
    s->proc.CallOk("select_send_slot", [&](uint32_t id) {
      return BeginMessage("select_send_slot", id).Uint("session", s->session).Str("participant", "local").Uint("slot", 0).Finish();
    });
  }
  Handshake(a, b, corrupt_answer_fp);
}

// Offer, answer, media binding and tuning on the current peer connections of both sides (the keys
// of the sessions are already in place).
void Handshake(Side& a, Side& b, bool corrupt_answer_fp) {
  Msg offer = a.proc.Call("create_offer", [&](uint32_t id) { return BeginMessage("create_offer", id).Uint("pc", a.pc).Finish(); });
  if (offer->kind() != "sdp_ready") Abort("create_offer failed " + offer->Text("code") + " " + offer->Text("detail"));
  const std::string offer_sdp = MungeOpus(offer->Text("sdp"));
  Check(FingerprintOfSdp(offer_sdp) == a.fingerprint, "the offer announces the certificate cert_create returned (check a)");
  SetDesc(a, true, "offer", offer_sdp);
  const std::string mid = MidOf(offer_sdp);
  if (mid.empty()) Abort("the offer has no mid");
  Bind(a, mid);

  SetDesc(b, false, "offer", offer_sdp);
  Bind(b, mid);
  Msg answer = b.proc.Call("create_answer", [&](uint32_t id) { return BeginMessage("create_answer", id).Uint("pc", b.pc).Finish(); });
  if (answer->kind() != "sdp_ready") Abort("create_answer failed " + answer->Text("code") + " " + answer->Text("detail"));
  const std::string answer_sdp = MungeOpus(answer->Text("sdp"));
  Check(FingerprintOfSdp(answer_sdp) == b.fingerprint, "the answer announces the certificate cert_create returned (check a)");
  SetDesc(b, true, "answer", answer_sdp);
  SetDesc(a, false, "answer", corrupt_answer_fp ? WithWrongFingerprint(answer_sdp) : answer_sdp);

  // The mobile apps clamp the encoder to the negotiated rate once the sender exists.
  for (Side* s : {&a, &b}) {
    s->proc.CallOk("set_audio_tuning", [&](uint32_t id) {
      return BeginMessage("set_audio_tuning", id).Uint("pc", s->pc).Uint("bitrate_bps", 32000).Uint("fec_floor_pct", 10).Finish();
    });
  }
}

void Finish(Side& a, Side& b) {
  for (Side* s : {&a, &b}) {
    s->proc.Call("session_close", [&](uint32_t id) { return BeginMessage("session_close", id).Uint("session", s->session).Finish(); });
    s->proc.Call("shutdown", [](uint32_t id) { return Req("shutdown", id); });
  }
  Check(a.proc.WaitExit(20000) == 0, a.proc.tag() + ": engine exits cleanly after shutdown");
  Check(b.proc.WaitExit(20000) == 0, b.proc.tag() + ": engine exits cleanly after shutdown");
}

// Waits until `e` has decoded audio: packets arrived and the decoded signal carries energy.
bool WaitAudio(EngineProc& e, uint32_t pc, int timeout_s, AudioIn* last) {
  const auto end = Clock::now() + std::chrono::seconds(timeout_s);
  while (Clock::now() < end) {
    *last = ReadInbound(e, pc);
    if (last->found && last->packets >= 20 && last->energy > 1e-4) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  return false;
}

void Call1(const std::string& dir) {
  std::printf("[call] ---- call 1: shared keys, then a retired slot ----\n");
  Side a, b;
  SetupSide(a, dir, "c1a", 0.3);
  SetupSide(b, dir, "c1b", 1.7);
  const Buf o2a = RandomKey();
  const Buf a2o = RandomKey();
  Negotiate(a, b, o2a, a2o, o2a);
  const bool up = WaitConnected(a, b, 90);
  Check(up, "call 1: both peer connections connected over loopback");
  if (!up) {
    a.proc.DumpStderr();
    b.proc.DumpStderr();
    Abort("no connection");
  }
  CheckTransport(a, b, "call 1 caller");
  CheckTransport(b, a, "call 1 callee");

  Check(WaitCryptorState(a, "remote", "ok", 30), "call 1 caller: receiver cryptor reports OK");
  Check(WaitCryptorState(b, "remote", "ok", 30), "call 1 callee: receiver cryptor reports OK");
  Check(WaitCryptorState(a, "local", "ok", 30), "call 1 caller: sender cryptor reports OK");
  Check(WaitCryptorState(b, "local", "ok", 30), "call 1 callee: sender cryptor reports OK");

  AudioIn ia, ib;
  Check(WaitAudio(a.proc, a.pc, 40, &ia), "call 1 caller: audio packets arrive and decode to non-silent audio");
  Check(WaitAudio(b.proc, b.pc, 40, &ib), "call 1 callee: audio packets arrive and decode to non-silent audio");

  // Retire the receive slot of the callee: the caller's frames can no longer be decrypted.
  const AudioIn before = ReadInbound(b.proc, b.pc);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const AudioIn mid = ReadInbound(b.proc, b.pc);
  const double rate = (mid.energy - before.energy) / 2.0;
  std::printf("[call]   note: energy rate while keyed %.6g per second\n", rate);
  Check(rate > 1e-5, "call 1 callee: decoded audio energy grows while the key is installed");
  b.proc.CallOk("retire_slot", [&](uint32_t id) {
    return BeginMessage("retire_slot", id).Uint("session", b.session).Str("participant", "remote").Str("direction", "recv").Uint("slot", 0).Finish();
  });
  const bool failed = WaitCryptorState(b, "remote", "decryption_failed", 30) || HasCryptorState(b, "remote", "missing_key");
  Check(failed, "call 1 callee: a retired slot makes the receiver cryptor report a failure");
  std::this_thread::sleep_for(std::chrono::seconds(3));  // lets concealment of the last frames die out
  const AudioIn settled = ReadInbound(b.proc, b.pc);
  std::this_thread::sleep_for(std::chrono::seconds(3));
  const AudioIn after = ReadInbound(b.proc, b.pc);
  std::printf("[call]   note: energy delta after retirement %.6g over 3 s\n", after.energy - settled.energy);
  Check(after.packets > settled.packets, "call 1 callee: packets keep arriving after the retirement");
  Check((after.energy - settled.energy) < rate * 3.0 * 0.02, "call 1 callee: no decoded audio after the retirement");

  Finish(a, b);
  const int spk_a = MaxAbsSample(dir + "\\c1a-spk.raw");
  const int spk_b = MaxAbsSample(dir + "\\c1b-spk.raw");
  Check(spk_a > 50, "call 1 caller: the played-out file carries audio");
  Check(spk_b > 50, "call 1 callee: the played-out file carries audio");
  Check(!a.proc.invalid_message() && !b.proc.invalid_message(), "call 1: every engine message validated against the schema");
}

void Call2(const std::string& dir) {
  std::printf("[call] ---- call 2: the receiver holds a wrong key ----\n");
  Side a, b;
  SetupSide(a, dir, "c2a", 0.9);
  SetupSide(b, dir, "c2b", 2.4);
  const Buf o2a = RandomKey();
  const Buf a2o = RandomKey();
  const Buf wrong = RandomKey();
  Negotiate(a, b, o2a, a2o, wrong);
  const bool up = WaitConnected(a, b, 90);
  Check(up, "call 2: both peer connections connected over loopback");
  if (!up) {
    a.proc.DumpStderr();
    b.proc.DumpStderr();
    Abort("no connection");
  }
  CheckTransport(b, a, "call 2 callee");
  Check(WaitCryptorState(b, "remote", "decryption_failed", 30), "call 2 callee: the wrong key gives a decryption failure");
  // The caller holds the right key for the callee's direction: its audio is fine.
  AudioIn ia;
  Check(WaitAudio(a.proc, a.pc, 40, &ia), "call 2 caller: audio from the callee still decrypts (control)");
  std::this_thread::sleep_for(std::chrono::seconds(3));
  const AudioIn ib = ReadInbound(b.proc, b.pc);
  std::printf("[call]   note: wrong key: packets %llu energy %.6g; control energy %.6g\n",
              static_cast<unsigned long long>(ib.packets), ib.energy, ia.energy);
  Check(ib.packets >= 20, "call 2 callee: packets arrive");
  Check(ib.energy < 1e-4, "call 2 callee: no decoded audio with the wrong key");

  // The remaining commands, on a live call.
  Msg devs = a.proc.Call("list_devices", [](uint32_t id) { return Req("list_devices", id); });
  Check(devs->kind() == "devices", "list_devices answers with a device list");
  a.proc.CallOk("set_muted mic", [&](uint32_t id) {
    return BeginMessage("set_muted", id).Uint("pc", a.pc).Str("track", "mic").Flag("muted", true).Finish();
  });
  a.proc.CallOk("set_muted mic off", [&](uint32_t id) {
    return BeginMessage("set_muted", id).Uint("pc", a.pc).Str("track", "mic").Flag("muted", false).Finish();
  });
  Msg video = a.proc.Call("set_muted camera", [&](uint32_t id) {
    return BeginMessage("set_muted", id).Uint("pc", a.pc).Str("track", "camera").Flag("muted", true).Finish();
  });
  Check(video->kind() == "err" && video->Text("code") == "unsupported", "video commands are refused in this build");
  Msg tune = a.proc.Call("set_audio_tuning ptime", [&](uint32_t id) {
    return BeginMessage("set_audio_tuning", id).Uint("pc", a.pc).Uint("ptime_ms", 60).Finish();
  });
  Check(tune->kind() == "err" && tune->Text("code") == "unsupported", "ptime is an SDP parameter and is refused at run time");
  Msg ts = a.proc.Call("get_stats transport", [&](uint32_t id) {
    return BeginMessage("get_stats", id).Uint("pc", a.pc).Str("scope", "transport").Finish();
  });
  Check(ts->kind() == "stats", "get_stats answers for the transport scope");
  a.proc.CallOk("update_ice_servers", [&](uint32_t id) {
    cbor::ArrayBuilder none;
    return BeginMessage("update_ice_servers", id).Uint("pc", a.pc).Raw("ice_servers", none.Finish()).Finish();
  });
  a.proc.CallOk("restart_ice", [&](uint32_t id) { return BeginMessage("restart_ice", id).Uint("pc", a.pc).Finish(); });

  // Peer connections rebuilt in the same session present the same certificates (R-CERT): both
  // sides close their connection and build another one with the certificate handle they already
  // hold, connect again, and each reports its own certificate as the local one, the same as before.
  const uint32_t old_a = a.pc;
  const uint32_t old_b = b.pc;
  for (Side* s : {&a, &b}) {
    s->proc.CallOk("pc_close", [&](uint32_t id) { return BeginMessage("pc_close", id).Uint("pc", s->pc).Finish(); });
    Msg again = s->proc.Call("pc_create again", [&](uint32_t id) {
      return BeginMessage("pc_create", id).Uint("session", s->session).Uint("cert", s->cert).Str("ice_policy", "all").Finish();
    });
    Check(again->kind() == "pc_created", s->proc.tag() + ": a second peer connection is created with the same certificate handle");
    if (again->kind() != "pc_created") Abort("pc_create again failed");
    s->pc = static_cast<uint32_t>(again->Uint("pc"));
  }
  Check(a.pc != old_a && b.pc != old_b, "the rebuilt peer connections have new handles");
  Handshake(a, b, false);
  const bool up2 = WaitConnected(a, b, 90);
  Check(up2, "call 2: the rebuilt peer connections connect over loopback");
  if (up2) {
    CheckTransport(a, b, "call 2 rebuilt caller", a.pc);
    CheckTransport(b, a, "call 2 rebuilt callee", b.pc);
    for (Side* s : {&a, &b}) {
      const uint32_t before = s == &a ? old_a : old_b;
      const uint32_t now = s->pc;
      Msg first = s->proc.FindEvent([before](const Parsed& e) { return e.kind() == "transport_info" && e.Uint("pc") == before; });
      Msg second = s->proc.FindEvent([now](const Parsed& e) { return e.kind() == "transport_info" && e.Uint("pc") == now; });
      Check(first && second && first->Bytes("local_cert_fingerprint") == second->Bytes("local_cert_fingerprint"),
            s->proc.tag() + ": the local fingerprint is the same before and after the rebuild");
    }
  } else {
    a.proc.DumpStderr();
    b.proc.DumpStderr();
  }
  Finish(a, b);
  Check(!a.proc.invalid_message() && !b.proc.invalid_message(), "call 2: every engine message validated against the schema");
}

// Negative case for the remote check: the caller expects another certificate than the callee's. The
// library must refuse the handshake on the caller, so no transport_info (which would carry the
// callee's real fingerprint next to a connection the caller believes is up) can appear there.
void Call3(const std::string& dir) {
  std::printf("[call] ---- call 3: the caller is told a wrong fingerprint for the callee ----\n");
  Side a, b;
  SetupSide(a, dir, "c3a", 0.5);
  SetupSide(b, dir, "c3b", 2.1);
  const Buf o2a = RandomKey();
  const Buf a2o = RandomKey();
  Negotiate(a, b, o2a, a2o, o2a, /*corrupt_answer_fp=*/true);
  const auto failed = [](const Parsed& e) { return e.kind() == "pc_state" && e.Text("state") == "failed"; };
  const auto connected = [](const Parsed& e) { return e.kind() == "pc_state" && e.Text("state") == "connected"; };
  // Keep the ICE candidates flowing until the caller's connection ends one way or the other.
  const auto end = Clock::now() + std::chrono::seconds(60);
  while (Clock::now() < end) {
    ForwardIce(a, b);
    ForwardIce(b, a);
    if (a.proc.FindEvent(failed) || a.proc.FindEvent(connected)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  Check(a.proc.FindEvent(failed) != nullptr, "call 3 caller: the handshake against a wrong fingerprint fails");
  Check(a.proc.FindEvent(connected) == nullptr, "call 3 caller: the connection never reaches connected");
  std::this_thread::sleep_for(std::chrono::seconds(2));
  Check(a.proc.FindEvent([](const Parsed& e) { return e.kind() == "transport_info"; }) == nullptr,
        "call 3 caller: no transport_info is reported");
  Finish(a, b);
  Check(!a.proc.invalid_message() && !b.proc.invalid_message(), "call 3: every engine message validated against the schema");
}

}  // namespace

int main() {
  char tmp[MAX_PATH];
  if (GetTempPathA(MAX_PATH, tmp) == 0) return 2;
  const std::string dir = std::string(tmp) + "qmedia-call-" + std::to_string(GetCurrentProcessId());
  CreateDirectoryA(dir.c_str(), nullptr);

  // Every wait inside is bounded; this is the backstop for a hang the waits cannot see.
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::minutes(12));
    std::printf("[call] FAILED: the test hung\n");
    std::fflush(stdout);
    std::_Exit(3);
  }).detach();

  Call1(dir);
  Call2(dir);
  Call3(dir);

  if (g_failures != 0) {
    std::printf("[call] FAILED (%d check(s))\n", g_failures);
    return 1;
  }
  std::printf("[call] CALL-OK\n");
  return 0;
}
