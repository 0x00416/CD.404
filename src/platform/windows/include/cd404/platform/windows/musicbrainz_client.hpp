#pragma once

#include <cd404/disc/toc.hpp>

#include <optional>
#include <filesystem>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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
    std::vector<std::uint64_t> track_lengths_milliseconds;
    std::filesystem::path cover_art_path;
    bool exact_disc_id_match{};
    bool content_match{};
};

struct MusicBrainzContentQuery final {
    std::wstring album_title;
    std::wstring album_artist;
    std::wstring year;
    std::vector<std::wstring> track_titles;
    std::vector<std::uint64_t> track_lengths_milliseconds;
};

struct MusicBrainzLookupResult final {
    std::optional<MusicBrainzMetadata> metadata;
    unsigned long system_error{};
    unsigned long http_status{};
    std::vector<MusicBrainzMetadata> candidates;
    bool used_fuzzy_fallback{};
};

struct MusicBrainzLookupPaths final {
    std::wstring exact;
    std::wstring fuzzy;
};

[[nodiscard]] std::optional<MusicBrainzLookupPaths>
make_musicbrainz_lookup_paths(const disc::Toc& toc);

// Exposed for deterministic tests and alternate HTTP transports.
[[nodiscard]] std::vector<MusicBrainzMetadata> parse_musicbrainz_candidates(
    std::span<const std::uint8_t> body,
    std::span<const std::uint64_t> expected_lengths,
    bool exact_disc_id_match);

[[nodiscard]] std::vector<MusicBrainzMetadata>
parse_musicbrainz_content_release(
    std::span<const std::uint8_t> body,
    std::size_t expected_track_count);

[[nodiscard]] MusicBrainzLookupResult lookup_musicbrainz(
    const disc::Toc& toc,
    std::wstring_view preferred_release_id = {});

// Associates a custom or reordered disc with the recordings of an existing
// MusicBrainz release. The returned release IDs are references only; callers
// must not report them as the physical release identity of the current disc.
[[nodiscard]] MusicBrainzLookupResult lookup_musicbrainz_by_content(
    const MusicBrainzContentQuery& query);

[[nodiscard]] std::filesystem::path download_musicbrainz_cover_art(
    std::wstring_view release_id,
    std::wstring_view release_group_id = {});

// Exposed for deterministic matching tests. The returned track vectors are in
// query order and contain recording/artist identities from the reference release.
[[nodiscard]] std::optional<MusicBrainzMetadata> match_musicbrainz_content(
    const MusicBrainzContentQuery& query,
    std::span<const MusicBrainzMetadata> candidates);

} // namespace cd404::platform::windows
