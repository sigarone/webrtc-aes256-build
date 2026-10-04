// hevc-probe: a small Windows console tool that reports what the machine can do
// with HEVC (H.265) or H.264 through Media Foundation.
//
// It lists the encoder and decoder MFTs (hardware / software, async / sync,
// vendor, friendly name), the DXGI adapters, whether the HEVC Video Extensions
// are registered for the current user, and then runs real encode and decode tests
// on synthetic NV12 frames (1280x720 and 1920x1080 at 30 fps). The result is one
// JSON document, written to stdout and to a file next to the executable.
//
// Every attempt is independent: it enumerates and activates the MFT afresh,
// creates its own D3D11 device and DXGI device manager, and tears all of it down
// (MFT shut down through its activation object, every COM object released, the
// device flushed) before the next attempt starts. Results are tri-state: "ok",
// "failed" or "not_attempted" (with the reason); an attempt that was not made is
// never reported as a failure.
//
// Privacy: the report carries no user name, machine name, serial numbers, MAC or
// IP addresses and no file system paths. Adapter and MFT names are product names
// only.
//
// This is a generic diagnostic. It contains no application protocol logic.

#include <windows.h>

#include <fcntl.h>
#include <intrin.h>
#include <io.h>

#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef PROBE_GIT_SHA
#define PROBE_GIT_SHA "unknown"
#endif

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kFps = 30;
constexpr int kFrames = 60;
constexpr DWORD kTestTimeoutMs = 45000;
// Pause after the teardown of an attempt, so that a driver that releases its
// hardware session asynchronously has finished before the next attempt starts.
constexpr DWORD kSettleMs = 300;
constexpr UINT32 kH265Main420x8 = 1;  // eAVEncH265VProfile_Main_420_8
// Upper bound for the whole run. The per-test watchdog does not cover the
// start-up enumeration (DXGI and MFTEnumEx load vendor user-mode drivers), so a
// stuck driver there would otherwise leave the process hanging forever.
constexpr DWORD kHardLimitMs = 30 * 60 * 1000;
std::atomic<bool> g_run_finished{false};
// Attempts whose thread did not come back within the watchdog time. Such a thread
// may still hold its MFT instance, which can disturb later attempts.
std::atomic<int> g_hung_attempts{0};

// The codec under test. HEVC is the point of the probe; "--codec h264" runs the
// very same pipeline on H.264 and is used by CI as a self-test of the encode and
// decode machinery (the hosted runner has no HEVC MFT at all).
GUID g_subtype = MFVideoFormat_HEVC;
bool g_is_hevc = true;
const char* g_codec_name = "hevc";

// Lets the D3D11 attempts fall back to a software adapter (WARP) when the machine
// has no GPU. CI uses it to run the D3D11 code path on a hosted runner.
bool g_allow_sw_adapter = false;

// ---------------------------------------------------------------- helpers

std::string Hex32(unsigned long v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08lX", v);
  return b;
}

std::string HrStr(HRESULT hr) { return Hex32(static_cast<unsigned long>(hr)); }

std::string Utf8(const wchar_t* w, int len = -1) {
  if (!w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

std::string GuidStr(const GUID& g) {
  char b[48];
  std::snprintf(b, sizeof b, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
                g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return b;
}

// ---------------------------------------------------------------- JSON

class Json {
 public:
  void BeginObject() { Value(); out_ += '{'; first_.push_back(true); }
  void EndObject() { Close('}'); }
  void BeginArray() { Value(); out_ += '['; first_.push_back(true); }
  void EndArray() { Close(']'); }
  void Key(const char* k) { Sep(); Quote(k); out_ += ": "; after_key_ = true; }
  void Str(const std::string& v) { Value(); Quote(v); }
  void Bool(bool v) { Value(); out_ += v ? "true" : "false"; }
  void Int(long long v) { Value(); out_ += std::to_string(v); }
  void Num(double v) {
    Value();
    char b[64];
    std::snprintf(b, sizeof b, "%.2f", v);
    out_ += b;
  }
  void Null() { Value(); out_ += "null"; }
  void KvS(const char* k, const std::string& v) { Key(k); Str(v); }
  void KvB(const char* k, bool v) { Key(k); Bool(v); }
  void KvI(const char* k, long long v) { Key(k); Int(v); }
  void KvN(const char* k, double v) { Key(k); Num(v); }
  const std::string& Text() const { return out_; }

 private:
  void Indent() { out_ += '\n'; out_.append(first_.size() * 2, ' '); }
  void Sep() {
    if (first_.empty()) return;
    if (!first_.back()) out_ += ',';
    first_.back() = false;
    Indent();
  }
  void Value() {
    if (after_key_) {
      after_key_ = false;
    } else {
      Sep();
    }
  }
  void Close(char c) {
    bool empty = first_.back();
    first_.pop_back();
    if (!empty) Indent();
    out_ += c;
  }
  void Quote(const std::string& s) {
    out_ += '"';
    for (unsigned char c : s) {
      switch (c) {
        case '"': out_ += "\\\""; break;
        case '\\': out_ += "\\\\"; break;
        case '\n': out_ += "\\n"; break;
        case '\r': out_ += "\\r"; break;
        case '\t': out_ += "\\t"; break;
        default:
          if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04X", c);
            out_ += b;
          } else {
            out_ += static_cast<char>(c);
          }
      }
    }
    out_ += '"';
  }
  std::string out_;
  std::vector<bool> first_;
  bool after_key_ = false;
};

// ---------------------------------------------------------------- DXGI

struct AdapterInfo {
  std::string description;
  unsigned vendor_id = 0;
  unsigned device_id = 0;
  unsigned long long dedicated_mb = 0;
  unsigned long long shared_mb = 0;
  bool software = false;
  std::string driver_version;
};

struct AdapterEntry {
  ComPtr<IDXGIAdapter1> adapter;
  AdapterInfo info;
};

std::vector<AdapterInfo> g_adapters;

// A fresh DXGI factory on every call: a factory that was created before a GPU
// changed its power state (hybrid laptops) can report a stale adapter list.
std::vector<AdapterEntry> EnumAdapters() {
  std::vector<AdapterEntry> out;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    HRESULT hr = factory->EnumAdapters1(i, &a);
    if (FAILED(hr)) break;  // DXGI_ERROR_NOT_FOUND ends the list
    DXGI_ADAPTER_DESC1 d{};
    if (FAILED(a->GetDesc1(&d))) continue;
    AdapterEntry e;
    e.info.description = Utf8(d.Description);
    e.info.vendor_id = d.VendorId;
    e.info.device_id = d.DeviceId;
    e.info.dedicated_mb = d.DedicatedVideoMemory / (1024ull * 1024ull);
    e.info.shared_mb = d.SharedSystemMemory / (1024ull * 1024ull);
    e.info.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    LARGE_INTEGER umd{};
    if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
      char b[48];
      std::snprintf(b, sizeof b, "%u.%u.%u.%u", HIWORD(umd.HighPart),
                    LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                    LOWORD(umd.LowPart));
      e.info.driver_version = b;
    }
    e.adapter = a;
    out.push_back(std::move(e));
  }
  return out;
}

int CountHardwareAdapters() {
  int n = 0;
  for (const AdapterInfo& a : g_adapters) {
    if (!a.software) ++n;
  }
  return n;
}

// A D3D11 device made for one attempt, with the adapter it lives on.
struct D3dDev {
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  AdapterInfo adapter;
  bool vendor_matched = false;  // the adapter belongs to the vendor of the MFT
  bool video_device = false;    // the device exposes ID3D11VideoDevice
  bool video_flag_dropped = false;  // software adapter only: made without VIDEO_SUPPORT
  std::string fallback_note;        // --ci only: why the WARP software rasterizer was used
};

// Explicit adapter choice. ordinal >= 0: the n-th non-software adapter. Otherwise
// the first non-software adapter whose vendor id matches the MFT vendor id, then
// the first non-software adapter. A software adapter is only used when
// --allow-software-adapter (or --ci) was given.
bool MakeD3DDevice(unsigned vendor_id, int ordinal, D3dDev* out, HRESULT* hr_out) {
  std::vector<AdapterEntry> list = EnumAdapters();
  const AdapterEntry* pick = nullptr;
  if (ordinal >= 0) {
    int idx = 0;
    for (const AdapterEntry& e : list) {
      if (e.info.software) continue;
      if (idx++ == ordinal) {
        pick = &e;
        break;
      }
    }
  } else {
    for (const AdapterEntry& e : list) {
      if (!e.info.software && vendor_id && e.info.vendor_id == vendor_id) {
        pick = &e;
        break;
      }
    }
    if (!pick) {
      for (const AdapterEntry& e : list) {
        if (!e.info.software) {
          pick = &e;
          break;
        }
      }
    }
  }
  if (!pick && g_allow_sw_adapter) {
    for (const AdapterEntry& e : list) {
      if (e.info.software) {
        pick = &e;
        break;
      }
    }
  }
  if (!pick) {
    *hr_out = DXGI_ERROR_NOT_FOUND;
    return false;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got{};
  AdapterInfo used = pick->info;
  HRESULT hr = D3D11CreateDevice(
      pick->adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
      levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &out->dev, &got, &out->ctx);
  const HRESULT first_hr = hr;
  if (FAILED(hr) && pick->info.software) {
    // A software adapter (WARP) is only used to run the code path on a machine
    // without a GPU, and it does not offer video support everywhere. A hardware
    // adapter is never retried without it: video support is the point.
    out->dev.Reset();
    out->ctx.Reset();
    hr = D3D11CreateDevice(pick->adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                           D3D11_SDK_VERSION, &out->dev, &got, &out->ctx);
    if (SUCCEEDED(hr)) out->video_flag_dropped = true;
  }
  if (FAILED(hr) && g_allow_sw_adapter) {
    // Virtual machines list display adapters that cannot make a D3D11 device at all.
    // With --ci the code path still has to run, so the WARP software rasterizer is
    // used, and the report says so.
    out->dev.Reset();
    out->ctx.Reset();
    HRESULT wh = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &out->dev, &got,
                                   &out->ctx);
    bool dropped = false;
    if (FAILED(wh)) {
      out->dev.Reset();
      out->ctx.Reset();
      wh = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                             levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &out->dev, &got, &out->ctx);
      dropped = SUCCEEDED(wh);
    }
    if (SUCCEEDED(wh)) {
      out->fallback_note = "the D3D11 device could not be made on '" + pick->info.description +
                           "' (" + HrStr(first_hr) + "); the WARP software rasterizer was used instead (--ci)";
      out->video_flag_dropped = dropped;
      used = AdapterInfo();
      used.description = "WARP software rasterizer";
      used.vendor_id = 0x1414;
      used.software = true;
      hr = S_OK;
    }
  }
  *hr_out = hr;
  if (FAILED(hr)) return false;
  // MFTs call into the device from their own threads.
  ComPtr<ID3D11Multithread> mt;
  if (SUCCEEDED(out->dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
  ComPtr<ID3D11VideoDevice> vd;
  out->video_device = SUCCEEDED(out->dev.As(&vd));
  out->adapter = used;
  out->vendor_matched = vendor_id == 0 || used.vendor_id == vendor_id;
  return true;
}

// ---------------------------------------------------------------- MFT list

struct MftInfo {
  std::string name;
  std::string vendor;       // raw vendor id string, e.g. "VEN_10DE"
  unsigned vendor_id = 0;   // parsed from the string, 0 if unknown
  std::string clsid;
  GUID clsid_guid{};
  bool hardware = false;
  bool async = false;
  bool sync = false;
  UINT32 flags = 0;
  // Where it was found, so that every attempt can enumerate it again.
  GUID category{};
  GUID subtype{};
  bool subtype_is_output = false;
};

unsigned ParseVendorId(const std::string& s) {
  size_t p = s.find("VEN_");
  if (p == std::string::npos) return 0;
  return static_cast<unsigned>(std::strtoul(s.c_str() + p + 4, nullptr, 16));
}

std::vector<MftInfo> EnumMfts(const GUID& category, const GUID& subtype,
                              bool subtype_is_output, std::map<std::string, int>* counts) {
  struct Q {
    const char* label;
    UINT32 flags;
  };
  const Q queries[] = {
      {"hardware", MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER},
      {"async", MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER},
      {"sync", MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER},
  };
  std::vector<MftInfo> list;
  std::map<std::string, size_t> by_clsid;
  MFT_REGISTER_TYPE_INFO ti{MFMediaType_Video, subtype};
  for (const Q& q : queries) {
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFTEnumEx(category, q.flags, subtype_is_output ? nullptr : &ti,
                           subtype_is_output ? &ti : nullptr, &acts, &n);
    if (counts) (*counts)[q.label] = SUCCEEDED(hr) ? static_cast<int>(n) : -1;
    if (FAILED(hr)) continue;
    for (UINT32 i = 0; i < n; ++i) {
      // The activation objects are only read here and released; every test
      // enumerates again and gets an activation object of its own.
      ComPtr<IMFActivate> act;
      act.Attach(acts[i]);
      GUID clsid{};
      act->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
      std::string key = GuidStr(clsid);
      size_t idx;
      auto it = by_clsid.find(key);
      if (it == by_clsid.end()) {
        MftInfo mi;
        mi.clsid = key;
        mi.clsid_guid = clsid;
        mi.category = category;
        mi.subtype = subtype;
        mi.subtype_is_output = subtype_is_output;
        WCHAR* w = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &w, &len))) {
          mi.name = Utf8(w);
          CoTaskMemFree(w);
        }
        w = nullptr;
        if (SUCCEEDED(act->GetAllocatedString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &w, &len))) {
          mi.vendor = Utf8(w);
          mi.vendor_id = ParseVendorId(mi.vendor);
          CoTaskMemFree(w);
        }
        UINT32 f = 0;
        if (SUCCEEDED(act->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &f))) mi.flags = f;
        idx = list.size();
        by_clsid[key] = idx;
        list.push_back(std::move(mi));
      } else {
        idx = it->second;
      }
      MftInfo& m = list[idx];
      if (std::string(q.label) == "hardware") m.hardware = true;
      if (std::string(q.label) == "async") m.async = true;
      if (std::string(q.label) == "sync") m.sync = true;
    }
    CoTaskMemFree(acts);
  }
  for (MftInfo& m : list) {
    if (m.flags & MFT_ENUM_FLAG_HARDWARE) m.hardware = true;
    if (m.flags & MFT_ENUM_FLAG_ASYNCMFT) m.async = true;
    if (m.flags & MFT_ENUM_FLAG_SYNCMFT) m.sync = true;
  }
  return list;
}

// A new activation object for the MFT, from a new enumeration, in the same query
// order as EnumMfts (so that a hardware MFT keeps the hardware binding of its
// first query). Never reuses an activation object of an earlier attempt.
ComPtr<IMFActivate> FreshActivate(const MftInfo& mi, HRESULT* hr_out) {
  const UINT32 flag_sets[] = {MFT_ENUM_FLAG_HARDWARE, MFT_ENUM_FLAG_ASYNCMFT,
                              MFT_ENUM_FLAG_SYNCMFT};
  MFT_REGISTER_TYPE_INFO ti{MFMediaType_Video, mi.subtype};
  HRESULT last = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  ComPtr<IMFActivate> found;
  for (UINT32 f : flag_sets) {
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFTEnumEx(mi.category, f | MFT_ENUM_FLAG_SORTANDFILTER,
                           mi.subtype_is_output ? nullptr : &ti,
                           mi.subtype_is_output ? &ti : nullptr, &acts, &n);
    if (FAILED(hr)) {
      last = hr;
      continue;
    }
    for (UINT32 i = 0; i < n; ++i) {
      ComPtr<IMFActivate> a;
      a.Attach(acts[i]);
      if (found) continue;
      GUID c{};
      if (SUCCEEDED(a->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &c)) && c == mi.clsid_guid) {
        found = a;
      }
    }
    CoTaskMemFree(acts);
    if (found) break;
  }
  *hr_out = found ? S_OK : last;
  return found;
}

// ---------------------------------------------------------------- results

enum class Outcome { kNotAttempted, kOk, kFailed };

const char* OutcomeStr(Outcome o) {
  switch (o) {
    case Outcome::kOk: return "ok";
    case Outcome::kFailed: return "failed";
    default: return "not_attempted";
  }
}

struct StepRec {
  std::string stage;
  HRESULT hr = S_OK;
};

struct TestResult {
  std::string role;       // "encode" | "decode"
  std::string kind;       // "full" | "configure_only"
  std::string mft;
  bool hardware = false;
  int width = 0;
  int height = 0;
  std::string memory;     // requested: "d3d11" | "system"
  bool use_d3d = false;   // a D3D11 device manager was set on the MFT
  std::string mode;       // "async" | "sync" (empty when the MFT was never reached)
  Outcome outcome = Outcome::kFailed;
  std::string stage;      // where it stopped on failure
  HRESULT hr = S_OK;
  std::string error;
  std::string reason_code;  // not_attempted: short code
  std::string reason;       // not_attempted: why
  std::string hint;         // hybrid GPU hint
  int frames_in = 0;
  int frames_out = 0;
  unsigned long long bytes = 0;
  double seconds = 0;
  double fps = 0;
  // adapter used for the D3D11 device
  bool adapter_known = false;
  std::string adapter;
  unsigned adapter_vendor = 0;
  bool adapter_software = false;
  bool adapter_same_vendor = false;
  int d3d11_aware = -1;        // MF_SA_D3D11_AWARE of the MFT: 1, 0, or -1 unknown
  int output_textures = 0;     // decode: frames delivered as D3D11 textures
  bool gpu_path = false;       // decode ok, D3D11 textures out, real (non-software) adapter
  int residual_refs = -1;      // references left on the MFT after the last release
  std::string stream_source;   // decode: where the bitstream came from
  std::vector<StepRec> steps;
  std::vector<std::string> notes;

  void Step(const std::string& st, HRESULT h) { steps.push_back(StepRec{st, h}); }
  void Fail(const std::string& st, HRESULT h) {
    outcome = Outcome::kFailed;
    stage = st;
    hr = h;
  }
  // Records the step and fails the attempt when it did not succeed.
  bool Check(const std::string& st, HRESULT h) {
    Step(st, h);
    if (FAILED(h)) {
      Fail(st, h);
      return false;
    }
    return true;
  }
  void Skip(const std::string& code, const std::string& why) {
    outcome = Outcome::kNotAttempted;
    reason_code = code;
    reason = why;
  }
};

struct EncFrame {
  std::vector<uint8_t> data;
  LONGLONG time = 0;
};

struct EncodeOut {
  TestResult r;
  std::vector<EncFrame> frames;
  std::vector<uint8_t> seq_header;
};

// ---------------------------------------------------------------- the session

// Everything one attempt owns. Teardown runs in the order that lets a hardware
// MFT release its session: stop streaming, give the D3D manager back, shut the
// object down through the activation object, release the last reference, then
// the DXGI manager and the device.
struct Session {
  ComPtr<IMFActivate> act;
  ComPtr<IMFTransform> mft;
  ComPtr<IMFMediaEventGenerator> gen;
  ComPtr<ICodecAPI> api;
  D3dDev d3d;
  ComPtr<IMFDXGIDeviceManager> mgr;
  bool d3d_aware = false;
  bool manager_set = false;
  bool streaming = false;
  bool clean_end = false;
  bool torn_down = false;

  ~Session() { Teardown(nullptr); }

  void Teardown(TestResult* r) {
    if (torn_down) return;
    torn_down = true;
    if (mft) {
      if (streaming) {
        if (!clean_end) mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
      }
      if (manager_set) {
        HRESULT hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        if (r) r->Step("release_d3d_manager", hr);
      }
    }
    api.Reset();
    gen.Reset();
    if (act) {
      HRESULT hr = act->ShutdownObject();
      if (r) r->Step("shutdown_object", hr);
      act.Reset();
    }
    if (mft) {
      IUnknown* raw = mft.Detach();
      ULONG left = raw->Release();
      if (r) {
        r->residual_refs = static_cast<int>(left);
        if (left != 0) {
          r->notes.push_back("the MFT still has " + std::to_string(left) +
                             " reference(s) after every release (possible leaked instance)");
        }
      }
    }
    mgr.Reset();
    if (d3d.ctx) {
      d3d.ctx->ClearState();
      d3d.ctx->Flush();
    }
    d3d.ctx.Reset();
    d3d.dev.Reset();
  }
};

// Hybrid-GPU hint: the MFT belongs to a GPU that is not the first adapter (the
// one Windows drives the display with on a laptop), or the D3D11 device had to be
// made on an adapter of another vendor.
std::string HybridHint(const MftInfo& mi, const D3dDev* d) {
  if (mi.vendor_id == 0) return {};
  int hw = 0;
  unsigned first_vendor = 0;
  for (const AdapterInfo& a : g_adapters) {
    if (a.software) continue;
    if (hw++ == 0) first_vendor = a.vendor_id;
  }
  if (hw < 2) return {};
  const bool device_mismatch = d && d->dev && !d->vendor_matched;
  if (mi.vendor_id == first_vendor && !device_mismatch) return {};
  std::string s = "hybrid GPU hint: this machine has " + std::to_string(hw) +
                  " hardware adapters and the MFT belongs to vendor " + mi.vendor + ". ";
  if (d && d->dev) {
    s += "The probe created the D3D11 device on '" + d->adapter.description + "' (" +
         (d->vendor_matched ? "same vendor as the MFT" : "a different vendor than the MFT") + "). ";
  } else {
    s += "No D3D11 device was involved at that point. ";
  }
  s += "The vendor MFT of a secondary GPU (usually the discrete GPU of a laptop) may only work "
       "when the process runs on that GPU (Windows Settings, System, Display, Graphics: set this "
       "exe to High performance) or may not be usable by this process at all. This is reported, "
       "not treated as a probe failure.";
  return s;
}

// ---------------------------------------------------------------- the pump

// Drives one MFT (async event loop or sync loop), feeding `total` inputs and
// collecting outputs until the drain completes.
class Pump {
 public:
  IMFTransform* mft = nullptr;
  IMFMediaEventGenerator* gen = nullptr;  // non-null for async MFTs
  int total = 0;
  ULONGLONG timeout_ms = 30000;
  DWORD default_out_size = 0;
  std::function<HRESULT(int, IMFSample**)> make_input;
  std::function<void(IMFSample*)> on_output;
  std::function<HRESULT()> renegotiate_output;
  int fed = 0;
  int produced = 0;
  int ev_need_input = 0;
  int ev_have_output = 0;
  int ev_other = 0;
  HRESULT hr = S_OK;
  std::string stage;
  double seconds = 0;

  bool Run() {
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    bool started = false;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    auto start = [&] {
      if (!started) {
        QueryPerformanceCounter(&t0);
        started = true;
      }
    };
    auto stop = [&] {
      QueryPerformanceCounter(&t1);
      if (started) seconds = double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart);
    };

    if (gen) {
      bool drain_sent = false;
      for (;;) {
        if (GetTickCount64() > deadline) {
          stage = "event_loop_timeout";
          hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
          return false;
        }
        ComPtr<IMFMediaEvent> ev;
        HRESULT e = gen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (e == MF_E_NO_EVENTS_AVAILABLE) {
          Sleep(1);
          continue;
        }
        if (FAILED(e)) {
          stage = "get_event";
          hr = e;
          return false;
        }
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        HRESULT st = S_OK;
        ev->GetStatus(&st);
        if (FAILED(st)) {
          stage = "event_status";
          hr = st;
          return false;
        }
        switch (type) {
          case METransformNeedInput: {
            ++ev_need_input;
            if (fed < total) {
              start();
              HRESULT r = FeedOne();
              if (FAILED(r)) {
                stage = "process_input";
                hr = r;
                return false;
              }
            }
            if (fed >= total && !drain_sent) {
              drain_sent = true;
              HRESULT r = mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
              if (FAILED(r)) {
                stage = "drain";
                hr = r;
                return false;
              }
            }
            break;
          }
          case METransformHaveOutput: {
            ++ev_have_output;
            bool more = false;
            HRESULT r = PullOne(&more);
            if (FAILED(r)) {
              stage = "process_output";
              hr = r;
              return false;
            }
            break;
          }
          case METransformDrainComplete:
            stop();
            return true;
          case MEError:
            stage = "media_event_error";
            hr = E_FAIL;
            return false;
          default:
            ++ev_other;
            break;
        }
      }
    }

    // Synchronous MFT.
    auto drain_outputs = [&]() -> bool {
      for (;;) {
        if (GetTickCount64() > deadline) {
          stage = "sync_loop_timeout";
          hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
          return false;
        }
        bool need_more = false;
        HRESULT r = PullOne(&need_more);
        if (FAILED(r)) {
          stage = "process_output";
          hr = r;
          return false;
        }
        if (need_more) return true;
      }
    };
    for (int i = 0; i < total; ++i) {
      start();
      HRESULT r = FeedOne();
      if (r == MF_E_NOTACCEPTING) {
        if (!drain_outputs()) return false;
        r = FeedOne();
      }
      if (FAILED(r)) {
        stage = "process_input";
        hr = r;
        return false;
      }
      if (!drain_outputs()) return false;
    }
    HRESULT r = mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(r)) {
      stage = "drain";
      hr = r;
      return false;
    }
    if (!drain_outputs()) return false;
    stop();
    return true;
  }

 private:
  HRESULT FeedOne() {
    ComPtr<IMFSample> s;
    HRESULT r = make_input(fed, &s);
    if (FAILED(r)) return r;
    r = mft->ProcessInput(0, s.Get(), 0);
    if (SUCCEEDED(r)) ++fed;
    return r;
  }

  HRESULT PullOne(bool* need_more) {
    *need_more = false;
    MFT_OUTPUT_STREAM_INFO osi{};
    HRESULT hr0 = mft->GetOutputStreamInfo(0, &osi);
    const bool provides =
        SUCCEEDED(hr0) && (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    ComPtr<IMFSample> own;
    if (!provides) {
      DWORD size = (SUCCEEDED(hr0) && osi.cbSize) ? osi.cbSize : default_out_size;
      if (size == 0) size = 1 << 20;
      ComPtr<IMFMediaBuffer> b;
      HRESULT r = MFCreateAlignedMemoryBuffer(
          size, (SUCCEEDED(hr0) && osi.cbAlignment > 1) ? osi.cbAlignment - 1 : 0, &b);
      if (FAILED(r)) return r;
      r = MFCreateSample(&own);
      if (FAILED(r)) return r;
      own->AddBuffer(b.Get());
    }
    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = 0;
    ob.pSample = own.Get();
    DWORD status = 0;
    HRESULT r = mft->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) ob.pEvents->Release();
    if (SUCCEEDED(r)) {
      if (ob.pSample) {
        on_output(ob.pSample);
        ++produced;
        if (provides) ob.pSample->Release();
      }
      return S_OK;
    }
    if (provides && ob.pSample) ob.pSample->Release();
    if (r == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      *need_more = true;
      return S_OK;
    }
    if (r == MF_E_TRANSFORM_STREAM_CHANGE || r == MF_E_TRANSFORM_TYPE_NOT_SET) {
      if (renegotiate_output) return renegotiate_output();
    }
    return r;
  }
};

// ---------------------------------------------------------------- SEH guard

struct SehInfo {
  bool crashed = false;
  unsigned long code = 0;
};

// No C++ objects with destructors live in this function, so __try is allowed.
bool SafeInvoke(const std::function<void()>& f, SehInfo* info) {
  __try {
    f();
    return true;
  } __except (info->code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
    info->crashed = true;
    return false;
  }
}

// ---------------------------------------------------------------- NV12 frames

HRESULT MakeNv12Sample(int w, int h, int idx, DWORD align, ComPtr<IMFSample>* out) {
  const DWORD size = static_cast<DWORD>(w) * h * 3 / 2;
  ComPtr<IMFMediaBuffer> buf;
  HRESULT hr = MFCreateAlignedMemoryBuffer(size, align > 1 ? align - 1 : 0, &buf);
  if (FAILED(hr)) return hr;
  BYTE* p = nullptr;
  hr = buf->Lock(&p, nullptr, nullptr);
  if (FAILED(hr)) return hr;
  // Luma: diagonal gradient that scrolls, plus a moving block, so that the
  // encoder has real motion to work on.
  const int bx = (idx * 17) % (w - 96);
  const int by = (idx * 7) % (h - 96);
  for (int y = 0; y < h; ++y) {
    BYTE* row = p + static_cast<size_t>(y) * w;
    for (int x = 0; x < w; ++x) {
      int v = ((x + y + idx * 3) >> 2) & 0xFF;
      if (x >= bx && x < bx + 96 && y >= by && y < by + 96) v = 235;
      row[x] = static_cast<BYTE>(v < 16 ? 16 : (v > 235 ? 235 : v));
    }
  }
  BYTE* uv = p + static_cast<size_t>(w) * h;
  for (int y = 0; y < h / 2; ++y) {
    BYTE* row = uv + static_cast<size_t>(y) * w;
    for (int x = 0; x < w; x += 2) {
      row[x] = static_cast<BYTE>(96 + ((x + idx * 2) & 63));
      row[x + 1] = static_cast<BYTE>(96 + ((y + idx) & 63));
    }
  }
  buf->Unlock();
  buf->SetCurrentLength(size);
  ComPtr<IMFSample> s;
  hr = MFCreateSample(&s);
  if (FAILED(hr)) return hr;
  s->AddBuffer(buf.Get());
  s->SetSampleTime(static_cast<LONGLONG>(idx) * 10000000LL / kFps);
  s->SetSampleDuration(10000000LL / kFps);
  *out = s;
  return S_OK;
}

HRESULT PickOutputNv12(IMFTransform* mft) {
  HRESULT last = MF_E_NO_MORE_TYPES;
  GUID st{};
  ComPtr<IMFMediaType> first;
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> t;
    HRESULT hr = mft->GetOutputAvailableType(0, i, &t);
    if (FAILED(hr)) {
      last = hr;
      break;
    }
    if (!first) first = t;
    if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &st)) && st == MFVideoFormat_NV12) {
      hr = mft->SetOutputType(0, t.Get(), 0);
      if (SUCCEEDED(hr)) return S_OK;
      last = hr;
    }
  }
  if (first) {
    HRESULT hr = mft->SetOutputType(0, first.Get(), 0);
    if (SUCCEEDED(hr)) return S_OK;
    last = hr;
  }
  return last;
}

void SetVideoCommon(IMFMediaType* t, int w, int h) {
  MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
  MFSetAttributeRatio(t, MF_MT_FRAME_RATE, kFps, 1);
  MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
}

// One independent attempt, first half: the D3D11 device and the DXGI device
// manager (when asked), then a freshly enumerated and activated MFT, then the
// async unlock and the D3D manager. Every call records its HRESULT as a step, so
// the report says which call failed. On false the result says why (failed or
// not_attempted).
bool PrepareMft(const MftInfo& mi, bool use_d3d, int adapter_ordinal, bool is_decoder,
                TestResult* r, Session* s) {
  r->memory = use_d3d ? "d3d11" : "system";
  if (use_d3d) {
    // The device is made first, on an explicitly chosen adapter, so that a vendor
    // MFT that looks for its GPU in the process finds it.
    HRESULT dh = S_OK;
    if (!MakeD3DDevice(mi.vendor_id, adapter_ordinal, &s->d3d, &dh)) {
      if (dh == DXGI_ERROR_NOT_FOUND) {
        r->Step("create_d3d_device", dh);
        r->Skip("no_d3d_adapter",
                "no non-software DXGI adapter to create the D3D11 device on "
                "(--allow-software-adapter runs the code path on a software adapter)");
      } else {
        r->Check("create_d3d_device", dh);
      }
      return false;
    }
    r->Step("create_d3d_device", S_OK);
    r->adapter_known = true;
    r->adapter = s->d3d.adapter.description;
    r->adapter_vendor = s->d3d.adapter.vendor_id;
    r->adapter_software = s->d3d.adapter.software;
    r->adapter_same_vendor = s->d3d.vendor_matched;
    if (!s->d3d.fallback_note.empty()) r->notes.push_back(s->d3d.fallback_note);
    if (s->d3d.video_flag_dropped) {
      r->notes.push_back(
          "software adapter: the D3D11 device was made without D3D11_CREATE_DEVICE_VIDEO_SUPPORT "
          "(not available there); this only runs the code path");
    } else if (!s->d3d.video_device) {
      r->notes.push_back("the D3D11 device has no ID3D11VideoDevice");
    }
    UINT token = 0;
    HRESULT hr = MFCreateDXGIDeviceManager(&token, &s->mgr);
    if (!r->Check("create_dxgi_manager", hr)) return false;
    hr = s->mgr->ResetDevice(s->d3d.dev.Get(), token);
    if (!r->Check("reset_device", hr)) return false;
  }

  HRESULT eh = S_OK;
  ComPtr<IMFActivate> act = FreshActivate(mi, &eh);
  if (!r->Check("enumerate_fresh", eh)) return false;
  HRESULT hr = act->ActivateObject(IID_PPV_ARGS(&s->mft));
  if (!r->Check("activate", hr)) {
    r->hint = HybridHint(mi, use_d3d ? &s->d3d : nullptr);
    return false;
  }
  s->act = act;

  UINT32 is_async = 0;
  {
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(s->mft->GetAttributes(&attrs)) && attrs) {
      attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
      if (is_async) {
        r->Step("async_unlock", attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
      }
      UINT32 aware = 0;
      if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware))) s->d3d_aware = aware != 0;
      attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
  }
  r->d3d11_aware = s->d3d_aware ? 1 : 0;
  r->mode = is_async ? "async" : "sync";
  if (is_async) {
    hr = s->mft.As(&s->gen);
    if (!r->Check("query_event_generator", hr)) return false;
  }

  if (use_d3d) {
    if (is_decoder && !mi.hardware && !s->d3d_aware) {
      r->Skip("not_d3d11_aware",
              "the decoder MFT is not D3D11 aware (MF_SA_D3D11_AWARE is not set), so it cannot "
              "take a D3D11 device manager");
      return false;
    }
    hr = s->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                reinterpret_cast<ULONG_PTR>(s->mgr.Get()));
    if (!r->Check("set_d3d_manager", hr)) {
      r->hint = HybridHint(mi, &s->d3d);
      return false;
    }
    s->manager_set = true;
    r->use_d3d = true;
  }
  return true;
}

// Diagnostics that make a first failure on real hardware debuggable from the
// report alone (no user data: counters and flag words only).
void AddPumpNotes(TestResult* r, const Pump& pump, IMFTransform* mft) {
  char b[128];
  MFT_INPUT_STREAM_INFO isi{};
  MFT_OUTPUT_STREAM_INFO osi{};
  if (SUCCEEDED(mft->GetInputStreamInfo(0, &isi)) && SUCCEEDED(mft->GetOutputStreamInfo(0, &osi))) {
    std::snprintf(b, sizeof b, "stream flags in=0x%lX out=0x%lX out_size=%lu", isi.dwFlags,
                  osi.dwFlags, osi.cbSize);
    r->notes.push_back(b);
  }
  if (pump.gen) {
    std::snprintf(b, sizeof b, "events need_input=%d have_output=%d other=%d",
                  pump.ev_need_input, pump.ev_have_output, pump.ev_other);
    r->notes.push_back(b);
  }
}

// ---------------------------------------------------------------- encode

void RunEncode(const MftInfo& mi, int w, int h, bool use_d3d, EncodeOut& out, Session& s) {
  TestResult& r = out.r;
  if (!PrepareMft(mi, use_d3d, -1, false, &r, &s)) return;
  IMFTransform* mft = s.mft.Get();
  const UINT32 bitrate = (w >= 1920) ? 6000000u : 3000000u;

  // Encoder tuning is best effort: failures are recorded as notes only.
  if (SUCCEEDED(s.mft.As(&s.api))) {
    auto set_u32 = [&](const GUID& g, const char* label, UINT32 v) {
      VARIANT var;
      ZeroMemory(&var, sizeof var);
      var.vt = VT_UI4;
      var.ulVal = v;
      HRESULT hr = s.api->SetValue(&g, &var);
      if (FAILED(hr)) r.notes.push_back(std::string("codecapi ") + label + " " + HrStr(hr));
    };
    set_u32(CODECAPI_AVEncCommonRateControlMode, "rate_control", eAVEncCommonRateControlMode_CBR);
    set_u32(CODECAPI_AVEncCommonMeanBitRate, "mean_bitrate", bitrate);
    set_u32(CODECAPI_AVEncMPVGOPSize, "gop", kFps);
    VARIANT var;
    ZeroMemory(&var, sizeof var);
    var.vt = VT_BOOL;
    var.boolVal = VARIANT_TRUE;
    HRESULT hr = s.api->SetValue(&CODECAPI_AVLowLatencyMode, &var);
    if (FAILED(hr)) r.notes.push_back("codecapi low_latency " + HrStr(hr));
  } else {
    r.notes.push_back("no ICodecAPI");
  }

  ComPtr<IMFMediaType> ot;
  MFCreateMediaType(&ot);
  ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  ot->SetGUID(MF_MT_SUBTYPE, g_subtype);
  ot->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
  SetVideoCommon(ot.Get(), w, h);
  if (g_is_hevc) ot->SetUINT32(MF_MT_MPEG2_PROFILE, kH265Main420x8);
  if (!r.Check("set_output_type", mft->SetOutputType(0, ot.Get(), 0))) return;

  bool in_ok = false;
  for (DWORD i = 0; !in_ok; ++i) {
    ComPtr<IMFMediaType> t;
    if (FAILED(mft->GetInputAvailableType(0, i, &t))) break;
    GUID st{};
    if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &st)) || st != MFVideoFormat_NV12) continue;
    SetVideoCommon(t.Get(), w, h);
    if (SUCCEEDED(mft->SetInputType(0, t.Get(), 0))) in_ok = true;
  }
  if (in_ok) {
    r.Step("set_input_type", S_OK);
  } else {
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    SetVideoCommon(t.Get(), w, h);
    if (!r.Check("set_input_type", mft->SetInputType(0, t.Get(), 0))) return;
  }

  MFT_INPUT_STREAM_INFO isi{};
  mft->GetInputStreamInfo(0, &isi);
  const DWORD align = isi.cbAlignment;

  s.streaming = true;
  mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  HRESULT hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  if (SUCCEEDED(hr)) hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (!r.Check("begin_streaming", hr)) return;

  Pump pump;
  pump.mft = mft;
  pump.gen = s.gen.Get();
  pump.total = kFrames;
  pump.default_out_size = static_cast<DWORD>(w) * h;
  pump.make_input = [&](int idx, IMFSample** smp) -> HRESULT {
    ComPtr<IMFSample> c;
    HRESULT e = MakeNv12Sample(w, h, idx, align, &c);
    if (FAILED(e)) return e;
    *smp = c.Detach();
    return S_OK;
  };
  pump.on_output = [&](IMFSample* smp) {
    ComPtr<IMFMediaBuffer> b;
    if (FAILED(smp->ConvertToContiguousBuffer(&b))) return;
    BYTE* d = nullptr;
    DWORD len = 0;
    if (FAILED(b->Lock(&d, nullptr, &len))) return;
    EncFrame f;
    f.data.assign(d, d + len);
    b->Unlock();
    smp->GetSampleTime(&f.time);
    r.bytes += len;
    out.frames.push_back(std::move(f));
  };
  pump.renegotiate_output = [&]() -> HRESULT {
    // Encoders rarely change the output type; re-apply ours if asked.
    return mft->SetOutputType(0, ot.Get(), 0);
  };
  const bool ok = pump.Run();
  AddPumpNotes(&r, pump, mft);
  r.frames_in = pump.fed;
  r.frames_out = pump.produced;
  r.seconds = pump.seconds;
  if (pump.seconds > 0) r.fps = pump.fed / pump.seconds;
  if (!ok) {
    r.Fail(pump.stage, pump.hr);
    r.Step(pump.stage, pump.hr);
    return;
  }
  if (pump.produced == 0) {
    r.Fail("no_output", E_FAIL);
    return;
  }
  s.clean_end = true;
  // Keep the sequence header (if the MFT publishes one) for the decoder test.
  ComPtr<IMFMediaType> cur;
  if (SUCCEEDED(mft->GetOutputCurrentType(0, &cur))) {
    UINT32 sz = 0;
    if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz > 0) {
      out.seq_header.resize(sz);
      cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, out.seq_header.data(), sz, nullptr);
    }
  }
  r.outcome = Outcome::kOk;
  if (r.frames_out != kFrames) {
    r.notes.push_back("frames_out differs from frames_in");
  }
}

EncodeOut DoEncode(const MftInfo& mi, int w, int h, bool use_d3d) {
  EncodeOut out;
  TestResult& r = out.r;
  r.role = "encode";
  r.kind = "full";
  r.mft = mi.name;
  r.hardware = mi.hardware;
  r.width = w;
  r.height = h;
  Session s;
  RunEncode(mi, w, h, use_d3d, out, s);
  s.Teardown(&r);
  return out;
}

// ---------------------------------------------------------------- decode

void RunDecode(const MftInfo& mi, int w, int h, const std::vector<EncFrame>* stream,
               const std::vector<uint8_t>* seq_header, bool use_d3d, int adapter_ordinal,
               TestResult& r, Session& s) {
  if (!PrepareMft(mi, use_d3d, adapter_ordinal, true, &r, &s)) return;
  IMFTransform* mft = s.mft.Get();

  ComPtr<IMFMediaType> it;
  MFCreateMediaType(&it);
  it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  it->SetGUID(MF_MT_SUBTYPE, g_subtype);
  SetVideoCommon(it.Get(), w, h);
  if (seq_header && !seq_header->empty()) {
    it->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seq_header->data(),
                static_cast<UINT32>(seq_header->size()));
  }
  if (!r.Check("set_input_type", mft->SetInputType(0, it.Get(), 0))) return;
  // Some decoders only publish their output types after the first frame; a
  // failure here is therefore not fatal for the full test.
  HRESULT out_hr = PickOutputNv12(mft);
  r.Step("set_output_type", out_hr);
  if (FAILED(out_hr)) r.notes.push_back("output type deferred " + HrStr(out_hr));

  s.streaming = true;
  mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  HRESULT hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  if (SUCCEEDED(hr)) hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (!r.Check("begin_streaming", hr)) return;
  if (!stream) {
    // No bitstream available (no working encoder on this machine): report that
    // the decoder could be created and configured for the codec's input.
    s.clean_end = true;
    r.outcome = Outcome::kOk;
    return;
  }

  Pump pump;
  pump.mft = mft;
  pump.gen = s.gen.Get();
  pump.total = static_cast<int>(stream->size());
  pump.default_out_size = static_cast<DWORD>(w) * h * 3 / 2;
  pump.make_input = [&](int idx, IMFSample** smp) -> HRESULT {
    const EncFrame& f = (*stream)[static_cast<size_t>(idx)];
    ComPtr<IMFMediaBuffer> b;
    HRESULT e = MFCreateMemoryBuffer(static_cast<DWORD>(f.data.size()), &b);
    if (FAILED(e)) return e;
    BYTE* d = nullptr;
    e = b->Lock(&d, nullptr, nullptr);
    if (FAILED(e)) return e;
    std::memcpy(d, f.data.data(), f.data.size());
    b->Unlock();
    b->SetCurrentLength(static_cast<DWORD>(f.data.size()));
    ComPtr<IMFSample> sm;
    e = MFCreateSample(&sm);
    if (FAILED(e)) return e;
    sm->AddBuffer(b.Get());
    sm->SetSampleTime(f.time);
    sm->SetSampleDuration(10000000LL / kFps);
    *smp = sm.Detach();
    return S_OK;
  };
  pump.on_output = [&](IMFSample* smp) {
    // A frame in a D3D11 texture (IMFDXGIBuffer) is what the GPU decode path
    // delivers; a frame in plain memory is not.
    DWORD n = 0;
    if (FAILED(smp->GetBufferCount(&n)) || n == 0) return;
    ComPtr<IMFMediaBuffer> b;
    if (FAILED(smp->GetBufferByIndex(0, &b))) return;
    ComPtr<IMFDXGIBuffer> dxgi;
    if (SUCCEEDED(b.As(&dxgi))) ++r.output_textures;
  };
  pump.renegotiate_output = [&]() -> HRESULT { return PickOutputNv12(mft); };
  const bool ok = pump.Run();
  AddPumpNotes(&r, pump, mft);
  r.frames_in = pump.fed;
  r.frames_out = pump.produced;
  r.seconds = pump.seconds;
  if (pump.seconds > 0) r.fps = pump.produced / pump.seconds;
  if (!ok) {
    r.Fail(pump.stage, pump.hr);
    r.Step(pump.stage, pump.hr);
    return;
  }
  if (pump.produced == 0) {
    r.Fail("no_output", E_FAIL);
    return;
  }
  s.clean_end = true;
  r.outcome = Outcome::kOk;
  if (r.frames_out != r.frames_in) r.notes.push_back("frames_out differs from frames_in");
  r.gpu_path = r.use_d3d && r.output_textures > 0 && r.output_textures == r.frames_out &&
               r.adapter_known && !r.adapter_software;
  if (r.use_d3d && r.output_textures != r.frames_out) {
    r.notes.push_back("D3D11 manager accepted, but " + std::to_string(r.output_textures) + " of " +
                      std::to_string(r.frames_out) + " frames came out as D3D11 textures");
  }
}

TestResult DoDecode(const MftInfo& mi, int w, int h, const std::vector<EncFrame>* stream,
                    const std::vector<uint8_t>* seq_header, bool use_d3d, int adapter_ordinal,
                    const std::string& stream_source) {
  TestResult r;
  r.role = "decode";
  r.kind = stream ? "full" : "configure_only";
  r.mft = mi.name;
  r.hardware = mi.hardware;
  r.width = w;
  r.height = h;
  r.stream_source = stream_source;
  Session s;
  RunDecode(mi, w, h, stream, seq_header, use_d3d, adapter_ordinal, r, s);
  s.Teardown(&r);
  return r;
}

// ---------------------------------------------------------------- watchdog

TestResult& Result(TestResult& r) { return r; }
TestResult& Result(EncodeOut& e) { return e.r; }

void MarkCrash(TestResult& r, unsigned long code) {
  r.outcome = Outcome::kFailed;
  r.stage = "crash";
  r.hr = E_FAIL;
  r.error = "structured exception " + Hex32(code);
}

// Runs `f` on its own MTA thread with a watchdog. A hung or crashing driver
// must not take the probe down: the result then says "timeout" or "crash".
template <typename R, typename F>
R WithWatchdog(F f, R timeout_value) {
  auto prom = std::make_shared<std::promise<R>>();
  std::future<R> fut = prom->get_future();
  std::thread([prom, f, timeout_value]() mutable {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    R res{};
    SehInfo info;
    std::function<void()> body = [&] { res = f(); };
    if (!SafeInvoke(body, &info)) {
      // Start from the timeout value so that the entry still names the MFT,
      // the resolution and the D3D mode that crashed.
      res = timeout_value;
      MarkCrash(Result(res), info.code);
    }
    prom->set_value(std::move(res));
  }).detach();
  if (fut.wait_for(std::chrono::milliseconds(kTestTimeoutMs)) != std::future_status::ready) {
    ++g_hung_attempts;
    return timeout_value;
  }
  return fut.get();
}

}  // namespace

// ---------------------------------------------------------------- system info

namespace {

std::string WindowsVersion() {
  using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  if (!nt) return {};
  Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")));
  if (!fn) return {};
  OSVERSIONINFOW vi{};
  vi.dwOSVersionInfoSize = sizeof vi;
  if (fn(&vi) != 0) return {};
  char b[48];
  std::snprintf(b, sizeof b, "%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion,
                vi.dwBuildNumber);
  return b;
}

std::string CpuBrand() {
  int r[4] = {0, 0, 0, 0};
  __cpuid(r, static_cast<int>(0x80000000));
  if (static_cast<unsigned>(r[0]) < 0x80000004u) return {};
  char brand[49] = {};
  for (int i = 0; i < 3; ++i) {
    __cpuid(r, static_cast<int>(0x80000002u + i));
    std::memcpy(brand + 16 * i, r, 16);
  }
  std::string s(brand);
  size_t a = s.find_first_not_of(' ');
  size_t b = s.find_last_not_of(' ');
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Package full names of the HEVC Video Extensions registered for the current
// user (the user the application process runs as). The names carry the package
// version and architecture, nothing user specific.
std::vector<std::string> FindPackages(const wchar_t* family) {
  using Fn = LONG(WINAPI*)(PCWSTR, UINT32*, PWSTR*, UINT32*, PWSTR);
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  if (!k) return {};
  Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
      GetProcAddress(k, "GetPackagesByPackageFamily")));
  if (!fn) return {};
  UINT32 count = 0, len = 0;
  LONG rc = fn(family, &count, nullptr, &len, nullptr);
  if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0) return {};
  std::vector<PWSTR> names(count);
  std::vector<WCHAR> buf(len);
  rc = fn(family, &count, names.data(), &len, buf.data());
  if (rc != ERROR_SUCCESS) return {};
  std::vector<std::string> out;
  for (UINT32 i = 0; i < count; ++i) out.push_back(Utf8(names[i]));
  return out;
}

// ---------------------------------------------------------------- JSON output

void WriteMft(Json& j, const MftInfo& m) {
  j.BeginObject();
  j.KvS("name", m.name);
  j.KvS("clsid", m.clsid);
  j.KvB("hardware", m.hardware);
  j.KvB("async", m.async);
  j.KvB("sync", m.sync);
  j.KvS("vendor_id_string", m.vendor);
  j.KvS("flags", Hex32(m.flags));
  j.EndObject();
}

void WriteTest(Json& j, const TestResult& t) {
  j.BeginObject();
  j.KvS("role", t.role);
  j.KvS("kind", t.kind);
  j.KvS("mft", t.mft);
  j.KvB("hardware", t.hardware);
  j.KvS("resolution", std::to_string(t.width) + "x" + std::to_string(t.height));
  j.KvS("memory", t.memory);
  j.KvS("mode", t.mode);
  j.KvB("d3d11_manager", t.use_d3d);
  j.KvS("status", OutcomeStr(t.outcome));
  if (t.outcome == Outcome::kNotAttempted) {
    j.KvS("reason_code", t.reason_code);
    j.KvS("reason", t.reason);
  } else if (t.outcome == Outcome::kFailed) {
    j.KvS("failed_stage", t.stage);
    j.KvS("hresult", HrStr(t.hr));
    if (!t.error.empty()) j.KvS("error", t.error);
  }
  if (t.adapter_known) {
    j.Key("adapter");
    j.BeginObject();
    j.KvS("description", t.adapter);
    j.KvS("vendor_id", Hex32(t.adapter_vendor));
    j.KvB("software_adapter", t.adapter_software);
    j.KvB("same_vendor_as_mft", t.adapter_same_vendor);
    j.EndObject();
  }
  if (t.d3d11_aware >= 0) j.KvB("mft_d3d11_aware", t.d3d11_aware != 0);
  if (t.role == "decode") {
    if (!t.stream_source.empty()) j.KvS("stream_source", t.stream_source);
    if (t.outcome == Outcome::kOk && t.kind == "full") {
      j.KvI("output_d3d11_textures", t.output_textures);
      j.KvB("gpu_path", t.gpu_path);
    }
  }
  if (t.outcome != Outcome::kNotAttempted) {
    j.KvI("frames_in", t.frames_in);
    j.KvI("frames_out", t.frames_out);
    j.KvI("bytes_out", static_cast<long long>(t.bytes));
    j.KvN("seconds", t.seconds);
    j.KvN("fps", t.fps);
  }
  if (t.residual_refs >= 0) j.KvI("mft_refs_left_after_release", t.residual_refs);
  if (!t.hint.empty()) j.KvS("hint", t.hint);
  if (!t.steps.empty()) {
    j.Key("steps");
    j.BeginArray();
    for (const StepRec& s : t.steps) {
      j.BeginObject();
      j.KvS("stage", s.stage);
      j.KvS("hresult", HrStr(s.hr));
      j.EndObject();
    }
    j.EndArray();
  }
  if (!t.notes.empty()) {
    j.Key("notes");
    j.BeginArray();
    for (const std::string& n : t.notes) j.Str(n);
    j.EndArray();
  }
  j.EndObject();
}

bool WriteWholeFile(const std::wstring& path, const std::string& data) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
            written == data.size();
  CloseHandle(h);
  return ok;
}

// ---------------------------------------------------------------- test drivers

EncodeOut TimeoutEnc(const MftInfo& mi, int w, int h, bool d3d) {
  EncodeOut o;
  o.r.role = "encode";
  o.r.kind = "full";
  o.r.mft = mi.name;
  o.r.hardware = mi.hardware;
  o.r.width = w;
  o.r.height = h;
  o.r.memory = d3d ? "d3d11" : "system";
  o.r.use_d3d = d3d;
  o.r.outcome = Outcome::kFailed;
  o.r.stage = "timeout";
  o.r.hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  o.r.error = "no result within the watchdog time";
  return o;
}

TestResult TimeoutDec(const MftInfo& mi, int w, int h, bool d3d, bool full) {
  TestResult r;
  r.role = "decode";
  r.kind = full ? "full" : "configure_only";
  r.mft = mi.name;
  r.hardware = mi.hardware;
  r.width = w;
  r.height = h;
  r.memory = d3d ? "d3d11" : "system";
  r.use_d3d = d3d;
  r.outcome = Outcome::kFailed;
  r.stage = "timeout";
  r.hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  r.error = "no result within the watchdog time";
  return r;
}

TestResult NotAttemptedTest(const char* role, const MftInfo& mi, int w, int h, const char* kind,
                            const std::string& code, const std::string& why,
                            const std::string& stream_source = std::string()) {
  TestResult r;
  r.role = role;
  r.kind = kind;
  r.mft = mi.name;
  r.hardware = mi.hardware;
  r.width = w;
  r.height = h;
  r.stream_source = stream_source;
  r.Skip(code, why);
  return r;
}

void Progress(const char* what, const std::string& mft, const std::string& extra) {
  std::fprintf(stderr, "[hevc-probe] %s: %s %s\n", what, mft.c_str(), extra.c_str());
}

// Notes that the attempt may have been disturbed by a hung earlier attempt.
void NoteHungAttempts(TestResult& r) {
  const int hung = g_hung_attempts.load();
  if (hung > 0 && r.stage != "timeout") {
    r.notes.push_back(std::to_string(hung) +
                      " earlier attempt(s) timed out; their threads may still hold MFT instances");
  }
}

struct Stream {
  std::shared_ptr<std::vector<EncFrame>> frames;
  std::shared_ptr<std::vector<uint8_t>> seq;
  bool d3d = false;
  std::string source;  // which encoder produced it
};

std::string StreamSource(const MftInfo& mi, bool fallback) {
  std::string s = std::string(mi.hardware ? "hardware" : "software") + " encoder " + mi.name;
  if (fallback) s += " (fallback: no hardware encoder produced this resolution)";
  return s;
}

// Encodes with one MFT, trying the D3D11 manager first for hardware MFTs. Every
// attempt starts from a fresh enumeration and ends with a full teardown.
// Appends every attempt to `log`; returns true on the first success.
bool EncodeAttempts(const MftInfo& mi, int w, int h, std::vector<TestResult>* log,
                    Stream* stream, bool fallback) {
  std::vector<bool> modes = mi.hardware ? std::vector<bool>{true, false} : std::vector<bool>{false};
  for (bool d3d : modes) {
    Progress("encode", mi.name,
             std::to_string(w) + "x" + std::to_string(h) + (d3d ? " (d3d11)" : " (system memory)"));
    EncodeOut eo = WithWatchdog<EncodeOut>([=] { return DoEncode(mi, w, h, d3d); },
                                           TimeoutEnc(mi, w, h, d3d));
    NoteHungAttempts(eo.r);
    log->push_back(eo.r);
    Sleep(kSettleMs);
    if (eo.r.outcome == Outcome::kOk) {
      stream->frames = std::make_shared<std::vector<EncFrame>>(std::move(eo.frames));
      stream->seq = std::make_shared<std::vector<uint8_t>>(std::move(eo.seq_header));
      stream->d3d = d3d;
      stream->source = StreamSource(mi, fallback);
      return true;
    }
  }
  return false;
}

struct DecodeMode {
  bool d3d;
  int adapter;  // -1: by MFT vendor, else the n-th non-software adapter
};

// The D3D11 (DXVA) path first, on the adapter of the MFT vendor or, for MFTs
// without a vendor (the in-box software decoders), on each of up to two hardware
// adapters; then system memory.
std::vector<DecodeMode> DecodeModes(const MftInfo& mi) {
  std::vector<DecodeMode> m;
  if (mi.hardware || mi.vendor_id != 0) {
    m.push_back({true, -1});
  } else {
    const int n = std::min(CountHardwareAdapters(), 2);
    if (n == 0) {
      m.push_back({true, -1});
    } else {
      for (int i = 0; i < n; ++i) m.push_back({true, i});
    }
  }
  m.push_back({false, -1});
  return m;
}

std::string DecodeKey(const MftInfo& mi, const DecodeMode& m) {
  return mi.clsid + "|" + (m.d3d ? "d3d11:" + std::to_string(m.adapter) : std::string("system"));
}

// Decodes with one MFT in every mode of DecodeModes. A hardware decoder uses
// system memory only as a fallback; a software decoder is tested in both memory
// models, so that the D3D11-aware path is reported separately from the plain one.
// `skip_keys`: modes that already failed at another resolution are not repeated.
bool DecodeAttempts(const MftInfo& mi, int w, int h, const Stream* s,
                    std::vector<TestResult>* log, const std::set<std::string>* skip_keys,
                    std::set<std::string>* failed_keys) {
  bool any = false;
  bool d3d_unaware = false;
  const bool full = s != nullptr;
  const std::string source = s ? s->source : std::string();
  for (const DecodeMode& mode : DecodeModes(mi)) {
    if (mode.d3d && d3d_unaware) continue;
    if (!mode.d3d && mi.hardware && any) continue;
    const std::string key = DecodeKey(mi, mode);
    if (skip_keys && skip_keys->count(key)) {
      TestResult na = NotAttemptedTest("decode", mi, w, h, "full", "earlier_stage_failed",
                                       "the same decode mode failed at 1280x720, so it was not "
                                       "repeated at this resolution",
                                       source);
      na.memory = mode.d3d ? "d3d11" : "system";
      log->push_back(na);
      continue;
    }
    Progress("decode", mi.name,
             std::to_string(w) + "x" + std::to_string(h) +
                 (mode.d3d ? " (d3d11)" : " (system memory)"));
    std::shared_ptr<std::vector<EncFrame>> frames = s ? s->frames : nullptr;
    std::shared_ptr<std::vector<uint8_t>> seq = s ? s->seq : nullptr;
    const int ordinal = mode.adapter;
    const bool d3d = mode.d3d;
    TestResult r = WithWatchdog<TestResult>(
        [=] { return DoDecode(mi, w, h, frames.get(), seq.get(), d3d, ordinal, source); },
        TimeoutDec(mi, w, h, d3d, full));
    NoteHungAttempts(r);
    log->push_back(r);
    Sleep(kSettleMs);
    if (r.outcome == Outcome::kOk) {
      any = true;
    } else if (r.outcome == Outcome::kNotAttempted && r.reason_code == "not_d3d11_aware") {
      d3d_unaware = true;
    } else if (r.outcome == Outcome::kFailed && failed_keys) {
      failed_keys->insert(key);
    }
  }
  return any;
}

// ---------------------------------------------------------------- summary

struct Verdict {
  std::string status;  // ok | failed | not_attempted
  std::string reason;  // empty for ok
};

// Folds the tests that match `pred` into one tri-state value: ok when one of them
// succeeded, failed when some were made and none succeeded, not_attempted when
// none was made (with the reason of the first one that was not, or `none_reason`
// when there was no entry at all). With need_gpu_path an ok test that did not run
// on the GPU (software adapter, or frames in plain memory) does not count.
template <typename Pred>
Verdict Judge(const std::vector<TestResult>& tests, Pred pred, const std::string& none_reason,
              bool need_gpu_path = false) {
  int ok = 0, failed = 0, na = 0;
  std::string fail_reason, na_reason;
  for (const TestResult& t : tests) {
    if (!pred(t)) continue;
    if (t.outcome == Outcome::kOk && (!need_gpu_path || t.gpu_path)) {
      ++ok;
    } else if (t.outcome == Outcome::kOk) {
      ++failed;
      fail_reason = t.mft + ": decoded, but not on a GPU path (software adapter, or frames in system memory)";
    } else if (t.outcome == Outcome::kFailed) {
      ++failed;
      fail_reason = t.mft + " (" + t.memory + (t.adapter.empty() ? std::string() : ", " + t.adapter) +
                    "): " + t.stage + " " + HrStr(t.hr);
    } else {
      ++na;
      if (na_reason.empty()) na_reason = t.mft + ": " + t.reason;
    }
  }
  if (ok) return {"ok", std::string()};
  if (failed) return {"failed", fail_reason};
  if (na) return {"not_attempted", na_reason};
  return {"not_attempted", none_reason};
}

Verdict Combine(const std::vector<Verdict>& vs) {
  for (const Verdict& v : vs) {
    if (v.status == "ok") return v;
  }
  for (const Verdict& v : vs) {
    if (v.status == "failed") return v;
  }
  for (const Verdict& v : vs) {
    if (!v.reason.empty()) return v;
  }
  return {"not_attempted", std::string()};
}

}  // namespace

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
  bool pause = false;
  bool ci = false;
  bool want[2] = {true, true};  // 720p, 1080p
  std::wstring out_name = L"hevc-probe-report.json";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--pause") {
      pause = true;
    } else if (a == "--ci") {
      ci = true;
      g_allow_sw_adapter = true;
    } else if (a == "--allow-software-adapter") {
      g_allow_sw_adapter = true;
    } else if (a == "--resolutions" && i + 1 < argc) {
      std::string v = argv[++i];
      want[0] = want[1] = false;
      size_t pos = 0;
      for (;;) {
        size_t c = v.find(',', pos);
        std::string tok = v.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        if (tok == "720" || tok == "720p") {
          want[0] = true;
        } else if (tok == "1080" || tok == "1080p") {
          want[1] = true;
        } else {
          std::fprintf(stderr, "unknown resolution %s (720, 1080)\n", tok.c_str());
          return 2;
        }
        if (c == std::string::npos) break;
        pos = c + 1;
      }
    } else if (a == "--codec" && i + 1 < argc) {
      std::string c = argv[++i];
      if (c == "h264") {
        g_subtype = MFVideoFormat_H264;
        g_is_hevc = false;
        g_codec_name = "h264";
      } else if (c != "hevc") {
        std::fprintf(stderr, "unknown codec %s (hevc or h264)\n", c.c_str());
        return 2;
      }
    } else if (a == "--out" && i + 1 < argc) {
      int n = MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, nullptr, 0);
      if (n > 1) {
        out_name.assign(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, &out_name[0], n);
        out_name.resize(static_cast<size_t>(n) - 1);
      }
      ++i;
    } else if (a == "--help" || a == "-h") {
      std::fprintf(stderr,
                   "hevc-probe [--out <file name>] [--pause] [--codec hevc|h264]\n"
                   "           [--resolutions 720,1080] [--allow-software-adapter] [--ci]\n"
                   "Writes a JSON report to stdout and to a file next to the exe.\n"
                   "--resolutions   run only the listed resolutions (default both); 1080 alone\n"
                   "                shows whether a 1080p failure depends on the 720p run before it\n"
                   "--allow-software-adapter  let the D3D11 attempts use a software adapter when\n"
                   "                the machine has no GPU (runs the code path, proves nothing\n"
                   "                about hardware)\n"
                   "--ci            --allow-software-adapter, and exit code 4 when an attempt\n"
                   "                crashed or timed out (the default exit code is 0 whenever the\n"
                   "                report was written, a machine without a GPU included)\n");
      return 0;
    }
  }

  // Hard cap on the run time (disarmed once the report is out, so that --pause
  // can wait for Enter).
  std::thread([] {
    for (DWORD waited = 0; waited < kHardLimitMs; waited += 500) {
      if (g_run_finished.load()) return;
      Sleep(500);
    }
    if (g_run_finished.load()) return;
    std::fprintf(stderr, "[hevc-probe] hard time limit reached, giving up\n");
    std::fflush(stderr);
    ExitProcess(3);
  }).detach();

  _setmode(_fileno(stdout), _O_BINARY);  // the file and stdout carry identical bytes
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::fprintf(stderr, "[hevc-probe] start (%s)\n", g_codec_name);

  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  for (AdapterEntry& e : EnumAdapters()) g_adapters.push_back(e.info);

  // Load Media Foundation from System32 only (the exe is typically run from a
  // download folder; the delay-loaded import then binds to this module).
  const bool mf_dll = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
  HRESULT mf_hr = mf_dll ? S_FALSE : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
  if (mf_dll) mf_hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
  const bool mf_ok = SUCCEEDED(mf_hr);

  std::vector<MftInfo> encoders, decoders;
  std::map<std::string, int> enc_counts, dec_counts;
  if (mf_ok) {
    encoders = EnumMfts(MFT_CATEGORY_VIDEO_ENCODER, g_subtype, true, &enc_counts);
    decoders = EnumMfts(MFT_CATEGORY_VIDEO_DECODER, g_subtype, false, &dec_counts);
  }
  auto hw_first = [](std::vector<MftInfo>& v) {
    std::stable_partition(v.begin(), v.end(), [](const MftInfo& m) { return m.hardware; });
  };
  hw_first(encoders);
  hw_first(decoders);

  // The two package families of the HEVC Video Extensions, as registered for the
  // current user. Whether the package is provisioned in the system image needs
  // elevation and is left to hw-inventory.ps1.
  const wchar_t* const kExtFamilies[] = {L"Microsoft.HEVCVideoExtension_8wekyb3d8bbwe",
                                         L"Microsoft.HEVCVideoExtensions_8wekyb3d8bbwe"};
  std::vector<std::string> ext_packages;
  for (const wchar_t* fam : kExtFamilies) {
    for (std::string& n : FindPackages(fam)) ext_packages.push_back(std::move(n));
  }

  std::vector<TestResult> tests;
  struct Res {
    int w, h;
    bool enabled;
    Stream stream;
    std::string no_stream_reason;
  };
  Res res[2] = {{1280, 720, want[0], Stream(), std::string()},
                {1920, 1080, want[1], Stream(), std::string()}};
  const std::string codec_upper = g_is_hevc ? "HEVC" : "H.264";

  // ---- encode, 720p: every hardware encoder (up to four); a software encoder
  // only when no hardware encoder worked.
  const MftInfo* best_enc = nullptr;
  std::set<const MftInfo*> enc_ok_720;
  if (res[0].enabled) {
    int hw_tried = 0, sw_tried = 0;
    for (const MftInfo& mi : encoders) {
      if (mi.hardware) {
        if (hw_tried++ >= 4) continue;
      } else {
        if (best_enc || sw_tried++ >= 2) continue;
      }
      Stream st;
      if (EncodeAttempts(mi, 1280, 720, &tests, &st, false)) {
        enc_ok_720.insert(&mi);
        if (!best_enc) {
          best_enc = &mi;
          res[0].stream = st;
        }
      }
    }
    if (!res[0].stream.frames) {
      res[0].no_stream_reason =
          encoders.empty() ? "no " + codec_upper + " encoder MFT is registered"
                           : "no encoder produced a 720p bitstream (see the encode tests)";
    }
  } else {
    res[0].no_stream_reason = "720p was not selected (--resolutions)";
  }

  // ---- encode, 1080p: each hardware encoder that worked at 720p (every hardware
  // encoder when 720p was not run), each attempt independent of the earlier ones.
  // When no hardware encoder produced a 1080p stream, a software encoder makes one
  // so that the decoders can still be tested (the stream source says so).
  if (res[1].enabled) {
    int hw_tried = 0;
    for (const MftInfo& mi : encoders) {
      if (!mi.hardware) continue;
      if (hw_tried++ >= 4) continue;
      if (res[0].enabled && !enc_ok_720.count(&mi)) {
        tests.push_back(NotAttemptedTest("encode", mi, 1920, 1080, "full", "earlier_stage_failed",
                                         "the 720p encode failed for this MFT, so 1080p was not "
                                         "tried for it"));
        continue;
      }
      if (res[1].stream.frames) {
        tests.push_back(NotAttemptedTest("encode", mi, 1920, 1080, "full", "stream_available",
                                         "another hardware encoder already produced the 1080p "
                                         "bitstream"));
        continue;
      }
      Stream st;
      if (EncodeAttempts(mi, 1920, 1080, &tests, &st, false)) res[1].stream = st;
    }
    bool sw_present = false;
    if (!res[1].stream.frames) {
      int sw_tried = 0;
      for (const MftInfo& mi : encoders) {
        if (mi.hardware) continue;
        sw_present = true;
        if (sw_tried++ >= 2) continue;
        Stream st;
        if (EncodeAttempts(mi, 1920, 1080, &tests, &st, true)) {
          res[1].stream = st;
          break;
        }
      }
    }
    if (!res[1].stream.frames) {
      if (encoders.empty()) {
        res[1].no_stream_reason = "no " + codec_upper + " encoder MFT is registered";
      } else if (!sw_present) {
        res[1].no_stream_reason =
            "no hardware encoder produced a 1080p bitstream and there is no software " +
            codec_upper + " encoder MFT to make one";
      } else {
        res[1].no_stream_reason =
            "no encoder, software ones included, produced a 1080p bitstream";
      }
    }
  } else {
    res[1].no_stream_reason = "1080p was not selected (--resolutions)";
  }

  // ---- decode tests: every decoder (up to six, hardware first), each resolution
  // on the stream of its own resolution.
  std::set<std::string> failed_720_modes;
  for (int ri = 0; ri < 2; ++ri) {
    if (!res[ri].enabled) continue;
    int dec_tried = 0;
    for (const MftInfo& mi : decoders) {
      if (dec_tried++ >= 6) break;
      if (res[ri].stream.frames) {
        DecodeAttempts(mi, res[ri].w, res[ri].h, &res[ri].stream, &tests,
                       ri == 1 ? &failed_720_modes : nullptr, ri == 0 ? &failed_720_modes : nullptr);
      } else if (ri == 0) {
        // No bitstream: only check that the decoder can be created and configured.
        DecodeAttempts(mi, res[ri].w, res[ri].h, nullptr, &tests, nullptr, nullptr);
      } else {
        TestResult na = NotAttemptedTest("decode", mi, res[ri].w, res[ri].h, "full",
                                         "no_bitstream",
                                         "no 1080p bitstream to decode: " + res[ri].no_stream_reason);
        tests.push_back(na);
      }
    }
  }

  // ---- summary values
  bool hw_enc_present = false, sw_enc_present = false;
  for (const MftInfo& m : encoders) (m.hardware ? hw_enc_present : sw_enc_present) = true;
  bool hw_dec_present = false, sw_dec_present = false;
  for (const MftInfo& m : decoders) (m.hardware ? hw_dec_present : sw_dec_present) = true;
  double fps_720 = 0, fps_1080 = 0;
  for (const TestResult& t : tests) {
    if (t.role != "encode" || t.outcome != Outcome::kOk) continue;
    if (t.width == 1280) {
      fps_720 = std::max(fps_720, t.fps);
    } else {
      fps_1080 = std::max(fps_1080, t.fps);
    }
  }

  const std::string not_selected_720 = "720p was not selected (--resolutions)";
  const std::string not_selected_1080 = "1080p was not selected (--resolutions)";
  const std::string no_enc = "no " + codec_upper + " encoder MFT is registered";
  const std::string no_hw_enc = "no hardware " + codec_upper + " encoder MFT is registered";
  std::string no_dec = "no " + codec_upper + " decoder MFT is registered";
  if (g_is_hevc && ext_packages.empty()) {
    no_dec += " (the HEVC Video Extensions are not registered for the current user)";
  }
  auto enc_is = [](int w) {
    return [w](const TestResult& t) { return t.role == "encode" && t.width == w; };
  };
  auto hw_enc_is = [](int w) {
    return [w](const TestResult& t) { return t.role == "encode" && t.width == w && t.hardware; };
  };
  auto dec_is = [](int w) {
    return [w](const TestResult& t) { return t.role == "decode" && t.kind == "full" && t.width == w; };
  };
  auto dxva_is = [](int w) {
    return [w](const TestResult& t) {
      return t.role == "decode" && t.kind == "full" && t.width == w && t.memory == "d3d11";
    };
  };
  auto hwmft_is = [](int w) {
    return [w](const TestResult& t) {
      return t.role == "decode" && t.kind == "full" && t.width == w && t.hardware;
    };
  };
  // The reason that stands in when a metric has no test entry at all.
  const std::string r_hw_enc_720 = res[0].enabled ? no_hw_enc : not_selected_720;
  const std::string r_hw_enc_1080 = res[1].enabled ? no_hw_enc : not_selected_1080;
  const std::string r_enc_720 = res[0].enabled ? no_enc : not_selected_720;
  const std::string r_enc_1080 = res[1].enabled ? res[1].no_stream_reason : not_selected_1080;
  const std::string r_dec_720 =
      !res[0].enabled ? not_selected_720
                      : (decoders.empty() ? no_dec : "no 720p bitstream: " + res[0].no_stream_reason);
  const std::string r_dec_1080 =
      !res[1].enabled ? not_selected_1080
                      : (decoders.empty() ? no_dec : "no 1080p bitstream: " + res[1].no_stream_reason);
  const std::string no_hw_dec = "no hardware decoder MFT is registered";
  const std::string r_hwmft_720 = res[0].enabled ? no_hw_dec : not_selected_720;
  const std::string r_hwmft_1080 = res[1].enabled ? no_hw_dec : not_selected_1080;

  const Verdict v_hw_enc_720 = Judge(tests, hw_enc_is(1280), r_hw_enc_720);
  const Verdict v_hw_enc_1080 = Judge(tests, hw_enc_is(1920), r_hw_enc_1080);
  const Verdict v_enc_720 = Judge(tests, enc_is(1280), r_enc_720);
  const Verdict v_enc_1080 = Judge(tests, enc_is(1920), r_enc_1080);
  const Verdict v_dec_720 = Judge(tests, dec_is(1280), r_dec_720);
  const Verdict v_dec_1080 = Judge(tests, dec_is(1920), r_dec_1080);
  const Verdict v_dxva_720 = Judge(tests, dxva_is(1280), r_dec_720, true);
  const Verdict v_dxva_1080 = Judge(tests, dxva_is(1920), r_dec_1080, true);
  const Verdict v_hwmft_720 = Judge(tests, hwmft_is(1280), r_hwmft_720);
  const Verdict v_hwmft_1080 = Judge(tests, hwmft_is(1920), r_hwmft_1080);
  const Verdict v_hw_accel = Combine({v_dxva_720, v_dxva_1080, v_hwmft_720, v_hwmft_1080});
  const Verdict v_cfg = Judge(tests,
                              [](const TestResult& t) { return t.role == "decode" && t.kind == "configure_only"; },
                              "a bitstream was available, so the decoders were tested with it instead");

  const bool have_stream_720 = res[0].stream.frames != nullptr;
  const bool receive_possible =
      v_dec_720.status == "ok" || (!have_stream_720 && v_cfg.status == "ok");
  std::string receive_basis = "none";
  if (v_dxva_720.status == "ok") {
    receive_basis = "d3d11_gpu_decode";
  } else if (v_hwmft_720.status == "ok") {
    receive_basis = "hardware_mft";
  } else if (v_dec_720.status == "ok") {
    receive_basis = "decoder_without_gpu_path";
  } else if (!have_stream_720 && v_cfg.status == "ok") {
    receive_basis = "configure_only";
  }

  bool crashed_or_timed_out = false;
  for (const TestResult& t : tests) {
    if (t.outcome == Outcome::kFailed && (t.stage == "crash" || t.stage == "timeout")) {
      crashed_or_timed_out = true;
    }
  }

  Json j;
  j.BeginObject();
  j.Key("tool");
  j.BeginObject();
  j.KvS("name", "hevc-probe");
  j.KvI("report_version", 2);
  j.KvS("codec", g_codec_name);
  j.KvS("git_sha", PROBE_GIT_SHA);
  j.KvS("test_content", "synthetic NV12, 60 frames, 30 fps, 1280x720 and 1920x1080");
  j.KvS("result_values", "ok | failed | not_attempted (an attempt that was not made is never failed)");
  j.KvB("allow_software_adapter", g_allow_sw_adapter);
  j.KvB("resolution_720_selected", res[0].enabled);
  j.KvB("resolution_1080_selected", res[1].enabled);
  j.EndObject();

  j.Key("system");
  j.BeginObject();
  j.KvS("windows_version", WindowsVersion());
  j.KvS("architecture", si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64   ? "x64"
                        : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64"
                                                                                     : "other");
  j.KvS("cpu", CpuBrand());
  j.KvI("logical_cpus", static_cast<long long>(si.dwNumberOfProcessors));
  j.EndObject();

  j.Key("adapters");
  j.BeginArray();
  for (const AdapterInfo& a : g_adapters) {
    j.BeginObject();
    j.KvS("description", a.description);
    j.KvS("vendor_id", Hex32(a.vendor_id));
    j.KvS("device_id", Hex32(a.device_id));
    j.KvI("dedicated_memory_mb", static_cast<long long>(a.dedicated_mb));
    j.KvI("shared_memory_mb", static_cast<long long>(a.shared_mb));
    j.KvB("software_adapter", a.software);
    j.KvS("driver_version", a.driver_version);
    j.EndObject();
  }
  j.EndArray();

  j.Key("media_foundation");
  j.BeginObject();
  j.KvB("dll_present", mf_dll);
  j.KvS("startup_hresult", HrStr(mf_hr));
  j.KvB("available", mf_ok);
  j.EndObject();

  // registered_current_user is what the application process sees (the packages are
  // registered per user). provisioned_system_image needs elevation to read; see
  // hw-inventory.ps1, which reports it.
  j.Key("hevc_video_extensions");
  j.BeginObject();
  j.KvB("registered_current_user", !ext_packages.empty());
  j.Key("family_names_checked");
  j.BeginArray();
  for (const wchar_t* fam : kExtFamilies) j.Str(Utf8(fam));
  j.EndArray();
  j.Key("packages_registered_current_user");
  j.BeginArray();
  for (const std::string& n : ext_packages) j.Str(n);
  j.EndArray();
  j.KvS("provisioned_system_image", "unknown (needs elevation, see hw-inventory.ps1)");
  j.KvB("software_decoder_mft_present", sw_dec_present);
  j.EndObject();

  for (int pass = 0; pass < 2; ++pass) {
    const std::vector<MftInfo>& list = pass == 0 ? encoders : decoders;
    const std::map<std::string, int>& counts = pass == 0 ? enc_counts : dec_counts;
    j.Key(pass == 0 ? "encoders" : "decoders");
    j.BeginObject();
    j.Key("enumeration_counts");
    j.BeginObject();
    for (const auto& kv : counts) j.KvI(kv.first.c_str(), kv.second);
    j.EndObject();
    j.Key("mfts");
    j.BeginArray();
    for (const MftInfo& m : list) WriteMft(j, m);
    j.EndArray();
    j.EndObject();
  }

  j.Key("tests");
  j.BeginArray();
  for (const TestResult& t : tests) WriteTest(j, t);
  j.EndArray();

  j.Key("summary");
  j.BeginObject();
  const std::string pre = std::string(g_codec_name) + "_";
  std::vector<std::pair<std::string, std::string>> reasons;
  auto kb = [&](const char* suffix, bool v) { j.KvB((pre + suffix).c_str(), v); };
  auto kv = [&](const char* suffix, const Verdict& v) {
    j.KvS((pre + suffix).c_str(), v.status);
    if (v.status != "ok" && !v.reason.empty()) reasons.emplace_back(pre + suffix, v.reason);
  };
  kb("hardware_encoder_present", hw_enc_present);
  kb("software_encoder_present", sw_enc_present);
  kb("hardware_decoder_present", hw_dec_present);
  kb("software_decoder_present", sw_dec_present);
  kv("hardware_encode_720p_status", v_hw_enc_720);
  kv("hardware_encode_1080p_status", v_hw_enc_1080);
  kv("any_encode_720p_status", v_enc_720);
  kv("any_encode_1080p_status", v_enc_1080);
  j.KvN("best_encode_fps_720p", fps_720);
  j.KvN("best_encode_fps_1080p", fps_1080);
  j.Key("best_encoder");
  if (best_enc) {
    j.Str(best_enc->name);
  } else {
    j.Null();
  }
  j.Key("stream_source_720p");
  if (res[0].stream.frames) {
    j.Str(res[0].stream.source);
  } else {
    j.Null();
  }
  j.Key("stream_source_1080p");
  if (res[1].stream.frames) {
    j.Str(res[1].stream.source);
  } else {
    j.Null();
  }
  kv("decode_720p_status", v_dec_720);
  kv("decode_1080p_status", v_dec_1080);
  kv("dxva_decode_720p_status", v_dxva_720);
  kv("dxva_decode_1080p_status", v_dxva_1080);
  kv("hardware_mft_decode_720p_status", v_hwmft_720);
  kv("hardware_mft_decode_1080p_status", v_hwmft_1080);
  kv("hardware_accelerated_decode_status", v_hw_accel);
  kv("decoder_configure_only_status", v_cfg);
  j.KvB("video_send_possible", v_hw_enc_720.status == "ok");
  j.KvB("video_send_1080p_possible", v_hw_enc_1080.status == "ok");
  j.KvB("video_receive_possible", receive_possible);
  j.KvS("video_receive_basis", receive_basis);
  j.Key("reasons");
  j.BeginObject();
  for (const auto& rs : reasons) j.KvS(rs.first.c_str(), rs.second);
  j.EndObject();
  j.EndObject();
  j.EndObject();
  std::string text = j.Text();
  text += "\n";

  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);

  // Report file next to the executable; fall back to the working directory.
  std::wstring dir(MAX_PATH, L'\0');
  DWORD n = GetModuleFileNameW(nullptr, &dir[0], static_cast<DWORD>(dir.size()));
  std::wstring full;
  if (n > 0 && n < dir.size()) {
    dir.resize(n);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) full = dir.substr(0, slash + 1) + out_name;
  }
  bool written = !full.empty() && WriteWholeFile(full, text);
  if (!written) written = WriteWholeFile(out_name, text);
  std::fprintf(stderr, "[hevc-probe] report file %s\n",
               written ? "written next to the executable" : "NOT written (read-only location)");

  g_run_finished = true;
  if (pause) {
    std::fprintf(stderr, "Press Enter to close.\n");
    std::getchar();
  }
  std::fflush(stderr);
  // A hung driver thread must not keep the process alive.
  ExitProcess(ci && crashed_or_timed_out ? 4 : 0);
}
