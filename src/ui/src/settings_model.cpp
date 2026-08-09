#include <cd404/ui/settings_model.hpp>

#include <algorithm>

namespace cd404::ui {

std::size_t find_audio_endpoint_index(
    const std::span<const platform::windows::WasapiEndpoint> endpoints,
    const std::wstring_view endpoint_id) noexcept
{
    const auto selected = std::ranges::find(
        endpoints,
        endpoint_id,
        &platform::windows::WasapiEndpoint::id);
    return selected == endpoints.end()
        ? 0U
        : static_cast<std::size_t>(selected - endpoints.begin());
}

bool select_next_audio_endpoint(
    const std::span<const platform::windows::WasapiEndpoint> endpoints,
    std::size_t& selected_index,
    platform::windows::UserSettings& settings)
{
    if (endpoints.empty()) {
        return false;
    }
    selected_index = (selected_index + 1U) % endpoints.size();
    settings.audio_endpoint_id = endpoints[selected_index].id;
    return true;
}

void toggle_exclusive_output(
    platform::windows::UserSettings& settings) noexcept
{
    settings.audio_exclusive_mode = !settings.audio_exclusive_mode;
    if (!settings.audio_exclusive_mode) {
        settings.audio_allow_shared_fallback = false;
    }
}

bool toggle_shared_fallback(
    platform::windows::UserSettings& settings) noexcept
{
    if (!settings.audio_exclusive_mode) {
        return false;
    }
    settings.audio_allow_shared_fallback =
        !settings.audio_allow_shared_fallback;
    return true;
}

} // namespace cd404::ui
