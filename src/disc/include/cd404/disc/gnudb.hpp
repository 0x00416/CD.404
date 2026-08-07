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
    std::vector<std::string> track_titles;
    std::vector<std::string> track_artists;
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

} // namespace cd404::disc
