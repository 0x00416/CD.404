#include <windows.h>
#include <winhttp.h>

#include <cd404/disc/gnudb.hpp>
#include <cd404/platform/windows/gnudb_client.hpp>

#include "http_client.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cwctype>
#include <format>
#include <limits>
#include <ranges>
#include <span>
#include <sstream>
#include <string>

namespace cd404::platform::windows {
namespace {

[[nodiscard]] std::string trim_protocol_line(std::string line)
{
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

[[nodiscard]] std::pair<unsigned int, std::string> parse_protocol_status_line(
    const std::string_view response)
{
    const std::size_t end = response.find('\n');
    std::string line = trim_protocol_line(std::string(response.substr(0, end)));
    unsigned int status{};
    if (line.size() < 3U) {
        return {0U, std::move(line)};
    }
    const auto [status_end, status_error] = std::from_chars(
        line.data(), line.data() + 3U, status);
    if (status_error != std::errc{} || status_end != line.data() + 3U) {
        status = 0U;
    }
    return {status, std::move(line)};
}

[[nodiscard]] std::pair<std::wstring, std::wstring> hello_identity(
    const std::wstring_view email)
{
    constexpr std::wstring_view kDeveloperEmail = L"admin@416.best";
    const std::wstring_view selected = is_valid_cddb_email(email)
        ? email
        : kDeveloperEmail;
    const std::size_t at = selected.find(L'@');
    return {
        std::wstring(selected.substr(0, at)),
        std::wstring(selected.substr(at + 1U)),
    };
}

[[nodiscard]] std::wstring hello_query(const std::wstring_view email)
{
    const auto [user, host] = hello_identity(email);
    return L"&hello=" + detail::percent_encode_utf8(user) + L"+" +
        detail::percent_encode_utf8(host) +
        L"+CD404WindowsAudioCDPlayer+v0.2.0&proto=6";
}

[[nodiscard]] std::wstring make_query_path(
    const disc::GnudbDiscIdentity& identity,
    const CddbServerEndpoint& endpoint,
    const std::wstring_view email)
{
    std::wstring command = std::format(
        L"cddb+query+{:08x}+{}",
        identity.disc_id,
        identity.track_offsets.size());
    for (const std::uint32_t offset : identity.track_offsets) {
        command += std::format(L"+{}", offset);
    }
    command += std::format(L"+{}", identity.disc_length_seconds);
    return endpoint.query_path + L"?cmd=" + command + hello_query(email);
}

[[nodiscard]] std::wstring make_read_path(
    const GnudbQueryMatch& match,
    const CddbServerEndpoint& endpoint,
    const std::wstring_view email)
{
    return endpoint.query_path + L"?cmd=cddb+read+" +
           detail::percent_encode_utf8(detail::utf8_to_wide(match.category)) + L"+" +
           detail::percent_encode_utf8(detail::utf8_to_wide(match.gnucdid)) +
           hello_query(email);
}

[[nodiscard]] GnudbMetadata to_wide_metadata(const disc::GnudbMetadataUtf8& source)
{
    GnudbMetadata metadata;
    metadata.album_title = detail::utf8_to_wide(source.album_title);
    metadata.album_artist = detail::utf8_to_wide(source.album_artist);
    metadata.category = detail::utf8_to_wide(source.genre);
    metadata.year = detail::utf8_to_wide(source.year);
    metadata.revision = source.revision;
    metadata.track_titles.reserve(source.track_titles.size());
    for (const auto& title : source.track_titles) {
        metadata.track_titles.push_back(detail::utf8_to_wide(title));
    }
    metadata.track_artists.reserve(source.track_artists.size());
    for (const auto& artist : source.track_artists) {
        metadata.track_artists.push_back(detail::utf8_to_wide(artist));
    }
    for (const auto& url : source.cover_urls) {
        metadata.cover_urls.push_back(detail::utf8_to_wide(url));
    }
    for (const auto& id : source.cover_art_ids) {
        metadata.cover_art_ids.push_back(detail::utf8_to_wide(id));
    }
    return metadata;
}

} // namespace

GnudbQueryResponse parse_gnudb_query_response(const std::string_view response)
{
    std::istringstream lines{std::string(response)};
    std::string first_line;
    if (!std::getline(lines, first_line)) {
        return {};
    }
    first_line = trim_protocol_line(std::move(first_line));
    GnudbQueryResponse parsed;
    const auto [status, message] = parse_protocol_status_line(first_line);
    parsed.protocol_status = status;
    parsed.message = message;
    if (status == 0U) {
        return parsed;
    }

    std::string match_line;
    if (parsed.protocol_status == 200U) {
        match_line = first_line.substr(3U);
    } else if (parsed.protocol_status == 210U ||
               parsed.protocol_status == 211U) {
        if (!std::getline(lines, match_line)) {
            return parsed;
        }
        match_line = trim_protocol_line(std::move(match_line));
    } else {
        return parsed;
    }

    std::istringstream fields(match_line);
    GnudbQueryMatch match;
    if (!(fields >> match.category >> match.gnucdid) || match.category.empty() ||
        match.gnucdid.size() != 8U ||
        !std::ranges::all_of(match.gnucdid, [](const unsigned char character) {
            return std::isxdigit(character) != 0;
        })) {
        return parsed;
    }
    parsed.match = std::move(match);
    return parsed;
}

std::optional<CddbServerEndpoint> parse_cddb_server(std::wstring_view server)
{
    while (!server.empty() && iswspace(server.front()) != 0) {
        server.remove_prefix(1U);
    }
    while (!server.empty() && iswspace(server.back()) != 0) {
        server.remove_suffix(1U);
    }
    if (server.empty() || server.size() > 512U ||
        server.find_first_of(L"\r\n\t?#") != std::wstring_view::npos) {
        return std::nullopt;
    }
    std::wstring url(server);
    if (url.find(L"://") == std::wstring::npos) {
        url = L"https://" + url;
    }
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1L);
    components.dwUrlPathLength = static_cast<DWORD>(-1L);
    components.dwUserNameLength = static_cast<DWORD>(-1L);
    components.dwPasswordLength = static_cast<DWORD>(-1L);
    components.dwExtraInfoLength = static_cast<DWORD>(-1L);
    if (WinHttpCrackUrl(url.c_str(), 0, 0, &components) == FALSE ||
        (components.nScheme != INTERNET_SCHEME_HTTP &&
         components.nScheme != INTERNET_SCHEME_HTTPS) ||
        components.dwHostNameLength == 0U ||
        components.dwUserNameLength != 0U || components.dwPasswordLength != 0U ||
        components.dwExtraInfoLength != 0U) {
        return std::nullopt;
    }
    CddbServerEndpoint endpoint;
    endpoint.host.assign(components.lpszHostName, components.dwHostNameLength);
    endpoint.port = components.nPort;
    endpoint.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring path;
    if (components.dwUrlPathLength != 0U) {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (path.empty() || path == L"/") {
        endpoint.query_path = L"/~cddb/cddb.cgi";
        endpoint.submit_path = L"/~cddb/submit.cgi";
    } else if (path.ends_with(L"/")) {
        endpoint.query_path = path + L"cddb.cgi";
        endpoint.submit_path = path + L"submit.cgi";
    } else {
        endpoint.query_path = path;
        const std::size_t slash = path.find_last_of(L'/');
        endpoint.submit_path =
            (slash == std::wstring::npos ? L"/" : path.substr(0, slash + 1U)) +
            L"submit.cgi";
    }
    return endpoint;
}

bool is_valid_cddb_email(const std::wstring_view email) noexcept
{
    if (email.empty() || email.size() > 320U ||
        email.find_first_of(L" \t\r\n") != std::wstring_view::npos) {
        return false;
    }
    const std::size_t at = email.find(L'@');
    return at != std::wstring_view::npos && at != 0U && at + 1U < email.size() &&
        email.find(L'@', at + 1U) == std::wstring_view::npos &&
        email.substr(at + 1U).find(L'.') != std::wstring_view::npos &&
        std::all_of(email.begin(), email.end(), [](const wchar_t character) {
            return character >= L'!' && character <= L'~';
        });
}

GnudbLookupResult lookup_gnudb(
    const disc::Toc& toc,
    const CddbClientConfiguration& configuration)
{
    const auto identity = disc::make_gnudb_disc_identity(toc);
    const auto endpoint = parse_cddb_server(configuration.server);
    if (!identity || !endpoint ||
        (!configuration.email.empty() &&
         !is_valid_cddb_email(configuration.email))) {
        return {std::nullopt, ERROR_INVALID_PARAMETER, 0};
    }

    const auto query_response = detail::http_get(
        endpoint->host,
        endpoint->port,
        endpoint->secure,
        make_query_path(*identity, *endpoint, configuration.email));
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
    const GnudbQueryResponse query = parse_gnudb_query_response(query_body);
    if (!query.match) {
        return {
            std::nullopt,
            static_cast<unsigned long>(
                query.protocol_status == 0U ? ERROR_INVALID_DATA : ERROR_SUCCESS),
            query_response.status,
            query.protocol_status,
            detail::utf8_to_wide(query.message),
        };
    }

    const auto read_response = detail::http_get(
        endpoint->host,
        endpoint->port,
        endpoint->secure,
        make_read_path(*query.match, *endpoint, configuration.email));
    if (read_response.system_error != ERROR_SUCCESS || read_response.status != 200) {
        return {std::nullopt, read_response.system_error, read_response.status,
                query.protocol_status, detail::utf8_to_wide(query.message)};
    }
    const std::string entry_body(
        reinterpret_cast<const char*>(read_response.body.data()),
        read_response.body.size());
    const auto [read_status, read_message] = parse_protocol_status_line(entry_body);
    if (read_status != 210U) {
        return {
            std::nullopt,
            static_cast<unsigned long>(
                read_status == 0U ? ERROR_INVALID_DATA : ERROR_SUCCESS),
            read_response.status,
            read_status,
            detail::utf8_to_wide(read_message),
        };
    }
    const auto parsed = disc::parse_gnudb_entry(entry_body, toc.tracks().size());
    if (!parsed) {
        return {std::nullopt, ERROR_INVALID_DATA, read_response.status,
                read_status, detail::utf8_to_wide(read_message)};
    }
    GnudbMetadata metadata = to_wide_metadata(*parsed);
    metadata.category = detail::utf8_to_wide(query.match->category);
    return {std::move(metadata), ERROR_SUCCESS, read_response.status,
            read_status, detail::utf8_to_wide(read_message)};
}

CddbSubmissionResult submit_gnudb(
    const CddbClientConfiguration& configuration,
    const disc::GnudbSubmissionMetadataUtf8& metadata,
    const disc::Toc& toc,
    const CddbSubmissionMode mode)
{
    const auto request = build_cddb_submission_request(
        configuration,
        metadata,
        toc,
        mode);
    if (!request) {
        return {false, ERROR_INVALID_PARAMETER, 0, L"服务器或提交邮箱无效"};
    }
    const auto response = detail::http_post(
        request->endpoint.host,
        request->endpoint.port,
        request->endpoint.secure,
        request->endpoint.submit_path,
        request->headers,
        request->body,
        64U * 1'024U);
    const std::wstring message = response.body.empty()
        ? std::wstring{}
        : detail::utf8_to_wide(std::string_view(
              reinterpret_cast<const char*>(response.body.data()),
              response.body.size()));
    const bool accepted = response.system_error == ERROR_SUCCESS &&
        response.status >= 200U && response.status < 300U &&
        (message.starts_with(L"200") || message.starts_with(L"210"));
    return {accepted, response.system_error, response.status, message};
}

std::optional<CddbSubmissionRequest> build_cddb_submission_request(
    const CddbClientConfiguration& configuration,
    const disc::GnudbSubmissionMetadataUtf8& metadata,
    const disc::Toc& toc,
    const CddbSubmissionMode mode)
{
    const auto endpoint = parse_cddb_server(configuration.server);
    if (!endpoint || !is_valid_cddb_email(configuration.email)) {
        return std::nullopt;
    }
    disc::GnudbSubmissionError validation_error{};
    const auto entry = disc::make_gnudb_submission_entry(toc, metadata, validation_error);
    if (!entry) {
        return std::nullopt;
    }
    const std::wstring headers = std::format(
        L"Category: {}\r\nDiscid: {:08x}\r\nUser-Email: {}\r\n"
        L"Submit-Mode: {}\r\nCharset: UTF-8\r\n"
        L"X-Cddbd-Note: Submitted by CD.404 0.2.0\r\n"
        L"Content-Type: text/plain; charset=utf-8\r\n",
        detail::utf8_to_wide(metadata.category),
        entry->disc_id,
        configuration.email,
        mode == CddbSubmissionMode::test ? L"test" : L"submit");
    CddbSubmissionRequest request;
    request.endpoint = *endpoint;
    request.headers = headers;
    request.body.assign(entry->body.begin(), entry->body.end());
    return request;
}

} // namespace cd404::platform::windows
