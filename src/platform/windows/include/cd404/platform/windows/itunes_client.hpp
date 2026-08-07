#pragma once

#include <cd404/disc/toc.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::platform::windows {

struct ItunesMetadata final {
    std::wstring album_title;
    std::wstring album_artist;
    std::vector<std::wstring> track_titles;
    std::vector<std::wstring> track_artists;
};

struct ItunesLookupResult final {
    std::optional<ItunesMetadata> metadata;
    unsigned long system_error{};
    unsigned long http_status{};
};

[[nodiscard]] ItunesLookupResult lookup_itunes(
    const disc::Toc& toc,
    std::wstring_view album_title,
    std::wstring_view album_artist);

} // namespace cd404::platform::windows
