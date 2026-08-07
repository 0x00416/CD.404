#include <cd404/disc/gnudb.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <map>

namespace cd404::disc {
namespace {

[[nodiscard]] std::uint32_t decimal_digit_sum(std::uint32_t value) noexcept
{
    std::uint32_t sum{};
    do {
        sum += value % 10U;
        value /= 10U;
    } while (value != 0U);
    return sum;
}

[[nodiscard]] std::string trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

[[nodiscard]] std::string decode_cddb_value(const std::string_view value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\' || index + 1 >= value.size()) {
            decoded.push_back(value[index]);
            continue;
        }
        const char escaped = value[++index];
        if (escaped == 'n') {
            decoded.push_back('\n');
        } else if (escaped == 't') {
            decoded.push_back('\t');
        } else {
            decoded.push_back(escaped);
        }
    }
    return decoded;
}

[[nodiscard]] bool is_various_artists(const std::string& artist)
{
    std::string normalized;
    normalized.reserve(artist.size());
    for (const unsigned char character : artist) {
        if (std::isalnum(character) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    return normalized == "various" || normalized == "variousartists";
}

} // namespace

std::optional<GnudbDiscIdentity> make_gnudb_disc_identity(const Toc& toc) noexcept
{
    const auto& tracks = toc.tracks();
    if (tracks.empty() || tracks.size() > 99U) {
        return std::nullopt;
    }

    GnudbDiscIdentity identity;
    identity.track_offsets.reserve(tracks.size());
    std::uint32_t checksum{};
    for (const auto& track : tracks) {
        const core::Sector absolute = track.start_lba + core::kCdProgramAreaOffsetSectors;
        if (absolute < 0 ||
            absolute > static_cast<core::Sector>(std::numeric_limits<std::uint32_t>::max())) {
            return std::nullopt;
        }
        const auto offset = static_cast<std::uint32_t>(absolute);
        identity.track_offsets.push_back(offset);
        checksum += decimal_digit_sum(
            offset / static_cast<std::uint32_t>(core::kCdSectorsPerSecond));
    }

    const core::Sector absolute_lead_out =
        toc.lead_out_lba() + core::kCdProgramAreaOffsetSectors;
    if (absolute_lead_out <= static_cast<core::Sector>(identity.track_offsets.front()) ||
        absolute_lead_out >
            static_cast<core::Sector>(std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
    }

    identity.disc_length_seconds = static_cast<std::uint32_t>(
        absolute_lead_out / core::kCdSectorsPerSecond);
    const std::uint32_t duration_seconds = identity.disc_length_seconds -
        identity.track_offsets.front() /
            static_cast<std::uint32_t>(core::kCdSectorsPerSecond);
    if (duration_seconds > 0xffffU) {
        return std::nullopt;
    }

    identity.disc_id = ((checksum % 0xffU) << 24U) |
                       (duration_seconds << 8U) |
                       static_cast<std::uint32_t>(tracks.size());
    return identity;
}

std::optional<GnudbMetadataUtf8> parse_gnudb_entry(
    std::string_view response,
    const std::size_t expected_track_count)
{
    if (expected_track_count == 0U || expected_track_count > 99U) {
        return std::nullopt;
    }

    std::map<std::string, std::string, std::less<>> fields;
    while (!response.empty()) {
        const std::size_t newline = response.find('\n');
        std::string_view line = response.substr(0, newline);
        response = newline == std::string_view::npos
            ? std::string_view{}
            : response.substr(newline + 1U);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#' || line == "." ||
            (line.size() >= 3U && std::isdigit(static_cast<unsigned char>(line[0])) != 0 &&
             std::isdigit(static_cast<unsigned char>(line[1])) != 0 &&
             std::isdigit(static_cast<unsigned char>(line[2])) != 0)) {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos || equals == 0U) {
            continue;
        }
        fields[std::string(line.substr(0, equals))] +=
            decode_cddb_value(line.substr(equals + 1U));
    }

    GnudbMetadataUtf8 metadata;
    if (const auto iterator = fields.find("DTITLE"); iterator != fields.end()) {
        const std::size_t separator = iterator->second.find(" / ");
        if (separator == std::string::npos) {
            metadata.album_title = trim(iterator->second);
        } else {
            metadata.album_artist = trim(
                std::string_view(iterator->second).substr(0, separator));
            metadata.album_title = trim(
                std::string_view(iterator->second).substr(separator + 3U));
        }
    }

    metadata.track_titles.resize(expected_track_count);
    metadata.track_artists.resize(expected_track_count);
    const bool split_track_artist = is_various_artists(metadata.album_artist);
    bool has_track_title{};
    for (std::size_t index = 0; index < expected_track_count; ++index) {
        const std::string key = "TTITLE" + std::to_string(index);
        const auto iterator = fields.find(key);
        if (iterator == fields.end()) {
            continue;
        }
        std::string title = trim(iterator->second);
        if (split_track_artist) {
            const std::size_t separator = title.find(" / ");
            if (separator != std::string::npos) {
                metadata.track_artists[index] = trim(
                    std::string_view(title).substr(0, separator));
                title = trim(std::string_view(title).substr(separator + 3U));
            }
        }
        metadata.track_titles[index] = std::move(title);
        has_track_title = has_track_title || !metadata.track_titles[index].empty();
    }

    if (metadata.album_title.empty() && metadata.album_artist.empty() && !has_track_title) {
        return std::nullopt;
    }
    return metadata;
}

} // namespace cd404::disc
