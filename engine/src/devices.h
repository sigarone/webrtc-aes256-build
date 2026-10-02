// Watches the Windows audio endpoints (IMMNotificationClient) and reports "something changed" so the
// host can list the devices again. The callback never carries a device id or name.
#pragma once

#include <functional>
#include <memory>

namespace qmedia::engine {

class DeviceWatcher {
 public:
  // `changed` runs on a Windows notification thread; calls are coalesced to at most one per
  // 250 ms. Returns nullptr if the notification client could not be registered.
  static std::unique_ptr<DeviceWatcher> Start(std::function<void()> changed);
  virtual ~DeviceWatcher() = default;
};

}  // namespace qmedia::engine
