#pragma once

#include <cd404/disc/toc.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::disc {

struct GnudbDiscIdentity final {
    std::uint32_t disc_id{};
    std::vector<std::uint32_t> track_offsets;
    std::uint32_t disc_length_seconds{};
};

struct GnudbMetadataUtf8 final {
    std::string album_title;
    std::string album_artist;
    std::string genre;
    std::string year;
    std::vector<std::string> track_titles;
    std::vector<std::string> track_artists;
    std::vector<std::string> cover_urls;
    std::vector<std::string> cover_art_ids;
    unsigned int revision{};
};

enum class GnudbSubmissionError {
    none,
    invalid_toc,
    not_edited,
    invalid_category,
    album_title_missing,
    album_artist_missing,
    track_count_mismatch,
    track_title_missing,
    invalid_text,
};

struct GnudbSubmissionMetadataUtf8 final {
    std::string album_title;
    std::string album_artist;
    std::string category{"misc"};
    std::string year;
    std::vector<std::string> track_titles;
    std::vector<std::string> track_artists;
    unsigned int revision{};
    bool user_edited{};
};

struct GnudbSubmissionEntry final {
    std::uint32_t disc_id{};
    std::string body;
};

// CDDB uses absolute frame offsets, including the conventional 150-sector
// lead-in, and a checksum derived from each track's starting second.
[[nodiscard]] std::optional<GnudbDiscIdentity> make_gnudb_disc_identity(
    const Toc& toc) noexcept;

// Parses a protocol-level-6 UTF-8 xmcd response. Repeated fields are joined,
// as required for long CDDB values split across multiple lines.
[[nodiscard]] std::optional<GnudbMetadataUtf8> parse_gnudb_entry(
    std::string_view response,
    std::size_t expected_track_count);

[[nodiscard]] bool is_valid_gnudb_category(std::string_view category) noexcept;

// Builds a protocol-level-6 UTF-8 xmcd body. The function rejects untouched
// or incomplete editor data so callers cannot accidentally submit generated
// placeholder metadata to a public CDDB/freedb-compatible service.
[[nodiscard]] std::optional<GnudbSubmissionEntry> make_gnudb_submission_entry(
    const Toc& toc,
    const GnudbSubmissionMetadataUtf8& metadata,
    GnudbSubmissionError& error);

} // namespace cd404::disc
