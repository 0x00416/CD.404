#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cd404::ui {

struct ApplicationLaunchOptions final {
    std::optional<std::wstring> autoplay_drive_root;
};

// Parses the shell command registered for PlayCDAudioOnArrival. The accepted
// drive argument is deliberately narrow so an AutoPlay invocation cannot turn
// arbitrary paths into media-device requests.
[[nodiscard]] ApplicationLaunchOptions parse_application_launch_options(
    std::span<const std::wstring_view> arguments);

[[nodiscard]] std::optional<std::wstring> normalize_autoplay_drive_root(
    std::wstring_view argument);

} // namespace cd404::ui
