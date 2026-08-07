#include <windows.h>

#include <cd404/disc/gnudb.hpp>
#include <cd404/platform/windows/gnudb_client.hpp>

#include "http_client.hpp"

#include <format>
#include <sstream>
#include <string>

namespace cd404::platform::windows {
namespace {

struct QueryMatch final {
    std::string category;
    std::string disc_id;
};

[[nodiscard]] std::optional<QueryMatch> parse_query_match(const std::string_view response)
{
    std::istringstream lines{std::string(response)};
    std::string line;
    if (!std::getline(lines, line) || line.size() < 3U) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    const std::string code = line.substr(0, 3);
    std::string match_line;
    if (code == "200") {
        match_line = line.substr(3);
    } else if (code == "210") {
        if (!std::getline(lines, match_line)) {
            return std::nullopt;
        }
        if (!match_line.empty() && match_line.back() == '\r') {
            match_line.pop_back();
        }
    } else {
        return std::nullopt;
    }

    std::istringstream fields(match_line);
    QueryMatch match;
    if (!(fields >> match.category >> match.disc_id) || match.category.empty() ||
        match.disc_id.empty()) {
        return std::nullopt;
    }
    return match;
}

[[nodiscard]] std::wstring make_query_path(const disc::GnudbDiscIdentity& identity)
{
    std::wstring command = std::format(
        L"cddb+query+{:08x}+{}",
        identity.disc_id,
        identity.track_offsets.size());
    for (const std::uint32_t offset : identity.track_offsets) {
        command += std::format(L"+{}", offset);
    }
    command += std::format(L"+{}", identity.disc_length_seconds);
    return L"/~cddb/cddb.cgi?cmd=" + command +
           L"&hello=0x00416+users.noreply.github.com+CD404+0.1&proto=6";
}

[[nodiscard]] std::wstring make_read_path(const QueryMatch& match)
{
    return L"/~cddb/cddb.cgi?cmd=cddb+read+" +
           detail::percent_encode_utf8(detail::utf8_to_wide(match.category)) + L"+" +
           detail::percent_encode_utf8(detail::utf8_to_wide(match.disc_id)) +
           L"&hello=0x00416+users.noreply.github.com+CD404+0.1&proto=6";
}

[[nodiscard]] GnudbMetadata to_wide_metadata(const disc::GnudbMetadataUtf8& source)
{
    GnudbMetadata metadata;
    metadata.album_title = detail::utf8_to_wide(source.album_title);
    metadata.album_artist = detail::utf8_to_wide(source.album_artist);
    metadata.track_titles.reserve(source.track_titles.size());
    for (const auto& title : source.track_titles) {
        metadata.track_titles.push_back(detail::utf8_to_wide(title));
    }
    metadata.track_artists.reserve(source.track_artists.size());
    for (const auto& artist : source.track_artists) {
        metadata.track_artists.push_back(detail::utf8_to_wide(artist));
    }
    return metadata;
}

} // namespace

GnudbLookupResult lookup_gnudb(const disc::Toc& toc)
{
    const auto identity = disc::make_gnudb_disc_identity(toc);
    if (!identity) {
        return {std::nullopt, ERROR_INVALID_PARAMETER, 0};
    }

    const auto query_response = detail::https_get(
        L"gnudb.gnudb.org", make_query_path(*identity));
    if (query_response.system_error != ERROR_SUCCESS || query_response.status != 200) {
        return {
            std::nullopt,
            query_response.system_error,
            query_response.status,
        };
    }
    const std::string query_body(
        reinterpret_cast<const char*>(query_response.body.data()),
        query_response.body.size());
    const auto match = parse_query_match(query_body);
    if (!match) {
        return {std::nullopt, ERROR_SUCCESS, query_response.status};
    }

    const auto read_response = detail::https_get(
        L"gnudb.gnudb.org", make_read_path(*match));
    if (read_response.system_error != ERROR_SUCCESS || read_response.status != 200) {
        return {
            std::nullopt,
            read_response.system_error,
            read_response.status,
        };
    }
    const std::string entry_body(
        reinterpret_cast<const char*>(read_response.body.data()),
        read_response.body.size());
    const auto parsed = disc::parse_gnudb_entry(entry_body, toc.tracks().size());
    if (!parsed) {
        return {std::nullopt, ERROR_INVALID_DATA, read_response.status};
    }
    return {to_wide_metadata(*parsed), ERROR_SUCCESS, read_response.status};
}

} // namespace cd404::platform::windows
