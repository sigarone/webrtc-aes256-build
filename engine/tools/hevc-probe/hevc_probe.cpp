// hevc-probe: a small Windows console tool that reports what the machine can do
// with HEVC (H.265) through Media Foundation.
//
// It lists the HEVC encoder and decoder MFTs (hardware / software, async / sync,
// vendor, friendly name), the DXGI adapters, whether the HEVC Video Extensions
// are installed, and then runs real encode and decode tests on synthetic NV12
// frames (1280x720 and 1920x1080 at 30 fps). The result is one JSON document,
// written to stdout and to a file next to the executable.
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
#include <chrono>
#include <cstdint>
#include <cstdio>
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
constexpr UINT32 kH265Main420x8 = 1;  // eAVEncH265VProfile_Main_420_8

// The codec under test. HEVC is the point of the probe; "--codec h264" runs the
// very same pipeline on H.264 and is used by CI as a self-test of the encode and
// decode machinery (the hosted runner has no HEVC MFT at all).
GUID g_subtype = MFVideoFormat_HEVC;
bool g_is_hevc = true;
const char* g_codec_name = "hevc";

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

ComPtr<IDXGIFactory1> g_factory;
std::vector<AdapterInfo> g_adapters;

void ListAdapters() {
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g_factory)))) return;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (g_factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d{};
    if (FAILED(a->GetDesc1(&d))) continue;
    AdapterInfo ai;
    ai.description = Utf8(d.Description);
    ai.vendor_id = d.VendorId;
    ai.device_id = d.DeviceId;
    ai.dedicated_mb = d.DedicatedVideoMemory / (1024ull * 1024ull);
    ai.shared_mb = d.SharedSystemMemory / (1024ull * 1024ull);
    ai.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    LARGE_INTEGER umd{};
    if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
      char b[48];
      std::snprintf(b, sizeof b, "%u.%u.%u.%u", HIWORD(umd.HighPart),
                    LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                    LOWORD(umd.LowPart));
      ai.driver_version = b;
    }
    g_adapters.push_back(ai);
  }
}

// Picks the adapter whose vendor id matches the MFT vendor id string
// ("VEN_10DE"); falls back to the first non-software adapter, then null.
ComPtr<IDXGIAdapter1> PickAdapter(unsigned vendor_id) {
  if (!g_factory) return nullptr;
  ComPtr<IDXGIAdapter1> fallback;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (g_factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d{};
    if (FAILED(a->GetDesc1(&d))) continue;
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (vendor_id && d.VendorId == vendor_id) return a;
    if (!fallback) fallback = a;
  }
  return fallback;
}

ComPtr<ID3D11Device> MakeD3DDevice(unsigned vendor_id, HRESULT* hr_out) {
  ComPtr<IDXGIAdapter1> ad = PickAdapter(vendor_id);
  if (!ad) {
    *hr_out = DXGI_ERROR_NOT_FOUND;
    return nullptr;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL got{};
  HRESULT hr = D3D11CreateDevice(
      ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
      levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &got, &ctx);
  *hr_out = hr;
  if (FAILED(hr)) return nullptr;
  ComPtr<ID3D11Multithread> mt;
  if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
  return dev;
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
  ComPtr<IMFActivate> activate;
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
        mi.activate = act;
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

// ---------------------------------------------------------------- results

struct TestResult {
  std::string role;       // "encode" | "decode"
  std::string kind;       // "full" | "configure_only"
  std::string mft;
  bool hardware = false;
  int width = 0;
  int height = 0;
  bool use_d3d = false;
  std::string mode;       // "async" | "sync"
  bool ok = false;
  std::string stage;      // where it stopped on failure
  HRESULT hr = S_OK;
  std::string error;
  int frames_in = 0;
  int frames_out = 0;
  unsigned long long bytes = 0;
  double seconds = 0;
  double fps = 0;
  std::vector<std::string> notes;
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

// Activates the MFT and prepares async unlock / D3D manager. On failure fills r.
struct Prepared {
  ComPtr<IMFTransform> mft;
  ComPtr<IMFMediaEventGenerator> gen;
  ComPtr<ID3D11Device> dev;
  ComPtr<IMFDXGIDeviceManager> mgr;
  bool d3d_aware = false;
};

bool PrepareMft(const MftInfo& mi, bool use_d3d, TestResult* r, Prepared* p) {
  HRESULT hr = mi.activate->ActivateObject(IID_PPV_ARGS(&p->mft));
  if (FAILED(hr)) {
    r->stage = "activate";
    r->hr = hr;
    return false;
  }
  // Make the next ActivateObject on the same activation object create a fresh
  // MFT instead of handing out this one again.
  mi.activate->DetachObject();
  ComPtr<IMFAttributes> attrs;
  UINT32 is_async = 0;
  if (SUCCEEDED(p->mft->GetAttributes(&attrs)) && attrs) {
    attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
    if (is_async) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    UINT32 aware = 0;
    if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware))) p->d3d_aware = aware != 0;
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
  }
  r->mode = is_async ? "async" : "sync";
  if (is_async) {
    hr = p->mft.As(&p->gen);
    if (FAILED(hr)) {
      r->stage = "query_event_generator";
      r->hr = hr;
      return false;
    }
  }
  if (use_d3d) {
    HRESULT dh = S_OK;
    p->dev = MakeD3DDevice(mi.vendor_id, &dh);
    if (!p->dev) {
      r->stage = "create_d3d_device";
      r->hr = dh;
      return false;
    }
    UINT token = 0;
    hr = MFCreateDXGIDeviceManager(&token, &p->mgr);
    if (SUCCEEDED(hr)) hr = p->mgr->ResetDevice(p->dev.Get(), token);
    if (SUCCEEDED(hr)) {
      hr = p->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                  reinterpret_cast<ULONG_PTR>(p->mgr.Get()));
    }
    if (FAILED(hr)) {
      r->stage = "set_d3d_manager";
      r->hr = hr;
      return false;
    }
    r->use_d3d = true;
  }
  return true;
}

// ---------------------------------------------------------------- encode

EncodeOut DoEncode(const MftInfo& mi, int w, int h, bool use_d3d) {
  EncodeOut out;
  TestResult& r = out.r;
  r.role = "encode";
  r.kind = "full";
  r.mft = mi.name;
  r.hardware = mi.hardware;
  r.width = w;
  r.height = h;
  Prepared p;
  if (!PrepareMft(mi, use_d3d, &r, &p)) return out;
  IMFTransform* mft = p.mft.Get();
  const UINT32 bitrate = (w >= 1920) ? 6000000u : 3000000u;

  // Encoder tuning is best effort: failures are recorded as notes only.
  ComPtr<ICodecAPI> api;
  if (SUCCEEDED(p.mft.As(&api))) {
    auto set_u32 = [&](const GUID& g, const char* label, UINT32 v) {
      VARIANT var;
      ZeroMemory(&var, sizeof var);
      var.vt = VT_UI4;
      var.ulVal = v;
      HRESULT hr = api->SetValue(&g, &var);
      if (FAILED(hr)) r.notes.push_back(std::string("codecapi ") + label + " " + HrStr(hr));
    };
    set_u32(CODECAPI_AVEncCommonRateControlMode, "rate_control", eAVEncCommonRateControlMode_CBR);
    set_u32(CODECAPI_AVEncCommonMeanBitRate, "mean_bitrate", bitrate);
    set_u32(CODECAPI_AVEncMPVGOPSize, "gop", kFps);
    VARIANT var;
    ZeroMemory(&var, sizeof var);
    var.vt = VT_BOOL;
    var.boolVal = VARIANT_TRUE;
    HRESULT hr = api->SetValue(&CODECAPI_AVLowLatencyMode, &var);
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
  HRESULT hr = mft->SetOutputType(0, ot.Get(), 0);
  if (FAILED(hr)) {
    r.stage = "set_output_type";
    r.hr = hr;
    return out;
  }

  bool in_ok = false;
  for (DWORD i = 0; !in_ok; ++i) {
    ComPtr<IMFMediaType> t;
    if (FAILED(mft->GetInputAvailableType(0, i, &t))) break;
    GUID st{};
    if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &st)) || st != MFVideoFormat_NV12) continue;
    SetVideoCommon(t.Get(), w, h);
    if (SUCCEEDED(mft->SetInputType(0, t.Get(), 0))) in_ok = true;
  }
  if (!in_ok) {
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    SetVideoCommon(t.Get(), w, h);
    hr = mft->SetInputType(0, t.Get(), 0);
    if (FAILED(hr)) {
      r.stage = "set_input_type";
      r.hr = hr;
      return out;
    }
  }

  MFT_INPUT_STREAM_INFO isi{};
  mft->GetInputStreamInfo(0, &isi);
  const DWORD align = isi.cbAlignment;

  mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  if (SUCCEEDED(hr)) hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (FAILED(hr)) {
    r.stage = "begin_streaming";
    r.hr = hr;
    return out;
  }

  Pump pump;
  pump.mft = mft;
  pump.gen = p.gen.Get();
  pump.total = kFrames;
  pump.default_out_size = static_cast<DWORD>(w) * h;
  pump.make_input = [&](int idx, IMFSample** s) -> HRESULT {
    ComPtr<IMFSample> c;
    HRESULT e = MakeNv12Sample(w, h, idx, align, &c);
    if (FAILED(e)) return e;
    *s = c.Detach();
    return S_OK;
  };
  pump.on_output = [&](IMFSample* s) {
    ComPtr<IMFMediaBuffer> b;
    if (FAILED(s->ConvertToContiguousBuffer(&b))) return;
    BYTE* d = nullptr;
    DWORD len = 0;
    if (FAILED(b->Lock(&d, nullptr, &len))) return;
    EncFrame f;
    f.data.assign(d, d + len);
    b->Unlock();
    s->GetSampleTime(&f.time);
    r.bytes += len;
    out.frames.push_back(std::move(f));
  };
  pump.renegotiate_output = [&]() -> HRESULT {
    // Encoders rarely change the output type; re-apply ours if asked.
    return mft->SetOutputType(0, ot.Get(), 0);
  };
  const bool ok = pump.Run();
  r.frames_in = pump.fed;
  r.frames_out = pump.produced;
  r.seconds = pump.seconds;
  if (pump.seconds > 0) r.fps = pump.fed / pump.seconds;
  if (!ok) {
    r.stage = pump.stage;
    r.hr = pump.hr;
    return out;
  }
  if (pump.produced == 0) {
    r.stage = "no_output";
    r.hr = E_FAIL;
    return out;
  }
  // Keep the sequence header (if the MFT publishes one) for the decoder test.
  ComPtr<IMFMediaType> cur;
  if (SUCCEEDED(mft->GetOutputCurrentType(0, &cur))) {
    UINT32 sz = 0;
    if (SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz > 0) {
      out.seq_header.resize(sz);
      cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, out.seq_header.data(), sz, nullptr);
    }
  }
  mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  r.ok = true;
  if (r.frames_out != kFrames) {
    r.notes.push_back("frames_out differs from frames_in");
  }
  return out;
}

// ---------------------------------------------------------------- decode

TestResult DoDecode(const MftInfo& mi, int w, int h, const std::vector<EncFrame>* stream,
                    const std::vector<uint8_t>* seq_header, bool use_d3d) {
  TestResult r;
  r.role = "decode";
  r.kind = stream ? "full" : "configure_only";
  r.mft = mi.name;
  r.hardware = mi.hardware;
  r.width = w;
  r.height = h;
  Prepared p;
  if (!PrepareMft(mi, use_d3d, &r, &p)) return r;
  IMFTransform* mft = p.mft.Get();

  ComPtr<IMFMediaType> it;
  MFCreateMediaType(&it);
  it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  it->SetGUID(MF_MT_SUBTYPE, g_subtype);
  SetVideoCommon(it.Get(), w, h);
  if (seq_header && !seq_header->empty()) {
    it->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seq_header->data(),
                static_cast<UINT32>(seq_header->size()));
  }
  HRESULT hr = mft->SetInputType(0, it.Get(), 0);
  if (FAILED(hr)) {
    r.stage = "set_input_type";
    r.hr = hr;
    return r;
  }
  // Some decoders only publish their output types after the first frame; a
  // failure here is therefore not fatal for the full test.
  HRESULT out_hr = PickOutputNv12(mft);
  if (FAILED(out_hr)) r.notes.push_back("output type deferred " + HrStr(out_hr));

  mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  if (SUCCEEDED(hr)) hr = mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (FAILED(hr)) {
    r.stage = "begin_streaming";
    r.hr = hr;
    return r;
  }
  if (!stream) {
    // No bitstream available (no working encoder on this machine): report that
    // the decoder could be created and configured for HEVC input.
    r.ok = true;
    return r;
  }

  Pump pump;
  pump.mft = mft;
  pump.gen = p.gen.Get();
  pump.total = static_cast<int>(stream->size());
  pump.default_out_size = static_cast<DWORD>(w) * h * 3 / 2;
  pump.make_input = [&](int idx, IMFSample** s) -> HRESULT {
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
    ComPtr<IMFSample> smp;
    e = MFCreateSample(&smp);
    if (FAILED(e)) return e;
    smp->AddBuffer(b.Get());
    smp->SetSampleTime(f.time);
    smp->SetSampleDuration(10000000LL / kFps);
    *s = smp.Detach();
    return S_OK;
  };
  pump.on_output = [&](IMFSample*) {};
  pump.renegotiate_output = [&]() -> HRESULT { return PickOutputNv12(mft); };
  const bool ok = pump.Run();
  r.frames_in = pump.fed;
  r.frames_out = pump.produced;
  r.seconds = pump.seconds;
  if (pump.seconds > 0) r.fps = pump.produced / pump.seconds;
  if (!ok) {
    r.stage = pump.stage;
    r.hr = pump.hr;
    return r;
  }
  if (pump.produced == 0) {
    r.stage = "no_output";
    r.hr = E_FAIL;
    return r;
  }
  mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  r.ok = true;
  if (r.frames_out != r.frames_in) r.notes.push_back("frames_out differs from frames_in");
  return r;
}

// ---------------------------------------------------------------- watchdog

TestResult& Result(TestResult& r) { return r; }
TestResult& Result(EncodeOut& e) { return e.r; }

void MarkCrash(TestResult& r, unsigned long code) {
  r.ok = false;
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
  std::thread([prom, f]() mutable {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    R res{};
    SehInfo info;
    std::function<void()> body = [&] { res = f(); };
    if (!SafeInvoke(body, &info)) {
      res = R{};
      MarkCrash(Result(res), info.code);
    }
    prom->set_value(std::move(res));
  }).detach();
  if (fut.wait_for(std::chrono::milliseconds(kTestTimeoutMs)) != std::future_status::ready) {
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

// Package full names of the HEVC Video Extensions for the current user. The
// names carry the package version and architecture, nothing user specific.
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
  j.KvS("mode", t.mode);
  j.KvB("d3d11_manager", t.use_d3d);
  j.KvB("ok", t.ok);
  if (!t.ok) {
    j.KvS("failed_stage", t.stage);
    j.KvS("hresult", HrStr(t.hr));
    if (!t.error.empty()) j.KvS("error", t.error);
  }
  j.KvI("frames_in", t.frames_in);
  j.KvI("frames_out", t.frames_out);
  j.KvI("bytes_out", static_cast<long long>(t.bytes));
  j.KvN("seconds", t.seconds);
  j.KvN("fps", t.fps);
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
  o.r.use_d3d = d3d;
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
  r.use_d3d = d3d;
  r.stage = "timeout";
  r.hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  r.error = "no result within the watchdog time";
  return r;
}

void Progress(const char* what, const std::string& mft, const char* extra) {
  std::fprintf(stderr, "[hevc-probe] %s: %s %s\n", what, mft.c_str(), extra);
}

struct Stream {
  std::shared_ptr<std::vector<EncFrame>> frames;
  std::shared_ptr<std::vector<uint8_t>> seq;
  bool d3d = false;
};

// Encodes with one MFT, trying the D3D11 manager first for hardware MFTs.
// Appends every attempt to `log`; returns true on the first success.
bool EncodeAttempts(const MftInfo& mi, int w, int h, std::vector<TestResult>* log,
                    Stream* stream) {
  std::vector<bool> modes = mi.hardware ? std::vector<bool>{true, false} : std::vector<bool>{false};
  for (bool d3d : modes) {
    Progress("encode", mi.name, d3d ? "(d3d11)" : "(system memory)");
    EncodeOut eo = WithWatchdog<EncodeOut>([=] { return DoEncode(mi, w, h, d3d); },
                                           TimeoutEnc(mi, w, h, d3d));
    log->push_back(eo.r);
    if (eo.r.ok) {
      stream->frames = std::make_shared<std::vector<EncFrame>>(std::move(eo.frames));
      stream->seq = std::make_shared<std::vector<uint8_t>>(std::move(eo.seq_header));
      stream->d3d = d3d;
      return true;
    }
  }
  return false;
}

bool DecodeAttempts(const MftInfo& mi, int w, int h, const Stream* s,
                    std::vector<TestResult>* log) {
  std::vector<bool> modes = mi.hardware ? std::vector<bool>{true, false} : std::vector<bool>{false};
  for (bool d3d : modes) {
    Progress("decode", mi.name, d3d ? "(d3d11)" : "(system memory)");
    const bool full = s != nullptr;
    std::shared_ptr<std::vector<EncFrame>> frames = s ? s->frames : nullptr;
    std::shared_ptr<std::vector<uint8_t>> seq = s ? s->seq : nullptr;
    TestResult r = WithWatchdog<TestResult>(
        [=] { return DoDecode(mi, w, h, frames.get(), seq.get(), d3d); },
        TimeoutDec(mi, w, h, d3d, full));
    log->push_back(r);
    if (r.ok) return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
  bool pause = false;
  std::wstring out_name = L"hevc-probe-report.json";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--pause") {
      pause = true;
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
                   "Writes a JSON report to stdout and to a file next to the exe.\n");
      return 0;
    }
  }

  _setmode(_fileno(stdout), _O_BINARY);  // the file and stdout carry identical bytes
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::fprintf(stderr, "[hevc-probe] start (%s)\n", g_codec_name);

  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  ListAdapters();

  const bool mf_dll = LoadLibraryW(L"mfplat.dll") != nullptr;
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

  std::vector<std::string> ext_packages;
  for (const wchar_t* fam : {L"Microsoft.HEVCVideoExtension_8wekyb3d8bbwe",
                             L"Microsoft.HEVCVideoExtensions_8wekyb3d8bbwe"}) {
    for (std::string& n : FindPackages(fam)) ext_packages.push_back(std::move(n));
  }

  // ---- encode tests
  std::vector<TestResult> tests;
  Stream s720, s1080;
  const MftInfo* best_enc = nullptr;
  bool best_d3d = false;
  int hw_tried = 0, sw_tried = 0;
  for (const MftInfo& mi : encoders) {
    if (mi.hardware) {
      if (hw_tried++ >= 4) continue;
    } else {
      // A software encoder is only tried when no hardware encoder worked.
      if (best_enc || sw_tried++ >= 2) continue;
    }
    Stream st;
    if (EncodeAttempts(mi, 1280, 720, &tests, &st) && !best_enc) {
      best_enc = &mi;
      best_d3d = st.d3d;
      s720 = st;
    }
  }
  if (best_enc) {
    Progress("encode 1080p", best_enc->name, "");
    const MftInfo enc_copy = *best_enc;
    const bool d3d = best_d3d;
    EncodeOut eo = WithWatchdog<EncodeOut>(
        [=] { return DoEncode(enc_copy, 1920, 1080, d3d); },
        TimeoutEnc(enc_copy, 1920, 1080, d3d));
    tests.push_back(eo.r);
    if (eo.r.ok) {
      s1080.frames = std::make_shared<std::vector<EncFrame>>(std::move(eo.frames));
      s1080.seq = std::make_shared<std::vector<uint8_t>>(std::move(eo.seq_header));
      s1080.d3d = d3d;
    }
  }

  // ---- decode tests
  int dec_tried = 0;
  bool any_decode_720 = false, any_hw_decode_720 = false, any_decode_1080 = false;
  bool any_decoder_configures = false;
  const bool have_stream = s720.frames != nullptr;
  for (const MftInfo& mi : decoders) {
    if (dec_tried++ >= 6) break;
    if (!DecodeAttempts(mi, 1280, 720, have_stream ? &s720 : nullptr, &tests)) continue;
    any_decoder_configures = true;
    if (!have_stream) continue;
    any_decode_720 = true;
    if (mi.hardware) any_hw_decode_720 = true;
    if (s1080.frames && DecodeAttempts(mi, 1920, 1080, &s1080, &tests)) any_decode_1080 = true;
  }

  // ---- summary values
  bool hw_enc_present = false, sw_enc_present = false;
  for (const MftInfo& m : encoders) (m.hardware ? hw_enc_present : sw_enc_present) = true;
  bool hw_dec_present = false, sw_dec_present = false;
  for (const MftInfo& m : decoders) (m.hardware ? hw_dec_present : sw_dec_present) = true;
  bool hw_enc_720 = false, hw_enc_1080 = false, enc_720 = false, enc_1080 = false;
  double fps_720 = 0, fps_1080 = 0;
  for (const TestResult& t : tests) {
    if (t.role != "encode" || !t.ok) continue;
    if (t.width == 1280) {
      enc_720 = true;
      if (t.hardware) hw_enc_720 = true;
      fps_720 = std::max(fps_720, t.fps);
    } else {
      enc_1080 = true;
      if (t.hardware) hw_enc_1080 = true;
      fps_1080 = std::max(fps_1080, t.fps);
    }
  }

  Json j;
  j.BeginObject();
  j.Key("tool");
  j.BeginObject();
  j.KvS("name", "hevc-probe");
  j.KvI("report_version", 1);
  j.KvS("codec", g_codec_name);
  j.KvS("git_sha", PROBE_GIT_SHA);
  j.KvS("test_content", "synthetic NV12, 60 frames, 30 fps, 1280x720 and 1920x1080");
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

  j.Key("hevc_video_extensions");
  j.BeginObject();
  j.KvB("package_installed", !ext_packages.empty());
  j.Key("packages");
  j.BeginArray();
  for (const std::string& n : ext_packages) j.Str(n);
  j.EndArray();
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
  auto kb = [&](const char* suffix, bool v) { j.KvB((pre + suffix).c_str(), v); };
  kb("hardware_encoder_present", hw_enc_present);
  kb("software_encoder_present", sw_enc_present);
  kb("hardware_decoder_present", hw_dec_present);
  kb("software_decoder_present", sw_dec_present);
  kb("hardware_encode_720p_ok", hw_enc_720);
  kb("hardware_encode_1080p_ok", hw_enc_1080);
  kb("any_encode_720p_ok", enc_720);
  kb("any_encode_1080p_ok", enc_1080);
  j.KvN("best_encode_fps_720p", fps_720);
  j.KvN("best_encode_fps_1080p", fps_1080);
  j.Key("best_encoder");
  if (best_enc) {
    j.Str(best_enc->name);
  } else {
    j.Null();
  }
  kb("decode_720p_ok", any_decode_720);
  kb("hardware_decode_720p_ok", any_hw_decode_720);
  kb("decode_1080p_ok", any_decode_1080);
  j.KvB("decoder_configure_only_ok", !have_stream && any_decoder_configures);
  j.KvB("video_send_possible", hw_enc_720);
  j.KvB("video_receive_possible", any_decode_720 || (!have_stream && any_decoder_configures));
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

  if (pause) {
    std::fprintf(stderr, "Press Enter to close.\n");
    std::getchar();
  }
  std::fflush(stderr);
  // A hung driver thread must not keep the process alive.
  ExitProcess(0);
}
