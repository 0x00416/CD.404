#include <cd404/disc/gnudb.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <format>
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

[[nodiscard]] bool is_generated_track_title(const std::string& value)
{
    const std::string title = trim(value);
    std::string normalized;
    normalized.reserve(title.size());
    for (const unsigned char character : title) {
        if (character < 0x80U && std::isalnum(character) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    const auto numbered_ascii_placeholder = [&normalized](const std::string_view prefix) {
        if (!normalized.starts_with(prefix) || normalized.size() == prefix.size()) {
            return false;
        }
        return std::ranges::all_of(
            normalized.substr(prefix.size()),
            [](const char character) { return character >= '0' && character <= '9'; });
    };
    if (numbered_ascii_placeholder("track") ||
        numbered_ascii_placeholder("audiotrack") ||
        numbered_ascii_placeholder("datatrack")) {
        return true;
    }
    constexpr std::string_view chinese_audio_track = "\xE9\x9F\xB3\xE8\xBD\xA8";
    constexpr std::string_view chinese_data_track = "\xE6\x95\xB0\xE6\x8D\xAE\xE8\xBD\xA8";
    for (const auto prefix : {chinese_audio_track, chinese_data_track}) {
        if (!title.starts_with(prefix)) {
            continue;
        }
        const std::string remainder = trim(std::string_view(title).substr(prefix.size()));
        if (!remainder.empty() && std::ranges::all_of(remainder, [](const char character) {
                return character >= '0' && character <= '9';
            })) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept
{
    std::size_t index{};
    while (index < value.size()) {
        const unsigned char first = static_cast<unsigned char>(value[index]);
        std::size_t trailing{};
        std::uint32_t codepoint{};
        if (first <= 0x7fU) {
            trailing = 0;
            codepoint = first;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            trailing = 1;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            trailing = 2;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            trailing = 3;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (trailing > value.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1; offset <= trailing; ++offset) {
            const unsigned char continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if ((trailing == 2U && codepoint < 0x800U) ||
            (trailing == 3U && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint > 0x10ffffU) {
            return false;
        }
        index += trailing + 1U;
    }
    return true;
}

[[nodiscard]] std::optional<std::string> encode_cddb_value(
    const std::string_view value)
{
    if (!valid_utf8(value)) {
        return std::nullopt;
    }
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) {
        if (character == '\\') {
            encoded += "\\\\";
        } else if (character == '\n') {
            encoded += "\\n";
        } else if (character == '\t') {
            encoded += "\\t";
        } else if (character == '\r') {
            continue;
        } else if (character < 0x20U || character == 0x7fU) {
            return std::nullopt;
        } else {
            encoded.push_back(static_cast<char>(character));
        }
    }
    return encoded;
}

[[nodiscard]] bool append_field(
    std::string& output,
    const std::string_view key,
    const std::string_view value)
{
    const auto encoded = encode_cddb_value(value);
    if (!encoded || key.empty() || key.size() + 2U > 256U) {
        return false;
    }
    constexpr std::size_t kMaximumLineBytes = 256U;
    const std::size_t maximum_value_bytes =
        kMaximumLineBytes - key.size() - 2U; // '=' and trailing LF
    if (encoded->empty()) {
        output.append(key).append("=\n");
        return true;
    }
    std::size_t offset{};
    while (offset < encoded->size()) {
        std::size_t end = std::min(encoded->size(), offset + maximum_value_bytes);
        if (end < encoded->size()) {
            while (end > offset &&
                   (static_cast<unsigned char>((*encoded)[end]) & 0xc0U) == 0x80U) {
                --end;
            }
        }
        if (end == offset) {
            return false;
        }
        output.append(key)
            .push_back('=');
        output.append(*encoded, offset, end - offset);
        output.push_back('\n');
        offset = end;
    }
    return true;
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
        if (line.starts_with("# Revision:")) {
            const std::string revision_text = trim(line.substr(11U));
            unsigned int revision{};
            const auto [end, parse_error] = std::from_chars(
                revision_text.data(),
                revision_text.data() + revision_text.size(),
                revision);
            if (parse_error == std::errc{} &&
                end == revision_text.data() + revision_text.size()) {
                fields["__REVISION"] = revision_text;
            }
            continue;
        }
        if (line.starts_with("# Cover:")) {
            const std::string value = trim(line.substr(8U));
            if (!value.empty()) {
                fields["__COVER" + std::to_string(fields.size())] = value;
            }
            continue;
        }
        if (line.starts_with("# Artid:")) {
            const std::string value = trim(line.substr(8U));
            if (!value.empty()) {
                fields["__ARTID" + std::to_string(fields.size())] = value;
            }
            continue;
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
    if (const auto iterator = fields.find("DGENRE"); iterator != fields.end()) {
        metadata.genre = trim(iterator->second);
    }
    if (const auto iterator = fields.find("DYEAR"); iterator != fields.end()) {
        metadata.year = trim(iterator->second);
    }
    if (const auto iterator = fields.find("__REVISION"); iterator != fields.end()) {
        static_cast<void>(std::from_chars(
            iterator->second.data(),
            iterator->second.data() + iterator->second.size(),
            metadata.revision));
    }
    for (const auto& [key, value] : fields) {
        if (key.starts_with("__COVER")) {
            metadata.cover_urls.push_back(value);
        } else if (key.starts_with("__ARTID")) {
            metadata.cover_art_ids.push_back(value);
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

bool is_valid_gnudb_category(const std::string_view category) noexcept
{
    constexpr std::array categories{
        std::string_view("blues"),
        std::string_view("classical"),
        std::string_view("country"),
        std::string_view("data"),
        std::string_view("folk"),
        std::string_view("jazz"),
        std::string_view("misc"),
        std::string_view("newage"),
        std::string_view("reggae"),
        std::string_view("rock"),
        std::string_view("soundtrack"),
    };
    return std::ranges::find(categories, category) != categories.end();
}

std::optional<GnudbSubmissionEntry> make_gnudb_submission_entry(
    const Toc& toc,
    const GnudbSubmissionMetadataUtf8& metadata,
    GnudbSubmissionError& error)
{
    error = GnudbSubmissionError::none;
    const auto identity = make_gnudb_disc_identity(toc);
    if (!identity) {
        error = GnudbSubmissionError::invalid_toc;
        return std::nullopt;
    }
    if (!metadata.user_edited) {
        error = GnudbSubmissionError::not_edited;
        return std::nullopt;
    }
    if (!is_valid_gnudb_category(metadata.category)) {
        error = GnudbSubmissionError::invalid_category;
        return std::nullopt;
    }
    if (trim(metadata.album_title).empty()) {
        error = GnudbSubmissionError::album_title_missing;
        return std::nullopt;
    }
    if (trim(metadata.album_artist).empty()) {
        error = GnudbSubmissionError::album_artist_missing;
        return std::nullopt;
    }
    if (metadata.track_titles.size() != toc.tracks().size()) {
        error = GnudbSubmissionError::track_count_mismatch;
        return std::nullopt;
    }
    if (std::ranges::any_of(metadata.track_titles, [](const std::string& title) {
            return trim(title).empty() || is_generated_track_title(title);
        })) {
        error = GnudbSubmissionError::track_title_missing;
        return std::nullopt;
    }
    if (!metadata.track_artists.empty() &&
        metadata.track_artists.size() != metadata.track_titles.size()) {
        error = GnudbSubmissionError::track_count_mismatch;
        return std::nullopt;
    }

    std::string body = "# xmcd\n#\n# Track frame offsets:\n";
    for (const std::uint32_t offset : identity->track_offsets) {
        body += std::format("# {}\n", offset);
    }
    body += std::format(
        "#\n# Disc length: {} seconds\n#\n# Revision: {}\n"
        "# Submitted via: CD.404 0.2.0\n#\n",
        identity->disc_length_seconds,
        metadata.revision);
    const std::string disc_id = std::format("{:08x}", identity->disc_id);
    bool valid = append_field(body, "DISCID", disc_id) &&
        append_field(
            body,
            "DTITLE",
            trim(metadata.album_artist) + " / " + trim(metadata.album_title)) &&
        append_field(body, "DYEAR", trim(metadata.year)) &&
        append_field(body, "DGENRE", metadata.category);
    for (std::size_t index = 0; valid && index < metadata.track_titles.size(); ++index) {
        std::string title = trim(metadata.track_titles[index]);
        if (!metadata.track_artists.empty()) {
            const std::string artist = trim(metadata.track_artists[index]);
            if (!artist.empty() && artist != trim(metadata.album_artist)) {
                title = artist + " / " + title;
            }
        }
        valid = append_field(body, "TTITLE" + std::to_string(index), title);
    }
    valid = valid && append_field(body, "EXTD", "");
    for (std::size_t index = 0; valid && index < metadata.track_titles.size(); ++index) {
        valid = append_field(body, "EXTT" + std::to_string(index), "");
    }
    valid = valid && append_field(body, "PLAYORDER", "");
    if (!valid) {
        error = GnudbSubmissionError::invalid_text;
        return std::nullopt;
    }
    return GnudbSubmissionEntry{identity->disc_id, std::move(body)};
}

} // namespace cd404::disc
