#include <windows.h>
#include <dbt.h>

#include <cd404/platform/windows/device_lifecycle.hpp>

namespace cd404::platform::windows {

DeviceLifecycleEvent classify_device_lifecycle_message(
    const std::uint32_t message,
    const std::uintptr_t wparam,
    const void* const lparam) noexcept
{
    if (message == WM_POWERBROADCAST) {
        if (wparam == PBT_APMSUSPEND) {
            return DeviceLifecycleEvent::suspending;
        }
        if (wparam == PBT_APMRESUMEAUTOMATIC ||
            wparam == PBT_APMRESUMESUSPEND) {
            return DeviceLifecycleEvent::resumed;
        }
        return DeviceLifecycleEvent::none;
    }

    if (message != WM_DEVICECHANGE) {
        return DeviceLifecycleEvent::none;
    }
    if ((wparam != DBT_DEVICEARRIVAL &&
         wparam != DBT_DEVICEREMOVECOMPLETE) ||
        lparam == nullptr) {
        return DeviceLifecycleEvent::none;
    }

    const auto* const header = static_cast<const DEV_BROADCAST_HDR*>(lparam);
    if (header->dbch_devicetype != DBT_DEVTYP_VOLUME ||
        header->dbch_size < sizeof(DEV_BROADCAST_VOLUME)) {
        return DeviceLifecycleEvent::none;
    }
    const auto* const volume = static_cast<const DEV_BROADCAST_VOLUME*>(lparam);
    return (volume->dbcv_flags & DBTF_MEDIA) != 0
        ? DeviceLifecycleEvent::optical_media_changed
        : DeviceLifecycleEvent::none;
}

} // namespace cd404::platform::windows
