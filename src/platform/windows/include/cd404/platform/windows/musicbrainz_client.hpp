#pragma once

#include <cd404/disc/toc.hpp>

#include <optional>
#include <filesystem>
#include <string>
#include <vector>

namespace cd404::platform::windows {

struct MusicBrainzMetadata final {
    std::wstring release_id;
    std::wstring release_group_id;
    std::wstring album_title;
    std::wstring album_artist;
    std::vector<std::wstring> track_titles;
    std::vector<std::wstring> track_artists;
    std::vector<std::wstring> track_ids;
    std::vector<std::wstring> recording_ids;
    std::vector<std::vector<std::wstring>> track_artist_ids;
    std::filesystem::path cover_art_path;
};

struct MusicBrainzLookupResult final {
    std::optional<MusicBrainzMetadata> metadata;
    unsigned long system_error{};
    unsigned long http_status{};
};

[[nodiscard]] MusicBrainzLookupResult lookup_musicbrainz(const disc::Toc& toc);

} // namespace cd404::platform::windows
