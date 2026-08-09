#pragma once

#include <cstdint>

namespace cd404::platform::windows {

enum class DeviceLifecycleEvent {
    none,
    optical_media_changed,
    suspending,
    resumed,
};

// Converts raw window-message data into injectable lifecycle events. `lparam`
// is inspected only for WM_DEVICECHANGE volume notifications.
[[nodiscard]] DeviceLifecycleEvent classify_device_lifecycle_message(
    std::uint32_t message,
    std::uintptr_t wparam,
    const void* lparam) noexcept;

} // namespace cd404::platform::windows
