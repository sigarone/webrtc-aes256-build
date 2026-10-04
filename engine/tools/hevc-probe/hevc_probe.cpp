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
// Two more things are reported besides the MFT tests. First, which MFTs MFTEnumEx
// returns with and without MFT_ENUM_FLAG_UNTRUSTED_STOREMFT (and unfiltered), so that a
// codec that is installed as a Microsoft Store package but not enumerated by default can
// be seen, and decoded with when it is returned. Second, what the D3D11 video device of
// each adapter offers directly (ID3D11VideoDevice: decoder profiles, output formats,
// decoder configurations, and one decoder object per supported profile, created and
// released at once): whether the GPU can decode a codec through D3D11VA without any MFT.
// Both are read-only capability probes.
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
#include <cctype>
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
// How long the teardown of one attempt may take after its result was published. A
// call that hangs there (an MFT that blocks in a shutdown message) must not turn an
// attempt that worked into a timeout, so the result is taken first and the teardown is
// waited for separately.
constexpr DWORD kTeardownWaitMs = 10000;
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
  // Which enumeration variant (kVariants) returned it first, and every one that did. An
  // MFT that the default enumeration does not return is only tested through the variant
  // that returned it, and the report says which class it is in (see MftEnumeration).
  int variant = 0;
  std::vector<std::string> seen_in;
  std::string mft_class = "default";
};

unsigned ParseVendorId(const std::string& s) {
  size_t p = s.find("VEN_");
  if (p == std::string::npos) return 0;
  return static_cast<unsigned>(std::strtoul(s.c_str() + p + 4, nullptr, 16));
}

// MFT_ENUM_FLAG_UNTRUSTED_STOREMFT. Per docs: the constant is listed in _MFT_ENUM_FLAG with the
// value 0x00000400, and Microsoft Learn gives it no description (the pages for _MFT_ENUM_FLAG
// and MFTEnumEx were read on 2026-10-04). What it does is not assumed: the probe enumerates
// with and without it and reports both. The value is written out here so that the probe does
// not depend on the SDK that builds it.
constexpr UINT32 kEnumFlagUntrustedStoreMft = 0x00000400u;

// The enumerations the probe runs for the codec, in this order. The first one is what every
// earlier version of the probe did. Per docs (MFTEnumEx, "Registering and Enumerating MFTs"),
// the default enumeration excludes MFTs with field-of-use restrictions, transcode-only MFTs and
// local MFTs, and MFT_ENUM_FLAG_SORTANDFILTER drops blocked MFTs; the unfiltered variants lift
// exactly those. Whether the store flag adds MFTs is what the lab measures.
struct EnumVariant {
  const char* label;
  UINT32 extra_flags;  // ORed into every query of the variant
  bool all;            // one query with MFT_ENUM_FLAG_ALL instead of hardware, async, sync
  bool sort;           // MFT_ENUM_FLAG_SORTANDFILTER
  bool store;          // carries MFT_ENUM_FLAG_UNTRUSTED_STOREMFT
};

const EnumVariant kVariants[] = {
    {"default", 0, false, true, false},
    {"store_flag", kEnumFlagUntrustedStoreMft, false, true, true},
    {"unfiltered_all", 0, true, false, false},
    {"unfiltered_all_store_flag", kEnumFlagUntrustedStoreMft, true, false, true},
};
constexpr int kVariantCount = static_cast<int>(sizeof kVariants / sizeof kVariants[0]);

struct EnumQuery {
  const char* label;
  UINT32 flags;
};

std::vector<EnumQuery> QueriesOf(const EnumVariant& v) {
  const UINT32 base = v.extra_flags | (v.sort ? static_cast<UINT32>(MFT_ENUM_FLAG_SORTANDFILTER) : 0u);
  std::vector<EnumQuery> q;
  if (v.all) {
    q.push_back({"all", static_cast<UINT32>(MFT_ENUM_FLAG_ALL) | base});
  } else {
    q.push_back({"hardware", static_cast<UINT32>(MFT_ENUM_FLAG_HARDWARE) | base});
    q.push_back({"async", static_cast<UINT32>(MFT_ENUM_FLAG_ASYNCMFT) | base});
    q.push_back({"sync", static_cast<UINT32>(MFT_ENUM_FLAG_SYNCMFT) | base});
  }
  return q;
}

// One enumeration variant. counts: per query (the number MFTEnumEx returned, -1 when the call
// failed); errors: the HRESULT of a failed query.
std::vector<MftInfo> EnumMfts(const GUID& category, const GUID& subtype, bool subtype_is_output,
                              int variant, std::map<std::string, int>* counts,
                              std::map<std::string, std::string>* errors) {
  const std::vector<EnumQuery> queries = QueriesOf(kVariants[variant]);
  std::vector<MftInfo> list;
  std::map<std::string, size_t> by_clsid;
  MFT_REGISTER_TYPE_INFO ti{MFMediaType_Video, subtype};
  for (const EnumQuery& q : queries) {
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFTEnumEx(category, q.flags, subtype_is_output ? nullptr : &ti,
                           subtype_is_output ? &ti : nullptr, &acts, &n);
    if (counts) (*counts)[q.label] = SUCCEEDED(hr) ? static_cast<int>(n) : -1;
    if (FAILED(hr)) {
      if (errors) (*errors)[q.label] = HrStr(hr);
      continue;
    }
    if (!acts) n = 0;  // a count without an array: nothing to read
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
        mi.variant = variant;
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
// order and with the same flags as the EnumMfts variant that returned it (so that a
// hardware MFT keeps the hardware binding of its first query, and an MFT that only the
// store flag returns is found again through that flag). Never reuses an activation
// object of an earlier attempt.
ComPtr<IMFActivate> FreshActivate(const MftInfo& mi, HRESULT* hr_out) {
  const std::vector<EnumQuery> queries = QueriesOf(kVariants[mi.variant]);
  MFT_REGISTER_TYPE_INFO ti{MFMediaType_Video, mi.subtype};
  HRESULT last = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  ComPtr<IMFActivate> found;
  for (const EnumQuery& q : queries) {
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFTEnumEx(mi.category, q.flags, mi.subtype_is_output ? nullptr : &ti,
                           mi.subtype_is_output ? &ti : nullptr, &acts, &n);
    if (FAILED(hr)) {
      last = hr;
      continue;
    }
    if (!acts) n = 0;
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

// What one enumeration variant returned.
struct VariantResult {
  std::string label;
  std::map<std::string, int> counts;
  std::map<std::string, std::string> errors;
  std::vector<MftInfo> mfts;
};

// All the variants for one category and codec. `extras` are the MFTs that a variant other than
// the default one returned and the default one did not, each once, with the class it is in:
//   store_flag_only    returned only by variants that carry MFT_ENUM_FLAG_UNTRUSTED_STOREMFT
//   needs_other_flags  returned by a variant without the store flag (field-of-use, transcode-only,
//                      local or filtered out by default), not by the default one
struct MftEnumeration {
  std::vector<VariantResult> variants;
  std::vector<MftInfo> extras;
};

// The part of the enumeration that needs no Media Foundation: which variants returned which MFT,
// and the MFTs that only a variant other than the default one returned, in the order they were
// found, each once, with its class. CI runs it on fabricated variants (--selftest-d3d11va-logic).
MftEnumeration BuildEnumeration(std::vector<VariantResult> variants) {
  MftEnumeration e;
  e.variants = std::move(variants);
  std::map<std::string, std::vector<int>> seen;  // clsid -> variants that returned it
  for (size_t vi = 0; vi < e.variants.size(); ++vi) {
    for (MftInfo& m : e.variants[vi].mfts) {
      m.variant = static_cast<int>(vi);
      seen[m.clsid].push_back(static_cast<int>(vi));
    }
  }
  for (VariantResult& vr : e.variants) {
    for (MftInfo& m : vr.mfts) {
      m.seen_in.clear();
      for (int vi : seen[m.clsid]) m.seen_in.push_back(kVariants[vi].label);
    }
  }
  for (size_t vi = 1; vi < e.variants.size(); ++vi) {
    for (const MftInfo& m : e.variants[vi].mfts) {
      const std::vector<int>& in = seen[m.clsid];
      if (std::find(in.begin(), in.end(), 0) != in.end()) continue;  // the default one has it
      bool queued = false;
      for (const MftInfo& x : e.extras) queued = queued || x.clsid == m.clsid;
      if (queued) continue;
      MftInfo x = m;
      bool without_store_flag = false;
      for (int v : in) without_store_flag = without_store_flag || !kVariants[v].store;
      x.mft_class = without_store_flag ? "needs_other_flags" : "store_flag_only";
      e.extras.push_back(std::move(x));
    }
  }
  return e;
}

MftEnumeration EnumAllVariants(const GUID& category, const GUID& subtype, bool subtype_is_output) {
  std::vector<VariantResult> variants;
  for (int vi = 0; vi < kVariantCount; ++vi) {
    VariantResult vr;
    vr.label = kVariants[vi].label;
    vr.mfts = EnumMfts(category, subtype, subtype_is_output, vi, &vr.counts, &vr.errors);
    variants.push_back(std::move(vr));
  }
  return BuildEnumeration(std::move(variants));
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
  std::string role;       // "encode" | "decode" | "d3d11va_capability"
  std::string kind;       // "full" | "configure_only" | "capability"
  std::string mft;
  std::string clsid;
  std::string mft_class = "default";  // see MftEnumeration: default | store_flag_only | needs_other_flags
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
  bool activation_refused = false;  // ActivateObject failed (the MFT was enumerated, but not created)
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
    r->activation_refused = true;
    if (hr == E_ACCESSDENIED) {
      r->notes.push_back(
          "ActivateObject was refused with E_ACCESSDENIED: the MFT was enumerated but this process "
          "may not load it");
    }
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

// What one attempt hands back: its result, and the teardown that still has to run. The
// watchdog publishes the result first and runs the teardown afterwards (see WithWatchdog).
template <typename R>
struct Attempt {
  R result;
  std::function<void(TestResult*)> cleanup;
};

Attempt<EncodeOut> DoEncode(const MftInfo& mi, int w, int h, bool use_d3d) {
  Attempt<EncodeOut> a;
  EncodeOut& out = a.result;
  TestResult& r = out.r;
  r.role = "encode";
  r.kind = "full";
  r.mft = mi.name;
  r.clsid = mi.clsid;
  r.mft_class = mi.mft_class;
  r.hardware = mi.hardware;
  r.width = w;
  r.height = h;
  std::shared_ptr<Session> s = std::make_shared<Session>();
  RunEncode(mi, w, h, use_d3d, out, *s);
  a.cleanup = [s](TestResult* td) { s->Teardown(td); };
  return a;
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

Attempt<TestResult> DoDecode(const MftInfo& mi, int w, int h, const std::vector<EncFrame>* stream,
                             const std::vector<uint8_t>* seq_header, bool use_d3d,
                             int adapter_ordinal, const std::string& stream_source) {
  Attempt<TestResult> a;
  TestResult& r = a.result;
  r.role = "decode";
  r.kind = stream ? "full" : "configure_only";
  r.mft = mi.name;
  r.clsid = mi.clsid;
  r.mft_class = mi.mft_class;
  r.hardware = mi.hardware;
  r.width = w;
  r.height = h;
  r.stream_source = stream_source;
  std::shared_ptr<Session> s = std::make_shared<Session>();
  RunDecode(mi, w, h, stream, seq_header, use_d3d, adapter_ordinal, r, *s);
  a.cleanup = [s](TestResult* td) { s->Teardown(td); };
  return a;
}

// ---------------------------------------------------------------- D3D11VA capabilities

// The decoder profiles of ID3D11VideoDevice. The GUIDs are written out here and not taken from
// d3d11.h: the SDK declares them as extern GUIDs (they would need dxguid.lib) and the newer
// ones only exist in newer SDKs. CI compares every value with the d3d11.h of the Windows SDK of
// the runner (hevc-probe.yml, "The decoder profile GUIDs agree with the Windows SDK").
struct VaProfileDef {
  const char* name;  // the D3D11_DECODER_PROFILE_<name> constant of d3d11.h, without the prefix
  GUID guid;
  bool probed;       // gets the full check; the others are only named when the driver lists them
  bool ten_bit;      // the output format is P010, otherwise NV12
};

const VaProfileDef kVaProfiles[] = {
    {"H264_VLD_NOFGT", {0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}}, true, false},
    {"HEVC_VLD_MAIN", {0x5b11d51b, 0x2f4c, 0x4452, {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}}, true, false},
    {"HEVC_VLD_MAIN10", {0x107af0e0, 0xef1a, 0x4d19, {0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13}}, true, true},
    {"VP9_VLD_PROFILE0", {0x463707f8, 0xa1d0, 0x4585, {0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e}}, true, false},
    {"VP9_VLD_10BIT_PROFILE2", {0xa4c749ef, 0x6ecf, 0x48aa, {0x84, 0x48, 0x50, 0xa7, 0xa1, 0x16, 0x5f, 0xf7}}, true, true},
    {"AV1_VLD_PROFILE0", {0xb8be4ccb, 0xcf53, 0x46ba, {0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a}}, true, false},
    {"HEVC_VLD_MONOCHROME", {0x0685b993, 0x3d8c, 0x43a0, {0x8b, 0x28, 0xd7, 0x4c, 0x2d, 0x68, 0x99, 0xa4}}, false, false},
    {"HEVC_VLD_MONOCHROME10", {0x142a1d0f, 0x69dd, 0x4ec9, {0x85, 0x91, 0xb1, 0x2f, 0xfc, 0xb9, 0x1a, 0x29}}, false, true},
    {"HEVC_VLD_MAIN12", {0x1a72925f, 0x0c2c, 0x4f15, {0x96, 0xfb, 0xb1, 0x7d, 0x14, 0x73, 0x60, 0x3f}}, false, true},
    {"HEVC_VLD_MAIN10_422", {0x0bac4fe5, 0x1532, 0x4429, {0xa8, 0x54, 0xf8, 0x4d, 0xe0, 0x49, 0x53, 0xdb}}, false, true},
    {"HEVC_VLD_MAIN12_422", {0x55bcac81, 0xf311, 0x4093, {0xa7, 0xd0, 0x1c, 0xbc, 0x0b, 0x84, 0x9b, 0xee}}, false, true},
    {"HEVC_VLD_MAIN_444", {0x4008018f, 0xf537, 0x4b36, {0x98, 0xcf, 0x61, 0xaf, 0x8a, 0x2c, 0x1a, 0x33}}, false, false},
    {"HEVC_VLD_MAIN10_EXT", {0x9cc55490, 0xe37c, 0x4932, {0x86, 0x84, 0x49, 0x20, 0xf9, 0xf6, 0x40, 0x9c}}, false, true},
    {"HEVC_VLD_MAIN10_444", {0x0dabeffa, 0x4458, 0x4602, {0xbc, 0x03, 0x07, 0x95, 0x65, 0x9d, 0x61, 0x7c}}, false, true},
    {"HEVC_VLD_MAIN12_444", {0x9798634d, 0xfe9d, 0x48e5, {0xb4, 0xda, 0xdb, 0xec, 0x45, 0xb3, 0xdf, 0x01}}, false, true},
    {"HEVC_VLD_MAIN16", {0xa4fbdbb0, 0xa113, 0x482b, {0xa2, 0x32, 0x63, 0x5c, 0xc0, 0x69, 0x7f, 0x6d}}, false, true},
    {"VP8_VLD", {0x90b899ea, 0x3a62, 0x4705, {0x88, 0xb3, 0x8d, 0xf0, 0x4b, 0x27, 0x44, 0xe7}}, false, false},
    {"AV1_VLD_PROFILE1", {0x6936ff0f, 0x45b1, 0x4163, {0x9c, 0xc1, 0x64, 0x6e, 0xf6, 0x94, 0x61, 0x08}}, false, false},
    {"AV1_VLD_PROFILE2", {0x0c5f2aa1, 0xe541, 0x4089, {0xbb, 0x7b, 0x98, 0x11, 0x0a, 0x19, 0xd7, 0xc8}}, false, false},
    {"AV1_VLD_12BIT_PROFILE2", {0x17127009, 0xa00f, 0x4ce1, {0x99, 0x4e, 0xbf, 0x40, 0x81, 0xf6, 0xf3, 0xf0}}, false, true},
    {"AV1_VLD_12BIT_PROFILE2_420", {0x2d80bed6, 0x9cac, 0x4835, {0x9e, 0x91, 0x32, 0x7b, 0xbc, 0x4f, 0x9e, 0xe8}}, false, true},
};

const VaProfileDef* FindVaProfile(const GUID& g) {
  for (const VaProfileDef& d : kVaProfiles) {
    if (d.guid == g) return &d;
  }
  return nullptr;
}

constexpr UINT kVaWidth1080 = 1920, kVaHeight1080 = 1080;
constexpr UINT kVaWidth2160 = 3840, kVaHeight2160 = 2160;
// A decoder object is made at 1920x1080 first and, when the driver refuses that, at the coded
// height of a 1080p picture (1088); the report lists every try.
constexpr UINT kVaHeight1088 = 1088;

struct VaFormat {
  bool asked = false;
  HRESULT hr = S_OK;
  bool supported = false;
};

struct VaConfig {
  bool asked = false;
  HRESULT hr = S_OK;
  UINT count = 0;
};

struct VaTry {
  std::string size;
  std::string stage;  // get_config | create_decoder
  HRESULT hr = S_OK;
};

// One capability check of one profile on one adapter.
struct VaProfileCheck {
  const VaProfileDef* def = nullptr;
  bool listed = false;
  VaFormat nv12, p010;
  VaConfig cfg1080, cfg2160;
  bool create_asked = false;
  bool created = false;
  std::vector<VaTry> tries;
  Outcome outcome = Outcome::kNotAttempted;
  std::string stage;   // failed: where it stopped
  HRESULT hr = S_OK;   // failed: the HRESULT of that stage (E_FAIL when the answer was "no")
  std::string reason;  // failed: in words
};

// The D3D11 video device of one adapter. `tr` carries the outcome of the adapter as a whole
// (ok: the profile list was read; failed: the device or the video device could not be made;
// not_attempted: software adapter that was not asked, or no video device), its steps and notes.
struct VaAdapterResult {
  TestResult tr;
  AdapterInfo adapter;
  std::string feature_level;
  bool video_flag_dropped = false;
  bool warp_stand_in = false;  // --ci on a machine that lists no adapter: the WARP rasterizer
  UINT profile_count = 0;
  std::vector<GUID> profiles;
  std::vector<VaProfileCheck> checks;
};

const char* FeatureLevelStr(D3D_FEATURE_LEVEL l) {
  switch (l) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    default: return "other";
  }
}

// Asks the D3D11 video device what it can decode. Read-only: it reads the profile list, asks the
// output formats and the configuration counts, and makes one decoder object per profile that
// looks supported, releasing it at once. No frame is decoded.
void CheckProfile(ID3D11VideoDevice* vd, const VaProfileDef& def, const std::vector<GUID>& listed,
                  VaProfileCheck* c) {
  c->def = &def;
  c->listed = std::find(listed.begin(), listed.end(), def.guid) != listed.end();
  if (!c->listed) {
    c->outcome = Outcome::kFailed;
    c->stage = "profile_not_listed";
    c->hr = E_FAIL;
    c->reason = "the driver does not list this profile (GetVideoDecoderProfile)";
    return;
  }
  const DXGI_FORMAT native = def.ten_bit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
  auto ask_format = [&](DXGI_FORMAT f, VaFormat* out) {
    out->asked = true;
    BOOL ok = FALSE;
    out->hr = vd->CheckVideoDecoderFormat(&def.guid, f, &ok);
    out->supported = SUCCEEDED(out->hr) && ok;
  };
  ask_format(DXGI_FORMAT_NV12, &c->nv12);
  ask_format(DXGI_FORMAT_P010, &c->p010);
  auto ask_config = [&](UINT w, UINT h, VaConfig* out) {
    out->asked = true;
    D3D11_VIDEO_DECODER_DESC desc{};
    desc.Guid = def.guid;
    desc.SampleWidth = w;
    desc.SampleHeight = h;
    desc.OutputFormat = native;
    out->hr = vd->GetVideoDecoderConfigCount(&desc, &out->count);
    if (FAILED(out->hr)) out->count = 0;
  };
  ask_config(kVaWidth1080, kVaHeight1080, &c->cfg1080);
  ask_config(kVaWidth2160, kVaHeight2160, &c->cfg2160);

  // A call that failed is not the same answer as a clean "no": the stage and the reason say which.
  const VaFormat& native_fmt = def.ten_bit ? c->p010 : c->nv12;
  const char* const native_name = def.ten_bit ? "P010" : "NV12";
  if (FAILED(native_fmt.hr)) {
    c->outcome = Outcome::kFailed;
    c->stage = "check_video_decoder_format";
    c->hr = native_fmt.hr;
    c->reason = std::string("CheckVideoDecoderFormat failed for the native output format ") + native_name +
                " although the profile is listed";
    return;
  }
  if (!native_fmt.supported) {
    c->outcome = Outcome::kFailed;
    c->stage = def.ten_bit ? "output_format_p010_not_supported" : "output_format_nv12_not_supported";
    c->hr = E_FAIL;
    c->reason = std::string("the driver does not support the native output format ") + native_name +
                " for this profile (CheckVideoDecoderFormat answered FALSE)";
    return;
  }
  if (FAILED(c->cfg1080.hr)) {
    c->outcome = Outcome::kFailed;
    c->stage = "get_decoder_config_count";
    c->hr = c->cfg1080.hr;
    c->reason = "GetVideoDecoderConfigCount failed at 1920x1080";
    return;
  }
  if (c->cfg1080.count == 0) {
    c->outcome = Outcome::kFailed;
    c->stage = "no_decoder_config_1080p";
    c->hr = E_FAIL;
    c->reason = "no decoder configuration for 1920x1080 (GetVideoDecoderConfigCount returned 0)";
    return;
  }

  // Confirm with a decoder object, released at once.
  c->create_asked = true;
  const UINT heights[] = {kVaHeight1080, kVaHeight1088};
  for (UINT h : heights) {
    D3D11_VIDEO_DECODER_DESC desc{};
    desc.Guid = def.guid;
    desc.SampleWidth = kVaWidth1080;
    desc.SampleHeight = h;
    desc.OutputFormat = native;
    VaTry t;
    t.size = std::to_string(kVaWidth1080) + "x" + std::to_string(h);
    D3D11_VIDEO_DECODER_CONFIG cfg{};
    t.stage = "get_config";
    t.hr = vd->GetVideoDecoderConfig(&desc, 0, &cfg);
    if (SUCCEEDED(t.hr)) {
      t.stage = "create_decoder";
      ComPtr<ID3D11VideoDecoder> dec;
      t.hr = vd->CreateVideoDecoder(&desc, &cfg, &dec);
      if (SUCCEEDED(t.hr) && dec) c->created = true;
    }
    c->tries.push_back(t);
    if (c->created) break;
  }
  if (!c->created) {
    c->outcome = Outcome::kFailed;
    c->stage = c->tries.back().stage;
    c->hr = c->tries.back().hr;
    c->reason = std::string(c->stage == "get_config" ? "GetVideoDecoderConfig" : "CreateVideoDecoder") +
                " failed, the decoder object could not be made (tried " + std::to_string(c->tries.size()) +
                " size(s))";
    return;
  }
  c->outcome = Outcome::kOk;
}

// One adapter (matched again by identity in a fresh DXGI enumeration), or the WARP rasterizer
// when `warp_stand_in`. Software adapters never count towards a verdict (see VaVerdict); they are
// only asked with --ci, to run the code path.
Attempt<VaAdapterResult> DoVaAdapter(const AdapterInfo& want, bool warp_stand_in) {
  Attempt<VaAdapterResult> a;
  VaAdapterResult& v = a.result;
  TestResult& r = v.tr;
  r.role = "d3d11va_capability";
  r.kind = "capability";
  v.adapter = want;
  v.warp_stand_in = warp_stand_in;
  r.adapter_known = true;
  r.adapter = want.description;
  r.adapter_vendor = want.vendor_id;
  r.adapter_software = want.software;

  ComPtr<IDXGIAdapter1> adapter;
  if (!warp_stand_in) {
    for (AdapterEntry& e : EnumAdapters()) {
      if (e.info.vendor_id == want.vendor_id && e.info.device_id == want.device_id &&
          e.info.description == want.description) {
        adapter = e.adapter;
        break;
      }
    }
    if (!adapter) {
      r.Skip("adapter_gone",
             "the adapter is not listed any more by a fresh DXGI enumeration (a hybrid GPU may have "
             "changed its power state)");
      return a;
    }
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got{};
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  const D3D_DRIVER_TYPE dtype = warp_stand_in ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN;
  HRESULT hr = D3D11CreateDevice(adapter.Get(), dtype, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                                 ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &got, &ctx);
  if (FAILED(hr) && want.software) {
    // A software adapter does not offer video support everywhere (the same fallback as the
    // decode tests); a hardware adapter is never retried without it.
    dev.Reset();
    ctx.Reset();
    hr = D3D11CreateDevice(adapter.Get(), dtype, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (SUCCEEDED(hr)) v.video_flag_dropped = true;
  }
  if (!r.Check("create_d3d_device", hr)) return a;
  v.feature_level = FeatureLevelStr(got);

  ComPtr<ID3D11VideoDevice> vd;
  hr = dev.As(&vd);
  r.Step("query_video_device", hr);
  if (FAILED(hr)) {
    if (want.software) {
      r.Skip("no_video_device", "the D3D11 device of the software adapter has no ID3D11VideoDevice");
      r.hr = hr;
    } else {
      r.Fail("query_video_device", hr);
    }
    return a;
  }

  v.profile_count = vd->GetVideoDecoderProfileCount();
  HRESULT list_hr = S_OK;
  for (UINT i = 0; i < v.profile_count; ++i) {
    GUID g{};
    HRESULT h = vd->GetVideoDecoderProfile(i, &g);
    if (SUCCEEDED(h)) {
      v.profiles.push_back(g);
    } else {
      r.notes.push_back("GetVideoDecoderProfile(" + std::to_string(i) + ") " + HrStr(h));
      if (SUCCEEDED(list_hr)) list_hr = h;
    }
  }
  // A profile list with a hole would report the missing profile as "not listed": the adapter's
  // answer is then a failure, not a capability report.
  if (!r.Check("list_profiles", list_hr)) return a;
  for (const VaProfileDef& def : kVaProfiles) {
    if (!def.probed) continue;
    VaProfileCheck c;
    CheckProfile(vd.Get(), def, v.profiles, &c);
    v.checks.push_back(std::move(c));
  }
  r.outcome = Outcome::kOk;
  if (ctx) {
    ctx->ClearState();
    ctx->Flush();
  }
  return a;
}

VaAdapterResult TimeoutVa(const AdapterInfo& want, bool warp_stand_in) {
  VaAdapterResult v;
  v.adapter = want;
  v.warp_stand_in = warp_stand_in;
  v.tr.role = "d3d11va_capability";
  v.tr.kind = "capability";
  v.tr.adapter_known = true;
  v.tr.adapter = want.description;
  v.tr.adapter_vendor = want.vendor_id;
  v.tr.adapter_software = want.software;
  v.tr.outcome = Outcome::kFailed;
  v.tr.stage = "timeout";
  v.tr.hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  v.tr.error = "no result within the watchdog time";
  return v;
}

TestResult& Result(VaAdapterResult& v) { return v.tr; }

// ---------------------------------------------------------------- watchdog

TestResult& Result(TestResult& r) { return r; }
TestResult& Result(EncodeOut& e) { return e.r; }

void MarkCrash(TestResult& r, unsigned long code) {
  r.outcome = Outcome::kFailed;
  r.stage = "crash";
  r.hr = E_FAIL;
  r.error = "structured exception " + Hex32(code);
}

// What the teardown of one attempt found, filled on the attempt's thread and read by
// the caller once `done` is set.
struct TeardownShared {
  std::atomic<bool> done{false};
  TestResult info;
};

// Runs `f` (which returns an Attempt<R>) on its own MTA thread with a watchdog. A hung
// or crashing driver must not take the probe down: the result then says "timeout" or
// "crash". The result is published as soon as `f` returns; the teardown (Attempt::cleanup)
// runs after that, and the caller waits for it for at most kTeardownWaitMs, so a call that
// hangs in the teardown costs the step trace, not the result of an attempt that worked.
template <typename R, typename F>
R WithWatchdog(F f, R timeout_value) {
  auto prom = std::make_shared<std::promise<R>>();
  auto td = std::make_shared<TeardownShared>();
  std::future<R> fut = prom->get_future();
  std::thread([prom, td, f, timeout_value]() mutable {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Attempt<R> a;
    SehInfo info;
    std::function<void()> body = [&] { a = f(); };
    if (!SafeInvoke(body, &info)) {
      // Start from the timeout value so that the entry still names the MFT,
      // the resolution and the D3D mode that crashed.
      a.result = timeout_value;
      MarkCrash(Result(a.result), info.code);
      a.cleanup = nullptr;
    }
    prom->set_value(std::move(a.result));
    if (a.cleanup) {
      SehInfo info2;
      std::function<void()> post = [&] { a.cleanup(&td->info); };
      if (!SafeInvoke(post, &info2)) {
        td->info.notes.push_back("structured exception " + Hex32(info2.code) + " during the teardown");
      }
    }
    td->done = true;
  }).detach();
  if (fut.wait_for(std::chrono::milliseconds(kTestTimeoutMs)) != std::future_status::ready) {
    ++g_hung_attempts;
    return timeout_value;
  }
  R res = fut.get();
  for (DWORD waited = 0; waited < kTeardownWaitMs && !td->done.load(); waited += 10) Sleep(10);
  TestResult& tr = Result(res);
  if (td->done.load()) {
    for (const StepRec& st : td->info.steps) tr.steps.push_back(st);
    for (const std::string& n : td->info.notes) tr.notes.push_back(n);
    if (td->info.residual_refs >= 0) tr.residual_refs = td->info.residual_refs;
  } else {
    ++g_hung_attempts;
    tr.notes.push_back("the teardown of this attempt did not finish within " +
                       std::to_string(kTeardownWaitMs / 1000) +
                       " s (a call blocks after the attempt); its step trace is missing");
  }
  return res;
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

std::string LowerAscii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A package registered for the current user whose name contains the word that was asked for.
struct PackageHit {
  std::string name;
  std::string version;
  std::string architecture;
  std::string family;
  bool in_known_family_list = false;
};

// A package full name, Name_Version_Architecture_ResourceId_PublisherId, split into its parts. The
// family name is Name_PublisherId. Pure: CI runs it on fixture names (--selftest-d3d11va-logic).
PackageHit ParsePackageFullName(const std::string& full, const std::vector<std::string>& known_families) {
  std::vector<std::string> parts;
  for (size_t pos = 0;;) {
    const size_t u = full.find('_', pos);
    parts.push_back(full.substr(pos, u == std::string::npos ? std::string::npos : u - pos));
    if (u == std::string::npos) break;
    pos = u + 1;
  }
  PackageHit h;
  h.name = parts[0];
  if (parts.size() > 1) h.version = parts[1];
  if (parts.size() > 2) h.architecture = parts[2];
  h.family = (parts.size() > 4 && !parts[4].empty()) ? h.name + "_" + parts[4] : h.name;
  for (const std::string& k : known_families) {
    if (LowerAscii(k) == LowerAscii(h.family)) h.in_known_family_list = true;
  }
  return h;
}

// Package full names registered for the current user that contain `needle_lower`, from the per-user
// package repository in the registry. This is NOT a documented API: it is read-only and best
// effort, observed on a Windows 10 machine (one sub key per registered package, named by the
// package full name), and the report says whether it could be read. It exists because the
// documented call used for the known families (GetPackagesByPackageFamily) needs the family name
// in advance, so a variant of the extension with a name nobody listed yet can only be found by
// name. Returns false when the repository could not be read completely (`rc` says why).
bool ScanPackageNames(const std::string& needle_lower, const std::vector<std::string>& known_families,
                      std::vector<PackageHit>* out, LONG* rc) {
  *rc = ERROR_SUCCESS;
  out->clear();
  HKEY key = nullptr;
  LONG r = RegOpenKeyExW(HKEY_CURRENT_USER,
                         L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\"
                         L"AppModel\\Repository\\Packages",
                         0, KEY_ENUMERATE_SUB_KEYS, &key);
  if (r != ERROR_SUCCESS) {
    *rc = r;
    return false;
  }
  bool complete = true;
  for (DWORD i = 0;; ++i) {
    wchar_t name[512];
    DWORD n = ARRAYSIZE(name);
    const LONG e = RegEnumKeyExW(key, i, name, &n, nullptr, nullptr, nullptr, nullptr);
    if (e == ERROR_NO_MORE_ITEMS) break;
    if (e == ERROR_MORE_DATA) {
      // Longer than a package full name can be (127 characters), but it was not read: say so.
      *rc = e;
      complete = false;
      continue;
    }
    if (e != ERROR_SUCCESS) {
      *rc = e;
      complete = false;
      break;
    }
    const std::string full = Utf8(name, static_cast<int>(n));
    if (LowerAscii(full).find(needle_lower) == std::string::npos) continue;
    out->push_back(ParsePackageFullName(full, known_families));
  }
  RegCloseKey(key);
  return complete;
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
  j.KvS("mft_class", m.mft_class);
  j.Key("returned_by");
  j.BeginArray();
  for (const std::string& v : m.seen_in) j.Str(v);
  j.EndArray();
  j.EndObject();
}

void WriteTest(Json& j, const TestResult& t) {
  j.BeginObject();
  j.KvS("role", t.role);
  j.KvS("kind", t.kind);
  j.KvS("mft", t.mft);
  if (!t.clsid.empty()) j.KvS("clsid", t.clsid);
  j.KvS("mft_class", t.mft_class);
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
    if (t.activation_refused) j.KvB("activation_refused", true);
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
  o.r.clsid = mi.clsid;
  o.r.mft_class = mi.mft_class;
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
  r.clsid = mi.clsid;
  r.mft_class = mi.mft_class;
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
  r.clsid = mi.clsid;
  r.mft_class = mi.mft_class;
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

// Why there is no decoder MFT, in words that match what was measured: the enumerations that were
// run are named, and what is known about the packages is said as it is. A registered package does
// not mean that its decoder is enumerated, and a missing registration is only claimed when no
// package with HEVC in its name was found either. Pure: CI runs it on fixture inputs.
std::string NoDecoderReason(const std::string& codec_upper, bool is_hevc, size_t ext_packages, int unlisted_count,
                            const std::string& unlisted_names, bool name_scan_complete) {
  std::string r = "MFTEnumEx returns no " + codec_upper +
                  " decoder MFT to this process (default enumeration, with "
                  "MFT_ENUM_FLAG_UNTRUSTED_STOREMFT, and unfiltered)";
  if (!is_hevc) return r;
  if (ext_packages > 0) {
    r += "; the HEVC Video Extensions package is registered for the current user (" + std::to_string(ext_packages) +
         " package(s)), so registration is not what is missing";
  } else if (unlisted_count > 0) {
    r += "; no package of the known families is registered for the current user, but a package with HEVC in its "
         "name is: " +
         unlisted_names + " (not in the family list of this probe)";
  } else if (!name_scan_complete) {
    r += "; no package of the known families is registered for the current user (the name scan could not be read "
         "completely)";
  } else {
    r += "; no package of the known families and no package with HEVC in its name is registered for the current user";
  }
  return r;
}

// ---------------------------------------------------------------- enumeration and D3D11VA reports

// What every enumeration variant returned, for one category (decoders or encoders).
void WriteEnumeration(Json& j, const MftEnumeration& e) {
  j.BeginObject();
  j.Key("variants");
  j.BeginArray();
  for (size_t vi = 0; vi < e.variants.size(); ++vi) {
    const VariantResult& vr = e.variants[vi];
    const EnumVariant& ev = kVariants[vi];
    j.BeginObject();
    j.KvS("label", vr.label);
    j.KvB("store_flag", ev.store);
    j.KvB("sort_and_filter", ev.sort);
    j.KvB("all_flags", ev.all);
    j.Key("queries");
    j.BeginObject();
    for (const EnumQuery& q : QueriesOf(ev)) {
      j.Key(q.label);
      j.BeginObject();
      j.KvS("flags", Hex32(q.flags));
      auto c = vr.counts.find(q.label);
      j.KvI("count", c == vr.counts.end() ? -1 : c->second);
      auto er = vr.errors.find(q.label);
      if (er != vr.errors.end()) j.KvS("hresult", er->second);
      j.EndObject();
    }
    j.EndObject();
    j.KvI("unique_mfts", static_cast<long long>(vr.mfts.size()));
    j.Key("mfts");
    j.BeginArray();
    for (const MftInfo& m : vr.mfts) WriteMft(j, m);
    j.EndArray();
    j.EndObject();
  }
  j.EndArray();
  // MFTs that only a variant other than the default one returned, and default MFTs that the
  // variant with the store flag did not return (in case the flag restricts instead of adds).
  j.Key("only_outside_the_default_enumeration");
  j.BeginArray();
  for (const MftInfo& m : e.extras) WriteMft(j, m);
  j.EndArray();
  j.Key("default_mfts_missing_with_store_flag");
  j.BeginArray();
  if (e.variants.size() > 1) {
    for (const MftInfo& m : e.variants[0].mfts) {
      bool found = false;
      for (const MftInfo& x : e.variants[1].mfts) found = found || x.clsid == m.clsid;
      if (!found) WriteMft(j, m);
    }
  }
  j.EndArray();
  j.EndObject();
}

const VaProfileCheck* FindCheck(const VaAdapterResult& v, const char* profile) {
  for (const VaProfileCheck& c : v.checks) {
    if (std::string(c.def->name) == profile) return &c;
  }
  return nullptr;
}

void WriteVaFormat(Json& j, const char* key, const VaFormat& f) {
  j.Key(key);
  j.BeginObject();
  if (f.asked) {
    j.KvB("supported", f.supported);
    j.KvS("hresult", HrStr(f.hr));
  } else {
    j.Key("supported");
    j.Null();
    j.KvS("reason", "not asked: the profile is not listed");
  }
  j.EndObject();
}

void WriteVaConfig(Json& j, const char* key, const VaConfig& c, const char* format) {
  j.Key(key);
  j.BeginObject();
  j.KvS("output_format", format);
  if (c.asked) {
    j.KvI("count", c.count);
    j.KvS("hresult", HrStr(c.hr));
  } else {
    j.Key("count");
    j.Null();
    j.KvS("reason", "not asked: the profile is not listed");
  }
  j.EndObject();
}

void WriteVaCheck(Json& j, const VaProfileCheck& c) {
  const char* native = c.def->ten_bit ? "P010" : "NV12";
  j.BeginObject();
  j.KvS("profile", c.def->name);
  j.KvS("guid", GuidStr(c.def->guid));
  j.KvB("listed", c.listed);
  j.KvS("status", OutcomeStr(c.outcome));
  if (c.outcome == Outcome::kFailed) {
    j.KvS("failed_stage", c.stage);
    j.KvS("hresult", HrStr(c.hr));
    j.KvS("reason", c.reason);
  }
  j.KvS("native_output_format", native);
  j.Key("output_formats");
  j.BeginObject();
  WriteVaFormat(j, "NV12", c.nv12);
  WriteVaFormat(j, "P010", c.p010);
  j.EndObject();
  j.Key("decoder_config_count");
  j.BeginObject();
  WriteVaConfig(j, "1920x1080", c.cfg1080, native);
  WriteVaConfig(j, "3840x2160", c.cfg2160, native);
  j.EndObject();
  j.Key("decoder_object");
  j.BeginObject();
  if (c.create_asked) {
    j.KvS("status", c.created ? "ok" : "failed");
    j.Key("tries");
    j.BeginArray();
    for (const VaTry& t : c.tries) {
      j.BeginObject();
      j.KvS("size", t.size);
      j.KvS("stage", t.stage);
      j.KvS("hresult", HrStr(t.hr));
      j.EndObject();
    }
    j.EndArray();
  } else {
    j.KvS("status", "not_attempted");
    j.KvS("reason", c.listed ? "the output format or the 1080p configuration check did not pass"
                             : "the profile is not listed");
  }
  j.EndObject();
  j.EndObject();
}

void WriteVaAdapter(Json& j, const VaAdapterResult& v) {
  j.BeginObject();
  j.Key("adapter");
  j.BeginObject();
  j.KvS("description", v.adapter.description);
  j.KvS("vendor_id", Hex32(v.adapter.vendor_id));
  j.KvS("device_id", Hex32(v.adapter.device_id));
  j.KvB("software_adapter", v.adapter.software);
  j.KvS("driver_version", v.adapter.driver_version);
  j.KvB("warp_stand_in", v.warp_stand_in);
  j.EndObject();
  j.KvS("status", OutcomeStr(v.tr.outcome));
  if (v.tr.outcome == Outcome::kNotAttempted) {
    j.KvS("reason_code", v.tr.reason_code);
    j.KvS("reason", v.tr.reason);
  } else if (v.tr.outcome == Outcome::kFailed) {
    j.KvS("failed_stage", v.tr.stage);
    j.KvS("hresult", HrStr(v.tr.hr));
    if (!v.tr.error.empty()) j.KvS("error", v.tr.error);
  }
  if (!v.feature_level.empty()) j.KvS("feature_level", v.feature_level);
  j.KvB("video_support_flag_dropped", v.video_flag_dropped);
  if (v.tr.outcome == Outcome::kOk) {
    j.KvI("profile_count", v.profile_count);
    j.Key("profiles");
    j.BeginArray();
    for (const GUID& g : v.profiles) {
      const VaProfileDef* d = FindVaProfile(g);
      j.BeginObject();
      j.KvS("guid", GuidStr(g));
      j.Key("name");
      if (d) {
        j.Str(d->name);
      } else {
        j.Null();
      }
      j.EndObject();
    }
    j.EndArray();
    j.Key("checked");
    j.BeginArray();
    for (const VaProfileCheck& c : v.checks) WriteVaCheck(j, c);
    j.EndArray();
  }
  if (!v.tr.steps.empty()) {
    j.Key("steps");
    j.BeginArray();
    for (const StepRec& s : v.tr.steps) {
      j.BeginObject();
      j.KvS("stage", s.stage);
      j.KvS("hresult", HrStr(s.hr));
      j.EndObject();
    }
    j.EndArray();
  }
  if (!v.tr.notes.empty()) {
    j.Key("notes");
    j.BeginArray();
    for (const std::string& n : v.tr.notes) j.Str(n);
    j.EndArray();
  }
  j.EndObject();
}

// Folds the D3D11VA checks of the adapters into one value for `profile`: ok when a non-software
// adapter passed every check for it (listed, native output format, a 1080p decoder configuration,
// a decoder object made and released); failed when a non-software adapter was asked and none
// passed; not_attempted when none was asked. A software adapter never counts.
Verdict VaVerdict(const std::vector<VaAdapterResult>& va, const char* profile) {
  int ok = 0;
  std::vector<std::string> failed, na;
  for (const VaAdapterResult& v : va) {
    if (v.adapter.software) continue;
    const std::string who = v.adapter.description;
    if (v.tr.outcome == Outcome::kFailed) {
      failed.push_back(who + ": " + v.tr.stage + " " + HrStr(v.tr.hr));
    } else if (v.tr.outcome == Outcome::kNotAttempted) {
      na.push_back(who + ": " + v.tr.reason);
    } else if (const VaProfileCheck* c = FindCheck(v, profile)) {
      if (c->outcome == Outcome::kOk) {
        ++ok;
      } else if (c->outcome == Outcome::kNotAttempted) {
        na.push_back(who + ": the profile was not checked");
      } else {
        failed.push_back(who + ": " + c->reason + " [" + c->stage + "]");
      }
    } else {
      na.push_back(who + ": the profile was not checked");
    }
  }
  auto join = [](const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : "; ") + x;
    return s;
  };
  if (ok) return {"ok", std::string()};
  if (!failed.empty()) return {"failed", join(failed)};
  if (!na.empty()) return {"not_attempted", join(na)};
  return {"not_attempted",
          "no non-software DXGI adapter was asked (a software adapter is only asked with --ci and "
          "never counts)"};
}

// One value per adapter for `profile`, in the order of the adapter list: which adapter said what.
void WriteVaByAdapter(Json& j, const std::vector<VaAdapterResult>& va, const char* profile) {
  j.BeginArray();
  for (const VaAdapterResult& v : va) {
    j.BeginObject();
    j.KvS("adapter", v.adapter.description);
    j.KvS("vendor_id", Hex32(v.adapter.vendor_id));
    j.KvB("software_adapter", v.adapter.software);
    std::string status = OutcomeStr(v.tr.outcome);
    std::string why;
    if (v.tr.outcome == Outcome::kFailed) {
      why = v.tr.stage + " " + HrStr(v.tr.hr);
    } else if (v.tr.outcome == Outcome::kNotAttempted) {
      why = v.tr.reason;
    } else if (const VaProfileCheck* c = FindCheck(v, profile)) {
      status = OutcomeStr(c->outcome);
      if (c->outcome != Outcome::kOk) why = c->reason + " [" + c->stage + "]";
    } else {
      status = "not_attempted";
      why = "the profile was not checked";
    }
    j.KvS("status", status);
    if (!why.empty()) j.KvS("reason", why);
    j.EndObject();
  }
  j.EndArray();
}

// ActivateObject of the MFTs of one class (see MftEnumeration): ok when one was created, failed
// when every one that was reached was refused, not_attempted when none was returned.
Verdict ActivationVerdict(const std::vector<TestResult>& tests, const std::string& mft_class,
                          const std::string& none_reason) {
  int ok = 0;
  std::vector<std::string> refused, unreached;
  for (const TestResult& t : tests) {
    if (t.role != "decode" || t.mft_class != mft_class) continue;
    bool reached = false;
    HRESULT ah = S_OK;
    for (const StepRec& st : t.steps) {
      if (st.stage == "activate") {
        reached = true;
        ah = st.hr;
      }
    }
    if (!reached) {
      // Returned and selected for a test, but an earlier step (no D3D11 adapter, a device that could
      // not be made, a fresh enumeration that did not find it again) stopped the attempt: that is not
      // "none was returned", and it is not a refusal either.
      std::string why = t.mft + ": ActivateObject was not reached (";
      if (t.outcome == Outcome::kNotAttempted) {
        why += t.reason;
      } else if (t.steps.empty()) {
        why += "no step was recorded";
      } else {
        why += "the last step was " + t.steps.back().stage + " " + HrStr(t.steps.back().hr);
      }
      unreached.push_back(why + ")");
      continue;
    }
    if (SUCCEEDED(ah)) {
      ++ok;
    } else {
      refused.push_back(t.mft + ": ActivateObject was refused, " + HrStr(ah));
    }
  }
  auto join = [](const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : "; ") + x;
    return s;
  };
  if (ok) return {"ok", std::string()};
  if (!refused.empty()) return {"failed", join(refused)};
  if (!unreached.empty()) return {"not_attempted", join(unreached)};
  return {"not_attempted", none_reason};
}

// ---------------------------------------------------------------- self-test of the D3D11VA logic

// The hosted runner has no GPU, so CheckProfile and VaVerdict would never meet a D3D11 video
// device there, and what they decide (what "ok" means, which stage a failure is blamed on, that a
// software adapter never counts) would be untested until the lab runs it. --selftest-d3d11va-logic
// runs both against a scripted mock of ID3D11VideoDevice and prints what they decided, for CI to
// compare with the expected answers. It touches no driver and no D3D11 device.
std::atomic<int> g_mock_decoders_alive{0};

class MockDecoder : public ID3D11VideoDecoder {
 public:
  MockDecoder() { ++g_mock_decoders_alive; }
  ~MockDecoder() { --g_mock_decoders_alive; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11VideoDecoder)) {
      *out = static_cast<ID3D11VideoDecoder*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  void STDMETHODCALLTYPE GetDevice(ID3D11Device** d) override {
    if (d) *d = nullptr;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCreationParameters(D3D11_VIDEO_DECODER_DESC*,
                                                  D3D11_VIDEO_DECODER_CONFIG*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetDriverHandle(HANDLE*) override { return E_NOTIMPL; }

 private:
  std::atomic<ULONG> refs_{1};
};

struct MockProfile {
  GUID guid;
  bool nv12;
  bool p010;
  UINT cfg1080;
  UINT cfg2160;
  HRESULT config_hr;    // GetVideoDecoderConfig
  HRESULT create_1080;  // CreateVideoDecoder at 1920x1080
  HRESULT create_1088;  // CreateVideoDecoder at 1920x1088
  HRESULT format_hr = S_OK;  // CheckVideoDecoderFormat (the call itself)
  HRESULT count_hr = S_OK;   // GetVideoDecoderConfigCount (the call itself)
};

class MockVideoDevice : public ID3D11VideoDevice {
 public:
  std::vector<MockProfile> profiles;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11VideoDevice)) {
      *out = static_cast<ID3D11VideoDevice*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  HRESULT STDMETHODCALLTYPE CreateVideoDecoder(const D3D11_VIDEO_DECODER_DESC* d,
                                               const D3D11_VIDEO_DECODER_CONFIG*,
                                               ID3D11VideoDecoder** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    const MockProfile* p = d ? Find(d->Guid) : nullptr;
    if (!p) return E_INVALIDARG;
    const HRESULT hr = d->SampleHeight == 1088 ? p->create_1088 : p->create_1080;
    if (SUCCEEDED(hr)) *out = new MockDecoder();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateVideoProcessor(ID3D11VideoProcessorEnumerator*, UINT,
                                                 ID3D11VideoProcessor**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreateAuthenticatedChannel(D3D11_AUTHENTICATED_CHANNEL_TYPE,
                                                       ID3D11AuthenticatedChannel**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreateCryptoSession(const GUID*, const GUID*, const GUID*,
                                                ID3D11CryptoSession**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreateVideoDecoderOutputView(ID3D11Resource*,
                                                         const D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC*,
                                                         ID3D11VideoDecoderOutputView**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreateVideoProcessorInputView(ID3D11Resource*, ID3D11VideoProcessorEnumerator*,
                                                          const D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC*,
                                                          ID3D11VideoProcessorInputView**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreateVideoProcessorOutputView(ID3D11Resource*, ID3D11VideoProcessorEnumerator*,
                                                           const D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC*,
                                                           ID3D11VideoProcessorOutputView**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreateVideoProcessorEnumerator(const D3D11_VIDEO_PROCESSOR_CONTENT_DESC*,
                                                           ID3D11VideoProcessorEnumerator**) override {
    return E_NOTIMPL;
  }
  UINT STDMETHODCALLTYPE GetVideoDecoderProfileCount(void) override {
    return static_cast<UINT>(profiles.size());
  }
  HRESULT STDMETHODCALLTYPE GetVideoDecoderProfile(UINT index, GUID* g) override {
    if (!g || index >= profiles.size()) return E_INVALIDARG;
    *g = profiles[index].guid;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE CheckVideoDecoderFormat(const GUID* g, DXGI_FORMAT f, BOOL* ok) override {
    if (!g || !ok) return E_POINTER;
    const MockProfile* p = Find(*g);
    if (!p) return E_INVALIDARG;  // per docs: a profile the driver does not support
    if (FAILED(p->format_hr)) return p->format_hr;
    *ok = (f == DXGI_FORMAT_NV12 && p->nv12) || (f == DXGI_FORMAT_P010 && p->p010);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetVideoDecoderConfigCount(const D3D11_VIDEO_DECODER_DESC* d, UINT* n) override {
    if (!d || !n) return E_POINTER;
    const MockProfile* p = Find(d->Guid);
    if (!p) return E_INVALIDARG;
    if (FAILED(p->count_hr)) return p->count_hr;
    *n = d->SampleWidth >= 3840 ? p->cfg2160 : p->cfg1080;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetVideoDecoderConfig(const D3D11_VIDEO_DECODER_DESC* d, UINT,
                                                  D3D11_VIDEO_DECODER_CONFIG* c) override {
    if (!d || !c) return E_POINTER;
    const MockProfile* p = Find(d->Guid);
    if (!p) return E_INVALIDARG;
    return p->config_hr;
  }
  HRESULT STDMETHODCALLTYPE GetContentProtectionCaps(const GUID*, const GUID*,
                                                     D3D11_VIDEO_CONTENT_PROTECTION_CAPS*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CheckCryptoKeyExchange(const GUID*, const GUID*, UINT, GUID*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }

 private:
  const MockProfile* Find(const GUID& g) const {
    for (const MockProfile& p : profiles) {
      if (p.guid == g) return &p;
    }
    return nullptr;
  }
};

GUID VaGuid(const char* name) {
  for (const VaProfileDef& d : kVaProfiles) {
    if (std::string(d.name) == name) return d.guid;
  }
  return GUID{};
}

// Runs CheckProfile and VaVerdict on the scripted mock and returns the JSON of what they decided.
std::string VaLogicSelfTest() {
  MockVideoDevice dev;
  dev.profiles = {
      // all good, decoder object at 1920x1080
      {VaGuid("H264_VLD_NOFGT"), true, false, 1, 1, S_OK, S_OK, S_OK},
      // the driver refuses a 1920x1080 decoder object and takes 1920x1088: still ok, two tries
      {VaGuid("HEVC_VLD_MAIN"), true, false, 3, 2, S_OK, E_INVALIDARG, S_OK},
      // listed, but not with P010, the native format of a 10 bit profile
      {VaGuid("HEVC_VLD_MAIN10"), true, false, 2, 1, S_OK, S_OK, S_OK},
      // listed and NV12, but no decoder configuration at 1920x1080
      {VaGuid("VP9_VLD_PROFILE0"), true, false, 0, 0, S_OK, S_OK, S_OK},
      // every check passes but the decoder object cannot be made
      {VaGuid("AV1_VLD_PROFILE0"), true, false, 1, 1, S_OK, E_OUTOFMEMORY, E_OUTOFMEMORY},
      // (VP9_VLD_10BIT_PROFILE2 is not listed at all)
  };
  std::vector<GUID> listed;
  const UINT n = dev.GetVideoDecoderProfileCount();
  for (UINT i = 0; i < n; ++i) {
    GUID g{};
    if (SUCCEEDED(dev.GetVideoDecoderProfile(i, &g))) listed.push_back(g);
  }
  std::vector<VaProfileCheck> checks;
  for (const VaProfileDef& def : kVaProfiles) {
    if (!def.probed) continue;
    VaProfileCheck c;
    CheckProfile(&dev, def, listed, &c);
    checks.push_back(std::move(c));
  }

  // A second scripted device: the calls themselves fail (not a clean "no").
  MockVideoDevice dev_b;
  dev_b.profiles = {
      {VaGuid("H264_VLD_NOFGT"), true, false, 1, 1, S_OK, S_OK, S_OK},
      {VaGuid("HEVC_VLD_MAIN"), true, false, 1, 1, S_OK, S_OK, S_OK},
      // CheckVideoDecoderFormat itself fails although the profile is listed
      {VaGuid("HEVC_VLD_MAIN10"), true, true, 1, 1, S_OK, S_OK, S_OK, E_FAIL, S_OK},
      // GetVideoDecoderConfigCount itself fails
      {VaGuid("VP9_VLD_PROFILE0"), true, false, 1, 1, S_OK, S_OK, S_OK, S_OK, E_OUTOFMEMORY},
      // GetVideoDecoderConfig itself fails (the counts say there is a configuration)
      {VaGuid("AV1_VLD_PROFILE0"), true, false, 1, 1, E_INVALIDARG, S_OK, S_OK},
  };
  std::vector<GUID> listed_b;
  for (UINT i = 0; i < dev_b.GetVideoDecoderProfileCount(); ++i) {
    GUID g{};
    if (SUCCEEDED(dev_b.GetVideoDecoderProfile(i, &g))) listed_b.push_back(g);
  }
  std::vector<VaProfileCheck> checks_b;
  for (const VaProfileDef& def : kVaProfiles) {
    if (!def.probed) continue;
    VaProfileCheck c;
    CheckProfile(&dev_b, def, listed_b, &c);
    checks_b.push_back(std::move(c));
  }

  auto adapter = [&](const char* name, unsigned vendor, bool software, Outcome outcome) {
    VaAdapterResult v;
    v.adapter.description = name;
    v.adapter.vendor_id = vendor;
    v.adapter.software = software;
    v.tr.outcome = outcome;
    if (outcome == Outcome::kOk) {
      v.checks = checks;
    } else if (outcome == Outcome::kFailed) {
      v.tr.stage = "create_d3d_device";
      v.tr.hr = DXGI_ERROR_UNSUPPORTED;
    } else {
      v.tr.reason_code = "no_video_device";
      v.tr.reason = "the adapter has no video device";
    }
    return v;
  };
  const VaAdapterResult hw_ok = adapter("hardware A", 0x8086, false, Outcome::kOk);
  const VaAdapterResult hw_failed = adapter("hardware B", 0x10DE, false, Outcome::kFailed);
  const VaAdapterResult hw_none = adapter("hardware C", 0x1002, false, Outcome::kNotAttempted);
  const VaAdapterResult sw_ok = adapter("software D", 0x1414, true, Outcome::kOk);

  Json j;
  j.BeginObject();
  j.Key("tool");
  j.BeginObject();
  j.KvS("name", "hevc-probe");
  j.KvS("mode", "selftest_d3d11va_logic");
  j.EndObject();
  j.KvI("mock_profile_count", n);
  j.Key("checked");
  j.BeginArray();
  for (const VaProfileCheck& c : checks) WriteVaCheck(j, c);
  j.EndArray();
  j.KvI("mock_decoder_objects_left", g_mock_decoders_alive.load());
  j.Key("checked_calls_fail");
  j.BeginArray();
  for (const VaProfileCheck& c : checks_b) WriteVaCheck(j, c);
  j.EndArray();

  // The classification of the MFTs that only a non-default enumeration returns, on fabricated
  // variants: default {A, E}, store flag {A, B}, unfiltered {A, C, E}, unfiltered with the store flag
  // {A, B, C, D}.
  auto mft = [](const char* name, const char* clsid) {
    MftInfo m;
    m.name = name;
    m.clsid = clsid;
    return m;
  };
  const MftInfo mA = mft("A", "A0000000-0000-0000-0000-000000000000");
  const MftInfo mB = mft("B", "B0000000-0000-0000-0000-000000000000");
  const MftInfo mC = mft("C", "C0000000-0000-0000-0000-000000000000");
  const MftInfo mD = mft("D", "D0000000-0000-0000-0000-000000000000");
  const MftInfo mE = mft("E", "E0000000-0000-0000-0000-000000000000");
  const std::vector<std::vector<MftInfo>> fabricated = {{mA, mE}, {mA, mB}, {mA, mC, mE}, {mA, mB, mC, mD}};
  std::vector<VariantResult> variants;
  for (size_t i = 0; i < fabricated.size(); ++i) {
    VariantResult vr;
    vr.label = kVariants[i].label;
    vr.mfts = fabricated[i];
    for (const EnumQuery& q : QueriesOf(kVariants[i])) vr.counts[q.label] = static_cast<int>(vr.mfts.size());
    variants.push_back(std::move(vr));
  }
  const MftEnumeration fab = BuildEnumeration(std::move(variants));
  j.Key("enumeration_case");
  WriteEnumeration(j, fab);

  // ActivateObject outcomes per class.
  auto decode_test = [](const char* cls, std::vector<HRESULT> activations) {
    TestResult t;
    t.role = "decode";
    t.mft_class = cls;
    t.mft = "X";
    for (HRESULT h : activations) t.Step("activate", h);
    return t;
  };
  TestResult not_reached = decode_test("store_flag_only", {});
  not_reached.Step("create_d3d_device", E_FAIL);
  const std::string none = "none was returned";
  struct ActCase {
    const char* name;
    std::vector<TestResult> tests;
  };
  const ActCase act_cases[] = {
      {"a_refused", {decode_test("store_flag_only", {E_ACCESSDENIED})}},
      {"b_refused_and_created", {decode_test("store_flag_only", {E_ACCESSDENIED}), decode_test("store_flag_only", {S_OK})}},
      {"c_only_default_ones", {decode_test("default", {E_ACCESSDENIED}), decode_test("default", {S_OK})}},
      {"d_never_reached_activate", {not_reached}},
      {"e_other_class_refused", {decode_test("needs_other_flags", {E_ACCESSDENIED})}},
      {"f_two_refusals", {decode_test("store_flag_only", {E_ACCESSDENIED}), decode_test("store_flag_only", {E_NOINTERFACE})}},
  };
  j.Key("activation_cases");
  j.BeginArray();
  for (const ActCase& c : act_cases) {
    const Verdict v = ActivationVerdict(c.tests, "store_flag_only", none);
    j.BeginObject();
    j.KvS("case", c.name);
    j.KvS("status", v.status);
    j.KvS("reason", v.reason);
    j.EndObject();
  }
  j.EndArray();

  // Package full names: split into parts, the family derived, the known families recognised.
  const std::vector<std::string> families = {"Microsoft.HEVCVideoExtensionFirstParty_8wekyb3d8bbwe",
                                             "Microsoft.HEVCVideoExtension_8wekyb3d8bbwe"};
  const char* const package_names[] = {
      "Microsoft.HEVCVideoExtensionFirstParty_2.4.111.0_x64__8wekyb3d8bbwe",
      "microsoft.hevcvideoextension_2.0.60091.0_x64__8WEKYB3D8BBWE",
      "Vendor.HEVCPlayer_1.2.3.4_x64__abc123",
      "Odd.Package_1.0.0.0_neutral_split.scale-100_abc123",
      "NoUnderscores",
  };
  j.Key("package_name_cases");
  j.BeginArray();
  for (const char* full : package_names) {
    const PackageHit h = ParsePackageFullName(full, families);
    j.BeginObject();
    j.KvS("full_name", full);
    j.KvS("name", h.name);
    j.KvS("version", h.version);
    j.KvS("architecture", h.architecture);
    j.KvS("family", h.family);
    j.KvB("in_known_family_list", h.in_known_family_list);
    j.EndObject();
  }
  j.EndArray();

  // The reason text when there is no decoder.
  j.Key("no_decoder_reason_cases");
  j.BeginArray();
  struct ReasonCase {
    const char* name;
    bool hevc;
    size_t registered;
    int unlisted;
    bool complete;
  };
  const ReasonCase reason_cases[] = {
      {"a_registered", true, 1, 0, true},
      {"b_unlisted_variant", true, 0, 1, true},
      {"c_scan_incomplete", true, 0, 0, false},
      {"d_nothing_registered", true, 0, 0, true},
      {"e_h264", false, 0, 0, true},
  };
  for (const ReasonCase& c : reason_cases) {
    j.BeginObject();
    j.KvS("case", c.name);
    j.KvS("reason", NoDecoderReason(c.hevc ? "HEVC" : "H.264", c.hevc, c.registered, c.unlisted,
                                    c.unlisted ? "Vendor.HEVCPlayer_abc123" : "", c.complete));
    j.EndObject();
  }
  j.EndArray();

  struct Case {
    const char* name;
    std::vector<VaAdapterResult> adapters;
    const char* profile;
  };
  const Case cases[] = {
      {"a_hardware_ok_and_hardware_failed", {hw_ok, hw_failed}, "HEVC_VLD_MAIN"},
      {"b_hardware_failed_and_software_ok", {hw_failed, sw_ok}, "HEVC_VLD_MAIN"},
      {"c_software_ok_only", {sw_ok}, "HEVC_VLD_MAIN"},
      {"d_no_adapter", {}, "HEVC_VLD_MAIN"},
      {"e_hardware_without_video_device", {hw_none}, "HEVC_VLD_MAIN"},
      {"f_hardware_ok_but_the_profile_failed", {hw_ok}, "AV1_VLD_PROFILE0"},
      {"g_hardware_ok_h264", {hw_ok, sw_ok}, "H264_VLD_NOFGT"},
      {"h_hardware_ok_but_the_profile_not_listed", {hw_ok}, "VP9_VLD_10BIT_PROFILE2"},
  };
  j.Key("verdict_cases");
  j.BeginArray();
  for (const Case& c : cases) {
    const Verdict v = VaVerdict(c.adapters, c.profile);
    j.BeginObject();
    j.KvS("case", c.name);
    j.KvS("profile", c.profile);
    j.KvS("status", v.status);
    j.KvS("reason", v.reason);
    j.Key("by_adapter");
    WriteVaByAdapter(j, c.adapters, c.profile);
    j.EndObject();
  }
  j.EndArray();
  j.EndObject();
  return j.Text() + "\n";
}

}  // namespace

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
  bool pause = false;
  bool ci = false;
  bool selftest_va_logic = false;
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
    } else if (a == "--selftest-d3d11va-logic") {
      selftest_va_logic = true;
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
                   "           [--selftest-d3d11va-logic]\n"
                   "Writes a JSON report to stdout and to a file next to the exe.\n"
                   "--resolutions   run only the listed resolutions (default both); 1080 alone\n"
                   "                shows whether a 1080p failure depends on the 720p run before it\n"
                   "--allow-software-adapter  let the D3D11 attempts use a software adapter when\n"
                   "                the machine has no GPU (runs the code path, proves nothing\n"
                   "                about hardware)\n"
                   "--ci            --allow-software-adapter, and exit code 4 when an attempt\n"
                   "                crashed or timed out (the default exit code is 0 whenever the\n"
                   "                report was written, a machine without a GPU included)\n"
                   "--selftest-d3d11va-logic  run the D3D11VA decision logic against a scripted\n"
                   "                mock of ID3D11VideoDevice and print what it decided (CI; no\n"
                   "                driver is touched, no report file is written)\n");
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
  if (selftest_va_logic) {
    const std::string text = VaLogicSelfTest();
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    g_run_finished = true;
    ExitProcess(0);
  }
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

  // Every enumeration variant (see kVariants) for the encoders and the decoders of the codec.
  // `encoders` and `decoders` are what the default enumeration returns, as in every earlier
  // version of the probe; what only another variant returns is in the *_enum.extras lists.
  MftEnumeration enc_enum, dec_enum;
  std::vector<MftInfo> encoders, decoders;
  std::map<std::string, int> enc_counts, dec_counts;
  std::map<std::string, std::string> enc_errors, dec_errors;
  if (mf_ok) {
    enc_enum = EnumAllVariants(MFT_CATEGORY_VIDEO_ENCODER, g_subtype, true);
    dec_enum = EnumAllVariants(MFT_CATEGORY_VIDEO_DECODER, g_subtype, false);
    encoders = enc_enum.variants[0].mfts;
    decoders = dec_enum.variants[0].mfts;
    enc_counts = enc_enum.variants[0].counts;
    dec_counts = dec_enum.variants[0].counts;
    enc_errors = enc_enum.variants[0].errors;
    dec_errors = dec_enum.variants[0].errors;
  }
  auto hw_first = [](std::vector<MftInfo>& v) {
    std::stable_partition(v.begin(), v.end(), [](const MftInfo& m) { return m.hardware; });
  };
  hw_first(encoders);
  hw_first(decoders);

  // The package families of the HEVC Video Extensions, as registered for the current user (the
  // list is the same as in hwlab-common.ps1; CI compares the family names of the two). Whether
  // the package is provisioned in the system image needs elevation and is left to
  // hw-inventory.ps1. Any other registered package with HEVC in its name is listed separately,
  // so that a new variant is never missed again.
  const wchar_t* const kExtFamilies[] = {L"Microsoft.HEVCVideoExtension_8wekyb3d8bbwe",
                                         L"Microsoft.HEVCVideoExtensions_8wekyb3d8bbwe",
                                         L"Microsoft.HEVCVideoExtensionFirstParty_8wekyb3d8bbwe"};
  std::vector<std::string> ext_packages;
  std::vector<std::string> known_families;
  for (const wchar_t* fam : kExtFamilies) {
    known_families.push_back(Utf8(fam));
    for (std::string& n : FindPackages(fam)) ext_packages.push_back(std::move(n));
  }
  std::vector<PackageHit> hevc_named;
  LONG hevc_named_rc = ERROR_SUCCESS;
  const bool hevc_named_complete = ScanPackageNames("hevc", known_families, &hevc_named, &hevc_named_rc);
  int hevc_named_unlisted = 0;
  std::string hevc_named_unlisted_names;
  for (const PackageHit& h : hevc_named) {
    if (h.in_known_family_list) continue;
    ++hevc_named_unlisted;
    hevc_named_unlisted_names += (hevc_named_unlisted_names.empty() ? "" : ", ") + h.family;
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
  // Then up to three decoders that only a non-default enumeration returns (the ones that need
  // MFT_ENUM_FLAG_UNTRUSTED_STOREMFT, or are excluded from the default enumeration for another
  // reason), tested the same way and marked with their mft_class.
  std::set<std::string> failed_720_modes;
  for (int ri = 0; ri < 2; ++ri) {
    if (!res[ri].enabled) continue;
    auto decode_one = [&](const MftInfo& mi) {
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
    };
    int dec_tried = 0;
    for (const MftInfo& mi : decoders) {
      if (dec_tried++ >= 6) break;
      decode_one(mi);
    }
    int extra_tried = 0;
    for (const MftInfo& mi : dec_enum.extras) {
      if (extra_tried++ >= 3) break;
      decode_one(mi);
    }
  }

  // ---- D3D11VA capability of every adapter, after every MFT test so that the MFT results are not
  // disturbed by it. It does not need Media Foundation, so it runs whether MFStartup worked or
  // not. Software adapters are only asked with --ci (to run the code path) and never count
  // towards a verdict.
  std::vector<VaAdapterResult> va;
  if (g_adapters.empty() && g_allow_sw_adapter) {
    AdapterInfo w;
    w.description = "WARP software rasterizer";
    w.vendor_id = 0x1414;
    w.software = true;
    Progress("d3d11va", w.description, "");
    va.push_back(WithWatchdog<VaAdapterResult>([=] { return DoVaAdapter(w, true); }, TimeoutVa(w, true)));
  }
  for (const AdapterInfo& ai : g_adapters) {
    if (ai.software && !g_allow_sw_adapter) {
      VaAdapterResult v;
      v.adapter = ai;
      v.tr.role = "d3d11va_capability";
      v.tr.kind = "capability";
      v.tr.Skip("software_adapter",
                "a software adapter says nothing about GPU decode; it is only asked with "
                "--allow-software-adapter or --ci");
      va.push_back(std::move(v));
      continue;
    }
    Progress("d3d11va", ai.description, "");
    va.push_back(WithWatchdog<VaAdapterResult>([=] { return DoVaAdapter(ai, false); }, TimeoutVa(ai, false)));
    Sleep(kSettleMs);
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
  // Why there is no decoder, in words that match what was measured (see NoDecoderReason).
  const std::string no_dec = NoDecoderReason(codec_upper, g_is_hevc, ext_packages.size(), hevc_named_unlisted,
                                             hevc_named_unlisted_names, hevc_named_complete);
  const bool any_dec_mft = !decoders.empty() || !dec_enum.extras.empty();
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
                      : (!any_dec_mft ? no_dec : "no 720p bitstream: " + res[0].no_stream_reason);
  const std::string r_dec_1080 =
      !res[1].enabled ? not_selected_1080
                      : (!any_dec_mft ? no_dec : "no 1080p bitstream: " + res[1].no_stream_reason);
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
  // Derived from the *_status values above (they are the truth). Tri-state like them: ok, failed,
  // or not_attempted when nothing was asked.
  const Verdict v_receive =
      have_stream_720 ? v_dec_720 : Combine({v_dec_720, v_cfg});

  // MFTs that only a non-default enumeration returns, per class (see MftEnumeration).
  auto cls_dec_is = [](const std::string& cls, int w) {
    return [cls, w](const TestResult& t) {
      return t.role == "decode" && t.kind == "full" && t.width == w && t.mft_class == cls;
    };
  };
  struct ClassVerdicts {
    Verdict dec720, dec1080, activation;
  };
  auto judge_class = [&](const std::string& cls, const std::string& none_what) {
    ClassVerdicts cv;
    const std::string none = "no " + codec_upper + " decoder MFT is returned " + none_what;
    cv.dec720 = Judge(tests, cls_dec_is(cls, 1280), res[0].enabled ? none : not_selected_720);
    cv.dec1080 = Judge(tests, cls_dec_is(cls, 1920), res[1].enabled ? none : not_selected_1080);
    cv.activation = ActivationVerdict(tests, cls, none);
    return cv;
  };
  const ClassVerdicts store_cv = judge_class("store_flag_only", "only with MFT_ENUM_FLAG_UNTRUSTED_STOREMFT");
  const ClassVerdicts other_cv = judge_class("needs_other_flags",
                                             "only by an enumeration with other flags than the default one");
  const Verdict v_va_primary = VaVerdict(va, g_is_hevc ? "HEVC_VLD_MAIN" : "H264_VLD_NOFGT");
  const Verdict v_va_main10 = VaVerdict(va, "HEVC_VLD_MAIN10");
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
  for (const VaAdapterResult& v : va) {
    if (v.tr.outcome == Outcome::kFailed && (v.tr.stage == "crash" || v.tr.stage == "timeout")) {
      crashed_or_timed_out = true;
    }
  }

  Json j;
  j.BeginObject();
  j.Key("tool");
  j.BeginObject();
  j.KvS("name", "hevc-probe");
  j.KvI("report_version", 3);
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
  // Any registered package with HEVC in its name, known family or not. Read from the per-user
  // package repository (registry), which is not a documented API: `readable` says whether it worked.
  j.Key("hevc_named_packages_current_user");
  j.BeginObject();
  j.KvS("source", "per-user package repository (registry, undocumented, best effort)");
  j.KvB("readable", hevc_named_complete);
  if (!hevc_named_complete) j.KvS("error_code", Hex32(static_cast<unsigned long>(hevc_named_rc)));
  j.KvI("not_in_family_list_count", hevc_named_unlisted);
  j.Key("packages");
  j.BeginArray();
  for (const PackageHit& h : hevc_named) {
    j.BeginObject();
    j.KvS("name", h.name);
    j.KvS("version", h.version);
    j.KvS("architecture", h.architecture);
    j.KvS("family", h.family);
    j.KvB("in_known_family_list", h.in_known_family_list);
    j.EndObject();
  }
  j.EndArray();
  j.EndObject();
  // Not read by the probe: it needs elevation. null means "could not be read", never "no", as in
  // hwlab-common.ps1 (hw-inventory.ps1 reports the value).
  j.Key("provisioned_system_image");
  j.Null();
  j.KvS("provisioned_system_image_note", "not read by the probe (needs elevation); see hw-inventory.ps1");
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
    const std::map<std::string, std::string>& errs = pass == 0 ? enc_errors : dec_errors;
    if (!errs.empty()) {
      j.Key("enumeration_errors");
      j.BeginObject();
      for (const auto& kv : errs) j.KvS(kv.first.c_str(), kv.second);
      j.EndObject();
    }
    j.Key("mfts");
    j.BeginArray();
    for (const MftInfo& m : list) WriteMft(j, m);
    j.EndArray();
    j.EndObject();
  }

  // The MFTs that MFTEnumEx returns with and without MFT_ENUM_FLAG_UNTRUSTED_STOREMFT, and
  // unfiltered, side by side. The "decoders" and "encoders" objects above are the default
  // enumeration only.
  j.Key("store_mft_enumeration");
  j.BeginObject();
  j.Key("flag");
  j.BeginObject();
  j.KvS("name", "MFT_ENUM_FLAG_UNTRUSTED_STOREMFT");
  j.KvS("value", Hex32(kEnumFlagUntrustedStoreMft));
  j.KvS("documentation",
        "per docs: the constant is listed in _MFT_ENUM_FLAG and has no description on Microsoft Learn "
        "(read 2026-10-04); what it does is measured here, not assumed");
  j.EndObject();
  j.Key("decoders");
  WriteEnumeration(j, dec_enum);
  j.Key("encoders");
  WriteEnumeration(j, enc_enum);
  j.EndObject();

  // What the D3D11 video device of each adapter offers, without any MFT. Read-only: profile
  // list, output formats, decoder configuration counts, and one decoder object per supported
  // profile, released at once.
  j.Key("d3d11va");
  j.BeginObject();
  j.KvS("basis",
        "ID3D11VideoDevice: GetVideoDecoderProfileCount/GetVideoDecoderProfile, CheckVideoDecoderFormat "
        "(NV12, P010), GetVideoDecoderConfigCount at 1920x1080 and 3840x2160, and CreateVideoDecoder "
        "(object released at once); no frame is decoded");
  j.KvS("primary_profile", g_is_hevc ? "HEVC_VLD_MAIN" : "H264_VLD_NOFGT");
  j.Key("profiles_checked");
  j.BeginArray();
  for (const VaProfileDef& d : kVaProfiles) {
    if (!d.probed) continue;
    j.BeginObject();
    j.KvS("profile", d.name);
    j.KvS("guid", GuidStr(d.guid));
    j.KvS("native_output_format", d.ten_bit ? "P010" : "NV12");
    j.EndObject();
  }
  j.EndArray();
  j.Key("profile_names_known");
  j.BeginArray();
  for (const VaProfileDef& d : kVaProfiles) {
    j.BeginObject();
    j.KvS("profile", d.name);
    j.KvS("guid", GuidStr(d.guid));
    j.EndObject();
  }
  j.EndArray();
  j.Key("adapters");
  j.BeginArray();
  for (const VaAdapterResult& v : va) WriteVaAdapter(j, v);
  j.EndArray();
  j.EndObject();

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
  auto ki = [&](const char* suffix, long long v) { j.KvI((pre + suffix).c_str(), v); };
  // Number of distinct MFTs a variant returned; -1 when there was no enumeration (no Media Foundation)
  // or the variant could not run at all (every query failed).
  auto variant_count = [](const MftEnumeration& e, size_t vi) -> long long {
    if (vi >= e.variants.size()) return -1;
    const VariantResult& vr = e.variants[vi];
    bool any_ok = false;
    for (const auto& c : vr.counts) any_ok = any_ok || c.second >= 0;
    return any_ok ? static_cast<long long>(vr.mfts.size()) : -1;
  };
  auto names_of = [&](const MftEnumeration& e, const std::string& cls) {
    j.BeginArray();
    for (const MftInfo& m : e.extras) {
      if (m.mft_class == cls) j.Str(m.name + " (" + m.clsid + ")");
    }
    j.EndArray();
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
  // Derived values, tri-state like the *_status values they come from (which are the truth): ok,
  // failed, or not_attempted. A value that was not asked is never false.
  j.KvS("video_send_possible", v_hw_enc_720.status);
  j.KvS("video_send_1080p_possible", v_hw_enc_1080.status);
  j.KvS("video_receive_possible", v_receive.status);
  j.KvS("video_receive_basis", receive_basis);
  // MFTEnumEx with and without MFT_ENUM_FLAG_UNTRUSTED_STOREMFT, and unfiltered: how many decoders
  // each returned (distinct MFTs), and which ones only a non-default enumeration returns.
  ki("decoder_mft_count_default", variant_count(dec_enum, 0));
  ki("decoder_mft_count_with_store_flag", variant_count(dec_enum, 1));
  ki("decoder_mft_count_unfiltered", variant_count(dec_enum, 2));
  ki("decoder_mft_count_unfiltered_with_store_flag", variant_count(dec_enum, 3));
  ki("encoder_mft_count_default", variant_count(enc_enum, 0));
  ki("encoder_mft_count_with_store_flag", variant_count(enc_enum, 1));
  j.Key((pre + "store_mft_decoders").c_str());
  names_of(dec_enum, "store_flag_only");
  j.Key((pre + "store_mft_encoders").c_str());
  names_of(enc_enum, "store_flag_only");
  kv("store_mft_activation_status", store_cv.activation);
  kv("store_mft_decode_720p_status", store_cv.dec720);
  kv("store_mft_decode_1080p_status", store_cv.dec1080);
  j.Key((pre + "other_flags_mft_decoders").c_str());
  names_of(dec_enum, "needs_other_flags");
  kv("other_flags_mft_activation_status", other_cv.activation);
  kv("other_flags_mft_decode_720p_status", other_cv.dec720);
  kv("other_flags_mft_decode_1080p_status", other_cv.dec1080);
  // Can the GPU decode the codec through D3D11VA directly, without an MFT: ok when a
  // non-software adapter passed every check for the profile (see d3d11va).
  kv("d3d11va_decode_supported", v_va_primary);
  j.Key((pre + "d3d11va_decode_by_adapter").c_str());
  WriteVaByAdapter(j, va, g_is_hevc ? "HEVC_VLD_MAIN" : "H264_VLD_NOFGT");
  if (g_is_hevc) {
    kv("d3d11va_main10_decode_supported", v_va_main10);
    j.Key((pre + "d3d11va_main10_decode_by_adapter").c_str());
    WriteVaByAdapter(j, va, "HEVC_VLD_MAIN10");
  }
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
