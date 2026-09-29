// Node-API binding over the vendored libopus (xiph/opus @ 55513e81, see
// PINS.md), built WITH deep PLC (FARGAN) AND OSCE (LACE/NoLACE).
//
// This file is a derivative of qaudion-desktop's private
// native/opus_addon.cc (deep-PLC only). Two differences from that file:
//   1. BuildInfo() additionally reports `osceCompiled` (compile-time,
//      #ifdef ENABLE_OSCE) so a caller can tell an OSCE build from a
//      deep-PLC-only build without guessing from the version string.
//   2. The Decoder constructor takes an optional `complexity` (default 5,
//      identical to the private repo's hardcoded value) instead of always
//      forcing 5. OSCE only engages at complexity 6 (LACE) or 7 (NoLACE);
//      forcing 5 unconditionally here would compile OSCE in and then never
//      let a caller turn it on. Reset() re-arms whatever complexity the
//      instance was constructed with, not a hardcoded constant — same
//      "silent regression on reset" bug class the private repo's own
//      comment on OPUS_RESET_STATE already flags for complexity 5.
//
// Everything else (surface, PCM convention, ctl passthrough) is unchanged
// from the private repo's file — see that file's own header comment for the
// full rationale (@evan/opus's silent no-op, why ctl is generic, etc.).

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <napi.h>
#include <opus.h>
#include <cstring>
#include <vector>

namespace {

constexpr size_t kMaxPacketBytes = 4000;
constexpr int kMaxFrameSamples = 5760;

const char* OpusErrText(int err) {
  const char* s = opus_strerror(err);
  return s ? s : "unknown opus error";
}

class Encoder : public Napi::ObjectWrap<Encoder> {
 public:
  static Napi::Function Init(Napi::Env env) {
    return DefineClass(env, "Encoder", {
      InstanceMethod("encode", &Encoder::Encode),
      InstanceMethod("ctl", &Encoder::Ctl),
      InstanceMethod("reset", &Encoder::Reset),
    });
  }

  explicit Encoder(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Encoder>(info) {
    Napi::Env env = info.Env();
    int sample_rate = 48000;
    int channels = 1;
    int application = OPUS_APPLICATION_VOIP;

    if (info.Length() > 0 && info[0].IsObject()) {
      Napi::Object o = info[0].As<Napi::Object>();
      if (o.Has("sample_rate")) sample_rate = o.Get("sample_rate").ToNumber().Int32Value();
      if (o.Has("channels")) channels = o.Get("channels").ToNumber().Int32Value();
      if (o.Has("application")) {
        std::string a = o.Get("application").ToString().Utf8Value();
        if (a == "audio") application = OPUS_APPLICATION_AUDIO;
        else if (a == "restricted_lowdelay") application = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
        else application = OPUS_APPLICATION_VOIP;
      }
    }

    int err = OPUS_OK;
    enc_ = opus_encoder_create(sample_rate, channels, application, &err);
    if (err != OPUS_OK || enc_ == nullptr) {
      Napi::Error::New(env, std::string("opus_encoder_create: ") + OpusErrText(err))
          .ThrowAsJavaScriptException();
      return;
    }
    channels_ = channels;
  }

  ~Encoder() override {
    if (enc_) { opus_encoder_destroy(enc_); enc_ = nullptr; }
  }

 private:
  Napi::Value Encode(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!enc_) {
      Napi::Error::New(env, "encoder is destroyed").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (info.Length() < 1 || !info[0].IsTypedArray()) {
      Napi::TypeError::New(env, "encode(pcm): expected a TypedArray of Int16 samples")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::TypedArray ta = info[0].As<Napi::TypedArray>();
    const opus_int16* pcm =
        reinterpret_cast<const opus_int16*>(static_cast<const uint8_t*>(ta.ArrayBuffer().Data()) + ta.ByteOffset());
    const int frame_size = static_cast<int>(ta.ByteLength() / sizeof(opus_int16) / channels_);

    std::vector<unsigned char> out(kMaxPacketBytes);
    const int n = opus_encode(enc_, pcm, frame_size, out.data(), static_cast<opus_int32>(out.size()));
    if (n < 0) {
      Napi::Error::New(env, std::string("opus_encode: ") + OpusErrText(n))
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    return Napi::Buffer<uint8_t>::Copy(env, out.data(), static_cast<size_t>(n));
  }

  Napi::Value Ctl(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!enc_) {
      Napi::Error::New(env, "encoder is destroyed").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    const int request = info[0].ToNumber().Int32Value();
    if (info.Length() >= 2 && !info[1].IsUndefined() && !info[1].IsNull()) {
      const opus_int32 value = info[1].ToNumber().Int32Value();
      const int err = opus_encoder_ctl(enc_, request, value);
      return Napi::Number::New(env, err);
    }
    opus_int32 value = 0;
    const int err = opus_encoder_ctl(enc_, request, &value);
    if (err != OPUS_OK) {
      Napi::Error::New(env, std::string("opus_encoder_ctl get: ") + OpusErrText(err))
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    return Napi::Number::New(env, value);
  }

  Napi::Value Reset(const Napi::CallbackInfo& info) {
    if (enc_) opus_encoder_ctl(enc_, OPUS_RESET_STATE);
    return info.Env().Undefined();
  }

  OpusEncoder* enc_ = nullptr;
  int channels_ = 1;
};

// Clamp to libopus's own valid complexity range so a bad caller value cannot
// reach opus_decoder_ctl as an out-of-range request (OPUS_BAD_ARG either way,
// but clamped here reads as "which complexity did you mean" instead of a ctl
// error from three layers down).
int ClampComplexity(int v) {
  if (v < 0) return 0;
  if (v > 10) return 10;
  return v;
}

class Decoder : public Napi::ObjectWrap<Decoder> {
 public:
  static Napi::Function Init(Napi::Env env) {
    return DefineClass(env, "Decoder", {
      InstanceMethod("decode", &Decoder::Decode),
      InstanceMethod("decodePlc", &Decoder::DecodePlc),
      InstanceMethod("decodeFec", &Decoder::DecodeFec),
      InstanceMethod("ctl", &Decoder::Ctl),
      InstanceMethod("reset", &Decoder::Reset),
    });
  }

  explicit Decoder(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Decoder>(info) {
    Napi::Env env = info.Env();
    int sample_rate = 48000;
    int channels = 1;
    // Default 5 — deep PLC only, identical to the private repo's hardcoded
    // value, so a caller that does not know about OSCE gets the exact same
    // behavior it would from that addon. A caller that DOES know can pass
    // 6 (LACE) or 7 (NoLACE) to engage OSCE.
    int complexity = 5;
    if (info.Length() > 0 && info[0].IsObject()) {
      Napi::Object o = info[0].As<Napi::Object>();
      if (o.Has("sample_rate")) sample_rate = o.Get("sample_rate").ToNumber().Int32Value();
      if (o.Has("channels")) channels = o.Get("channels").ToNumber().Int32Value();
      if (o.Has("complexity")) complexity = o.Get("complexity").ToNumber().Int32Value();
    }

    int err = OPUS_OK;
    dec_ = opus_decoder_create(sample_rate, channels, &err);
    if (err != OPUS_OK || dec_ == nullptr) {
      Napi::Error::New(env, std::string("opus_decoder_create: ") + OpusErrText(err))
          .ThrowAsJavaScriptException();
      return;
    }
    channels_ = channels;
    initial_complexity_ = ClampComplexity(complexity);

    // Failure is remembered, not thrown — see the private repo's identical
    // rationale. `deepPlcEnabled`/complexity readback surfaces the outcome.
    deep_plc_err_ = opus_decoder_ctl(dec_, OPUS_SET_COMPLEXITY(initial_complexity_));
  }

  ~Decoder() override {
    if (dec_) { opus_decoder_destroy(dec_); dec_ = nullptr; }
  }

 private:
  Napi::Value Decode(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!dec_) {
      Napi::Error::New(env, "decoder is destroyed").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (info.Length() < 1 || !info[0].IsTypedArray()) {
      Napi::TypeError::New(env, "decode(packet): expected a TypedArray")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::TypedArray ta = info[0].As<Napi::TypedArray>();
    const unsigned char* data =
        static_cast<const uint8_t*>(ta.ArrayBuffer().Data()) + ta.ByteOffset();
    const opus_int32 len = static_cast<opus_int32>(ta.ByteLength());

    std::vector<opus_int16> pcm(static_cast<size_t>(kMaxFrameSamples) * channels_);
    const int samples = opus_decode(dec_, data, len, pcm.data(), kMaxFrameSamples, 0);
    if (samples < 0) {
      Napi::Error::New(env, std::string("opus_decode: ") + OpusErrText(samples))
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    return Napi::Buffer<uint8_t>::Copy(
        env, reinterpret_cast<const uint8_t*>(pcm.data()),
        static_cast<size_t>(samples) * channels_ * sizeof(opus_int16));
  }

  Napi::Value DecodePlc(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!dec_) {
      Napi::Error::New(env, "decoder is destroyed").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    int frame_size = 960;
    if (info.Length() >= 1 && info[0].IsNumber()) {
      frame_size = info[0].As<Napi::Number>().Int32Value();
    }
    if (frame_size <= 0 || frame_size > kMaxFrameSamples) {
      Napi::RangeError::New(env, "decodePlc(frameSize): out of range")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    std::vector<opus_int16> pcm(static_cast<size_t>(frame_size) * channels_);
    const int samples = opus_decode(dec_, nullptr, 0, pcm.data(), frame_size, 0);
    if (samples < 0) {
      Napi::Error::New(env, std::string("opus_decode(PLC): ") + OpusErrText(samples))
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    return Napi::Buffer<uint8_t>::Copy(
        env, reinterpret_cast<const uint8_t*>(pcm.data()),
        static_cast<size_t>(samples) * channels_ * sizeof(opus_int16));
  }

  Napi::Value DecodeFec(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!dec_) {
      Napi::Error::New(env, "decoder is destroyed").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (info.Length() < 1 || !info[0].IsTypedArray()) {
      Napi::TypeError::New(env, "decodeFec(packet, frameSize): expected a TypedArray")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::TypedArray ta = info[0].As<Napi::TypedArray>();
    const unsigned char* data =
        static_cast<const uint8_t*>(ta.ArrayBuffer().Data()) + ta.ByteOffset();
    const opus_int32 len = static_cast<opus_int32>(ta.ByteLength());

    int frame_size = 960;
    if (info.Length() >= 2 && info[1].IsNumber()) {
      frame_size = info[1].As<Napi::Number>().Int32Value();
    }
    if (frame_size <= 0 || frame_size > kMaxFrameSamples) {
      Napi::RangeError::New(env, "decodeFec(packet, frameSize): frameSize out of range")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    std::vector<opus_int16> pcm(static_cast<size_t>(frame_size) * channels_);
    const int samples = opus_decode(dec_, data, len, pcm.data(), frame_size, 1);
    if (samples < 0) {
      Napi::Error::New(env, std::string("opus_decode(FEC): ") + OpusErrText(samples))
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    return Napi::Buffer<uint8_t>::Copy(
        env, reinterpret_cast<const uint8_t*>(pcm.data()),
        static_cast<size_t>(samples) * channels_ * sizeof(opus_int16));
  }

  Napi::Value Ctl(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!dec_) {
      Napi::Error::New(env, "decoder is destroyed").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    const int request = info[0].ToNumber().Int32Value();
    if (info.Length() >= 2 && !info[1].IsUndefined() && !info[1].IsNull()) {
      const opus_int32 value = info[1].ToNumber().Int32Value();
      return Napi::Number::New(env, opus_decoder_ctl(dec_, request, value));
    }
    opus_int32 value = 0;
    const int err = opus_decoder_ctl(dec_, request, &value);
    if (err != OPUS_OK) {
      Napi::Error::New(env, std::string("opus_decoder_ctl get: ") + OpusErrText(err))
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    return Napi::Number::New(env, value);
  }

  Napi::Value Reset(const Napi::CallbackInfo& info) {
    if (dec_) {
      opus_decoder_ctl(dec_, OPUS_RESET_STATE);
      // Re-arm the complexity this instance was CONSTRUCTED with, not a
      // hardcoded constant — a caller running at complexity 7 (NoLACE) whose
      // decoder resets mid-call must not silently drop back to plain deep
      // PLC. See the file header for why this differs from the private
      // repo's Reset(), which only ever had one value to restore.
      deep_plc_err_ = opus_decoder_ctl(dec_, OPUS_SET_COMPLEXITY(initial_complexity_));
    }
    return info.Env().Undefined();
  }

 public:
  int deep_plc_err_ = OPUS_OK;

 private:
  OpusDecoder* dec_ = nullptr;
  int channels_ = 1;
  int initial_complexity_ = 5;
};

// Reports what the BINARY can do, not what the source says it should — same
// philosophy as the private repo's BuildInfo(), extended with `osceCompiled`.
Napi::Value BuildInfo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object o = Napi::Object::New(env);
  o.Set("version", Napi::String::New(env, opus_get_version_string()));
#ifdef ENABLE_DEEP_PLC
  o.Set("deepPlcCompiled", Napi::Boolean::New(env, true));
#else
  o.Set("deepPlcCompiled", Napi::Boolean::New(env, false));
#endif
#ifdef ENABLE_OSCE
  o.Set("osceCompiled", Napi::Boolean::New(env, true));
#else
  o.Set("osceCompiled", Napi::Boolean::New(env, false));
#endif

  int err = OPUS_OK;
  OpusDecoder* d = opus_decoder_create(48000, 1, &err);
  bool accepts5 = false, accepts6 = false, accepts7 = false;
  if (err == OPUS_OK && d) {
    accepts5 = (opus_decoder_ctl(d, OPUS_SET_COMPLEXITY(5)) == OPUS_OK);
    accepts6 = (opus_decoder_ctl(d, OPUS_SET_COMPLEXITY(6)) == OPUS_OK);
    accepts7 = (opus_decoder_ctl(d, OPUS_SET_COMPLEXITY(7)) == OPUS_OK);
    opus_decoder_destroy(d);
  }
  o.Set("decoderAcceptsComplexity", Napi::Boolean::New(env, accepts5));
  // Accepting the ctl call is necessary but not sufficient proof OSCE ran —
  // libopus accepts 6/7 as plain valid complexity values even without OSCE
  // compiled (it just runs classic PLC at that complexity). osceCompiled
  // (compile-time) is the authoritative signal; this is a live sanity check
  // that the two do not disagree.
  o.Set("decoderAcceptsOsceComplexity", Napi::Boolean::New(env, accepts6 && accepts7));
  return o;
}

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
  exports.Set("Encoder", Encoder::Init(env));
  exports.Set("Decoder", Decoder::Init(env));
  exports.Set("buildInfo", Napi::Function::New(env, BuildInfo));
  return exports;
}

}  // namespace

NODE_API_MODULE(qaudion_opus, InitAll)
