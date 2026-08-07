#pragma once

#include <cd404/disc/toc.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::platform::windows {

struct OnlineMetadata final {
    std::wstring album_title;
    std::wstring album_artist;
    std::vector<std::wstring> track_titles;
    std::vector<std::wstring> track_artists;
    std::filesystem::path cover_art_path;
    std::vector<std::wstring> sources;
};

[[nodiscard]] std::optional<OnlineMetadata> lookup_online_metadata(
    const disc::Toc& toc,
    std::wstring_view seed_album_title,
    std::wstring_view seed_album_artist);

} // namespace cd404::platform::windows
