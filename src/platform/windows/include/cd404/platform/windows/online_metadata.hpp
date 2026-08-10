#pragma once

#include <cd404/disc/toc.hpp>
#include <cd404/platform/windows/metadata_store.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::platform::windows {

struct OnlineMetadata final {
    std::wstring album_title;
    std::wstring album_artist;
    std::wstring release_mbid;
    std::wstring release_group_mbid;
    std::wstring reference_release_mbid;
    std::wstring reference_release_group_mbid;
    std::wstring cddb_category;
    std::wstring cddb_year;
    unsigned int cddb_revision{};
    std::vector<std::wstring> track_titles;
    std::vector<std::wstring> track_artists;
    std::vector<std::wstring> track_mbids;
    std::vector<std::wstring> recording_mbids;
    std::vector<std::vector<std::wstring>> track_artist_mbids;
    std::filesystem::path cover_art_path;
    std::wstring cover_art_source;
    std::vector<std::wstring> sources;
    EditableDiscMetadata editable;
    std::vector<MetadataReleaseCandidate> release_candidates;
    std::wstring selected_release_id;
    bool used_fuzzy_musicbrainz_fallback{};
    bool used_musicbrainz_content_match{};
};

struct OnlineMetadataOptions final {
    bool cddb_enabled{true};
    std::wstring cddb_server{L"gnudb.gnudb.org"};
    std::wstring cddb_email;
};

[[nodiscard]] std::optional<OnlineMetadata> lookup_online_metadata(
    const disc::Toc& toc,
    std::wstring_view seed_album_title,
    std::wstring_view seed_album_artist,
    std::wstring_view preferred_release_id = {},
    const OnlineMetadataOptions& options = {});

} // namespace cd404::platform::windows
