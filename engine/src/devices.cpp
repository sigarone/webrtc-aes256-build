#include "devices.h"

#include <windows.h>

#include <mmdeviceapi.h>

#include <atomic>
#include <chrono>
#include <utility>

namespace qmedia::engine {

namespace {

class Watcher final : public DeviceWatcher, public IMMNotificationClient {
 public:
  explicit Watcher(std::function<void()> changed) : changed_(std::move(changed)) {}

  bool Register() {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator_)))) {
      return false;
    }
    if (FAILED(enumerator_->RegisterEndpointNotificationCallback(this))) {
      enumerator_->Release();
      enumerator_ = nullptr;
      return false;
    }
    return true;
  }

  ~Watcher() override {
    if (enumerator_ != nullptr) {
      enumerator_->UnregisterEndpointNotificationCallback(this);
      enumerator_->Release();
    }
  }

  // IUnknown. The object lives as long as its owner; the enumerator holds no counted reference.
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  // IMMNotificationClient
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Notify(); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Notify(); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Notify(); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override {
    Notify();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

 private:
  void Notify() {
    const int64_t now = static_cast<int64_t>(GetTickCount64());
    int64_t last = last_ms_.load();
    if (now - last < 250) return;
    if (!last_ms_.compare_exchange_strong(last, now)) return;
    changed_();
  }

  std::function<void()> changed_;
  IMMDeviceEnumerator* enumerator_ = nullptr;
  std::atomic<int64_t> last_ms_{-1000};
};

}  // namespace

std::unique_ptr<DeviceWatcher> DeviceWatcher::Start(std::function<void()> changed) {
  auto w = std::make_unique<Watcher>(std::move(changed));
  if (!w->Register()) return nullptr;
  return w;
}

}  // namespace qmedia::engine
