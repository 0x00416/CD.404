#pragma once

#include <cd404/platform/windows/user_settings.hpp>
#include <cd404/platform/windows/wasapi_output.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace cd404::ui {

[[nodiscard]] std::size_t find_audio_endpoint_index(
    std::span<const platform::windows::WasapiEndpoint> endpoints,
    std::wstring_view endpoint_id) noexcept;

[[nodiscard]] bool select_next_audio_endpoint(
    std::span<const platform::windows::WasapiEndpoint> endpoints,
    std::size_t& selected_index,
    platform::windows::UserSettings& settings);

void toggle_exclusive_output(platform::windows::UserSettings& settings) noexcept;

[[nodiscard]] bool toggle_shared_fallback(
    platform::windows::UserSettings& settings) noexcept;

} // namespace cd404::ui
