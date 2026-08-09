#pragma once

#include <cd404/platform/windows/cdda_playback_engine.hpp>

#include <string>

namespace cd404::ui {

[[nodiscard]] std::wstring playback_error_message(
    const platform::windows::CddaPlaybackResult& result);

} // namespace cd404::ui
