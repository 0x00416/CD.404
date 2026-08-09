#pragma once

#include <cd404/core/cd_time.hpp>
#include <cd404/disc/toc.hpp>

#include <map>
#include <string>

namespace cd404::platform::windows {

struct SavedPlaybackPosition final {
    unsigned int track_number{};
    core::SampleFrame offset_frames{};
};

struct UserSettings final {
    float volume{1.0F};
    bool listenbrainz_reporting_enabled{true};
    std::map<std::wstring, SavedPlaybackPosition> playback_positions;
};

// Stable TOC-derived key used only for local resume state. It does not contain
// album metadata, drive names, paths, or other machine-identifying data.
[[nodiscard]] std::wstring make_disc_settings_key(const disc::Toc& toc);

// JSON conversion is exposed for deterministic tests. Tokens are deliberately
// absent from UserSettings and remain in Windows Credential Manager.
[[nodiscard]] std::wstring encode_user_settings(const UserSettings& settings);
[[nodiscard]] UserSettings decode_user_settings(const std::wstring& json) noexcept;

[[nodiscard]] UserSettings load_user_settings() noexcept;
[[nodiscard]] bool save_user_settings(const UserSettings& settings) noexcept;

} // namespace cd404::platform::windows
