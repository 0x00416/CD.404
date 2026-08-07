#pragma once

#include <cd404/disc/toc.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cd404::platform::windows {

struct GnudbMetadata final {
    std::wstring album_title;
    std::wstring album_artist;
    std::vector<std::wstring> track_titles;
    std::vector<std::wstring> track_artists;
};

struct GnudbLookupResult final {
    std::optional<GnudbMetadata> metadata;
    unsigned long system_error{};
    unsigned long http_status{};
};

[[nodiscard]] GnudbLookupResult lookup_gnudb(const disc::Toc& toc);

} // namespace cd404::platform::windows
