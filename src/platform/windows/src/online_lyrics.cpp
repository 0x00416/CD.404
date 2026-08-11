#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>

#include <cd404/platform/windows/online_lyrics.hpp>
#include <cd404/platform/windows/lyrics_codecs.hpp>

#include "http_client.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <format>
#include <future>
#include <limits>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cd404::platform::windows {
namespace {

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;

struct ProviderCandidate final {
    core::LyricsMatchCandidate identity;
    core::LyricsDocument document;
    double score{};
};

[[nodiscard]] bool payload_identity_matches(
    const ProviderCandidate& candidate) noexcept
{
    if (candidate.document.lines.empty()) {
        return false;
    }
    const auto& first = candidate.document.lines.front();
    if (first.text.find(L"TME AI") != std::wstring::npos ||
        first.text.find(L"本字幕由TME AI技术生成") != std::wstring::npos) {
        return false;
    }
    if (first.start_milliseconds > 500) {
        return true;
    }
    constexpr std::wstring_view separators[]{L" - ", L" — ", L" – "};
    for (const std::wstring_view separator : separators) {
        const std::size_t position = first.text.find(separator);
        if (position == std::wstring::npos) {
            continue;
        }
        const std::wstring_view left(first.text.data(), position);
        const std::wstring_view right(
            first.text.data() + position + separator.size(),
            first.text.size() - position - separator.size());
        const core::LyricsMatchQuery expected{
            candidate.identity.title, L"", L"", 0};
        const auto title_score = [&](const std::wstring_view text) {
            return core::lyrics_match_score(
                expected,
                core::LyricsMatchCandidate{
                    std::wstring(text), L"", L"", 0, true, false});
        };
        return title_score(left) > 0.0 || title_score(right) > 0.0;
    }
    return true;
}

[[nodiscard]] std::wstring json_string(
    const JsonObject& object,
    const wchar_t* name)
{
    return object.HasKey(name)
        ? std::wstring(object.GetNamedString(name, L""))
        : std::wstring{};
}

[[nodiscard]] std::uint64_t json_unsigned(
    const JsonObject& object,
    const wchar_t* name) noexcept
{
    try {
        const double value = object.GetNamedNumber(name, 0.0);
        return std::isfinite(value) && value >= 0.0 &&
                value <= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
            ? static_cast<std::uint64_t>(value)
            : 0U;
    } catch (const winrt::hresult_error&) {
        return 0U;
    }
}

[[nodiscard]] std::wstring body_text(const HttpResponse& response)
{
    if (response.body.empty()) {
        return {};
    }
    return detail::utf8_to_wide(std::string_view(
        reinterpret_cast<const char*>(response.body.data()), response.body.size()));
}

[[nodiscard]] std::wstring joined_artists(const JsonObject& song)
{
    JsonArray artists;
    try {
        artists = song.HasKey(L"artists")
            ? song.GetNamedArray(L"artists")
            : song.GetNamedArray(L"ar", JsonArray{});
    } catch (const winrt::hresult_error&) {
        return {};
    }
    std::wstring result;
    for (std::uint32_t index{}; index < artists.Size(); ++index) {
        try {
            const std::wstring name = json_string(artists.GetObjectAt(index), L"name");
            if (name.empty()) {
                continue;
            }
            if (!result.empty()) {
                result += L" / ";
            }
            result += name;
        } catch (const winrt::hresult_error&) {
            continue;
        }
    }
    return result;
}

void attach_translations(
    core::LyricsDocument& primary,
    const core::LyricsDocument& translations)
{
    for (const auto& translation : translations.lines) {
        core::LyricLine* best{};
        core::LyricTime best_difference = 351;
        for (auto& line : primary.lines) {
            const core::LyricTime difference = std::llabs(
                line.start_milliseconds - translation.start_milliseconds);
            if (difference < best_difference) {
                best = &line;
                best_difference = difference;
            }
        }
        if (best != nullptr && best->translation.empty()) {
            best->translation = translation.text;
            best->translation_tokens = translation.tokens;
        }
    }
}

[[nodiscard]] bool likely_simplified_chinese(const std::wstring_view text) noexcept
{
    bool has_han{};
    for (const wchar_t character : text) {
        if ((character >= 0x3400 && character <= 0x4dbf) ||
            (character >= 0x4e00 && character <= 0x9fff)) {
            has_han = true;
            continue;
        }
        if ((character >= L'a' && character <= L'z') ||
            (character >= L'A' && character <= L'Z') ||
            (character >= 0x0400 && character <= 0x052f) ||
            (character >= 0x3040 && character <= 0x30ff) ||
            (character >= 0xac00 && character <= 0xd7af)) {
            return false;
        }
    }
    return has_han;
}

[[nodiscard]] bool attach_machine_translations(
    HttpClient& client,
    core::LyricsDocument& document) noexcept
{
    try {
        JsonArray request;
        std::vector<std::size_t> line_indices;
        line_indices.reserve(document.lines.size());
        for (std::size_t index{}; index < document.lines.size(); ++index) {
            const auto& line = document.lines[index];
            if (!line.translation.empty() || line.text.empty() ||
                likely_simplified_chinese(line.text)) {
                continue;
            }
            request.Append(winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                line.text));
            line_indices.push_back(index);
        }
        if (line_indices.empty()) {
            return false;
        }
        const std::string body = detail::wide_to_utf8(std::wstring(request.Stringify()));
        const HttpResponse response = client.post(
            L"edge.microsoft.com",
            L"/translate/translatetext?to=zh-Hans",
            L"Accept: application/json\r\n"
            L"Content-Type: application/json\r\n"
            L"User-Agent: CD.404/0.2.0 (admin@416.best)\r\n"
            L"sec-mesh-client-edge-version: 135.0.3179.98\r\n"
            L"sec-mesh-client-edge-channel: stable\r\n"
            L"sec-mesh-client-os: Windows\r\n"
            L"sec-mesh-client-os-version: 10.0\r\n"
            L"sec-mesh-client-arch: x86_64\r\n"
            L"sec-mesh-client-webview: 0\r\n",
            std::span(
                reinterpret_cast<const std::uint8_t*>(body.data()),
                body.size()),
            2U * 1'024U * 1'024U);
        if (response.system_error != ERROR_SUCCESS || response.status != 200U) {
            return false;
        }
        const JsonArray translated = JsonArray::Parse(body_text(response));
        const std::uint32_t count = std::min<std::uint32_t>(
            translated.Size(), static_cast<std::uint32_t>(line_indices.size()));
        bool changed{};
        for (std::uint32_t index{}; index < count; ++index) {
            const JsonArray alternatives = translated.GetObjectAt(index)
                .GetNamedArray(L"translations", JsonArray{});
            if (alternatives.Size() == 0U) {
                continue;
            }
            const std::wstring text = json_string(
                alternatives.GetObjectAt(0), L"text");
            auto& line = document.lines[line_indices[index]];
            if (!text.empty() && text != line.text) {
                line.translation = text;
                changed = true;
            }
        }
        return changed;
    } catch (const std::exception&) {
        return false;
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

[[nodiscard]] core::LyricsDocument parse_qrc_payload(
    const std::wstring_view text)
{
    core::LyricsDocument document = core::parse_qrc(text);
    return document.lines.empty() ? core::parse_lrc(text) : document;
}

[[nodiscard]] bool krc_timed_row(const std::wstring_view line) noexcept
{
    if (line.size() < 5U || line.front() != L'[') {
        return false;
    }
    std::size_t position = 1U;
    const auto consume_number = [&]() {
        const std::size_t begin = position;
        while (position < line.size() && line[position] >= L'0' &&
               line[position] <= L'9') {
            ++position;
        }
        return position != begin;
    };
    return consume_number() && position < line.size() &&
        line[position++] == L',' && consume_number() &&
        position < line.size() && line[position] == L']';
}

void attach_krc_translations_impl(
    core::LyricsDocument& document,
    const std::wstring_view plain)
{
    constexpr std::wstring_view prefix = L"[language:";
    const std::size_t begin = plain.find(prefix);
    if (begin == std::wstring_view::npos) {
        return;
    }
    const std::size_t value_begin = begin + prefix.size();
    const std::size_t end = plain.find(L']', value_begin);
    if (end == std::wstring_view::npos || end == value_begin) {
        return;
    }
    try {
        const auto decoded = detail::decode_base64(
            plain.substr(value_begin, end - value_begin));
        if (!decoded) {
            return;
        }
        const std::wstring json = detail::utf8_to_wide(std::string_view(
            reinterpret_cast<const char*>(decoded->data()), decoded->size()));
        const JsonArray content = JsonObject::Parse(json)
            .GetNamedArray(L"content", JsonArray{});
        for (std::uint32_t language_index{};
             language_index < content.Size(); ++language_index) {
            const JsonObject language = content.GetObjectAt(language_index);
            if (json_unsigned(language, L"type") != 1U) {
                continue;
            }
            const JsonArray lines = language.GetNamedArray(
                L"lyricContent", JsonArray{});
            struct TimedRow final {
                std::uint32_t raw_index{};
                std::optional<core::LyricLine> parsed;
            };
            std::vector<TimedRow> timed_rows;
            std::size_t position{};
            while (position <= plain.size()) {
                const std::size_t newline = plain.find_first_of(L"\r\n", position);
                const std::size_t row_end = newline == std::wstring_view::npos
                    ? plain.size()
                    : newline;
                const std::wstring_view raw_row = plain.substr(
                    position, row_end - position);
                if (krc_timed_row(raw_row)) {
                    TimedRow row{
                        static_cast<std::uint32_t>(timed_rows.size()),
                        std::nullopt,
                    };
                    auto parsed = core::parse_krc(raw_row);
                    if (parsed.lines.size() == 1U) {
                        row.parsed = std::move(parsed.lines.front());
                    }
                    timed_rows.push_back(std::move(row));
                }
                if (newline == std::wstring_view::npos) {
                    break;
                }
                position = newline + 1U;
                if (plain[newline] == L'\r' && position < plain.size() &&
                    plain[position] == L'\n') {
                    ++position;
                }
            }

            if (lines.Size() == timed_rows.size()) {
                for (const auto& timed_row : timed_rows) {
                    if (!timed_row.parsed) {
                        continue;
                    }
                    const auto target = std::ranges::find_if(
                        document.lines,
                        [&](const core::LyricLine& line) {
                            return line.start_milliseconds ==
                                    timed_row.parsed->start_milliseconds &&
                                line.text == timed_row.parsed->text;
                        });
                    const JsonArray row = lines.GetArrayAt(timed_row.raw_index);
                    if (target != document.lines.end() && row.Size() != 0U &&
                        target->translation.empty()) {
                        target->translation = row.GetStringAt(0);
                    }
                }
            } else {
                // Some KRC producers omit filtered credit rows from the
                // language block. Preserve compatibility with those files.
                const std::uint32_t count = std::min<std::uint32_t>(
                    lines.Size(),
                    static_cast<std::uint32_t>(document.lines.size()));
                for (std::uint32_t index{}; index < count; ++index) {
                    const JsonArray row = lines.GetArrayAt(index);
                    if (row.Size() != 0U &&
                        document.lines[index].translation.empty()) {
                        document.lines[index].translation = row.GetStringAt(0);
                    }
                }
            }
            return;
        }
    } catch (const winrt::hresult_error&) {
    }
}

[[nodiscard]] std::vector<ProviderCandidate> lrclib_candidates(
    HttpClient& client,
    const core::LyricsMatchQuery& query,
    unsigned long& system_error,
    unsigned long& status)
{
    const std::wstring path = L"/api/search?track_name=" +
        detail::percent_encode_utf8(query.title) + L"&artist_name=" +
        detail::percent_encode_utf8(query.artist) + L"&album_name=" +
        detail::percent_encode_utf8(query.album);
    const HttpResponse response = client.get(
        L"lrclib.net",
        path,
        L"Accept: application/json\r\n"
        L"User-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
        4U * 1'024U * 1'024U);
    system_error = response.system_error;
    status = response.status;
    if (response.system_error != ERROR_SUCCESS || response.status != 200U) {
        return {};
    }

    std::vector<ProviderCandidate> candidates;
    try {
        const JsonArray results = JsonArray::Parse(body_text(response));
        for (std::uint32_t index{}; index < results.Size(); ++index) {
            const JsonObject object = results.GetObjectAt(index);
            const std::wstring synced = json_string(object, L"syncedLyrics");
            if (synced.empty()) {
                continue;
            }
            ProviderCandidate candidate;
            candidate.document = core::parse_lrc(synced);
            candidate.document.source = L"LRCLIB";
            if (candidate.document.lines.empty()) {
                continue;
            }
            candidate.identity = {
                json_string(object, L"trackName"),
                json_string(object, L"artistName"),
                json_string(object, L"albumName"),
                static_cast<core::LyricTime>(json_unsigned(object, L"duration") * 1'000U),
                true,
                candidate.document.has_word_timing,
            };
            candidate.score = core::lyrics_match_score(query, candidate.identity);
            if (candidate.score >= 55.0 && payload_identity_matches(candidate)) {
                candidates.push_back(std::move(candidate));
            }
        }
    } catch (const winrt::hresult_error&) {
        system_error = ERROR_INVALID_DATA;
    }
    return candidates;
}

[[nodiscard]] std::optional<JsonArray> netease_search_keyword(
    HttpClient& client,
    const std::wstring_view keyword,
    unsigned long& system_error,
    unsigned long& status)
{
    const HttpResponse response = client.get(
        L"music.163.com",
        L"/api/search/get?s=" + detail::percent_encode_utf8(keyword) +
            L"&type=1&limit=20&offset=0",
        L"Accept: application/json\r\n"
        L"Referer: https://music.163.com/\r\n"
        L"User-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
        4U * 1'024U * 1'024U);
    system_error = response.system_error;
    status = response.status;
    if (response.system_error != ERROR_SUCCESS || response.status != 200U) {
        return std::nullopt;
    }
    try {
        const JsonObject root = JsonObject::Parse(body_text(response));
        const JsonObject result = root.GetNamedObject(L"result", JsonObject{});
        return result.HasKey(L"songs")
            ? std::optional(result.GetNamedArray(L"songs"))
            : std::nullopt;
    } catch (const winrt::hresult_error&) {
        system_error = ERROR_INVALID_DATA;
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<JsonArray> netease_search(
    HttpClient& client,
    const core::LyricsMatchQuery& query,
    unsigned long& system_error,
    unsigned long& status)
{
    const std::wstring keyword = query.artist.empty()
        ? query.title
        : query.artist + L" " + query.title;
    return netease_search_keyword(client, keyword, system_error, status);
}

[[nodiscard]] std::vector<ProviderCandidate> netease_candidates(
    HttpClient& client,
    const core::LyricsMatchQuery& query,
    unsigned long& system_error,
    unsigned long& status)
{
    const auto songs = netease_search(client, query, system_error, status);
    if (!songs) {
        return {};
    }

    struct SearchCandidate final {
        std::uint64_t id{};
        core::LyricsMatchCandidate identity;
        double score{};
    };
    std::vector<SearchCandidate> search;
    for (std::uint32_t index{}; index < songs->Size(); ++index) {
        try {
            const JsonObject song = songs->GetObjectAt(index);
            const JsonObject album = song.GetNamedObject(L"album", JsonObject{});
            SearchCandidate candidate;
            candidate.id = json_unsigned(song, L"id");
            candidate.identity = {
                json_string(song, L"name"),
                joined_artists(song),
                json_string(album, L"name"),
                static_cast<core::LyricTime>(json_unsigned(song, L"duration")),
                true,
                false,
            };
            candidate.score = core::lyrics_match_score(query, candidate.identity);
            if (candidate.id != 0U && candidate.score >= 55.0) {
                search.push_back(std::move(candidate));
            }
        } catch (const winrt::hresult_error&) {
            continue;
        }
    }
    std::ranges::sort(search, std::greater{}, &SearchCandidate::score);
    if (search.size() > 2U) {
        search.resize(2U);
    }

    std::vector<ProviderCandidate> result;
    for (auto& candidate : search) {
        const HttpResponse response = client.get(
            L"music.163.com",
            std::format(
                L"/api/song/lyric?os=pc&id={}&lv=-1&kv=-1&tv=-1&yv=-1&rv=-1",
                candidate.id),
            L"Accept: application/json\r\n"
            L"Referer: https://music.163.com/\r\n"
            L"User-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
            4U * 1'024U * 1'024U);
        system_error = response.system_error;
        status = response.status;
        if (response.system_error != ERROR_SUCCESS || response.status != 200U) {
            continue;
        }
        try {
            const JsonObject root = JsonObject::Parse(body_text(response));
            const JsonObject yrc = root.GetNamedObject(L"yrc", JsonObject{});
            const JsonObject lrc = root.GetNamedObject(L"lrc", JsonObject{});
            const std::wstring yrc_text = json_string(yrc, L"lyric");
            const std::wstring lrc_text = json_string(lrc, L"lyric");
            ProviderCandidate resolved;
            resolved.document = !yrc_text.empty()
                ? core::parse_yrc(yrc_text)
                : core::parse_lrc(lrc_text);
            if (resolved.document.lines.empty()) {
                continue;
            }
            const JsonObject translation = root.GetNamedObject(L"tlyric", JsonObject{});
            const std::wstring translation_text = json_string(translation, L"lyric");
            if (!translation_text.empty()) {
                attach_translations(resolved.document, core::parse_lrc(translation_text));
            }
            resolved.document.source = L"NetEase";
            candidate.identity.has_word_timing = resolved.document.has_word_timing;
            resolved.identity = candidate.identity;
            resolved.score = core::lyrics_match_score(query, resolved.identity);
            if (payload_identity_matches(resolved)) {
                result.push_back(std::move(resolved));
            }
        } catch (const winrt::hresult_error&) {
            system_error = ERROR_INVALID_DATA;
        }
    }
    return result;
}

void insert_string(JsonObject& object, const wchar_t* key, const std::wstring_view value)
{
    object.Insert(key, winrt::Windows::Data::Json::JsonValue::CreateStringValue(value));
}

void insert_number(JsonObject& object, const wchar_t* key, const double value)
{
    object.Insert(key, winrt::Windows::Data::Json::JsonValue::CreateNumberValue(value));
}

[[nodiscard]] std::wstring encode_base64_utf8(const std::wstring_view value)
{
    const std::string bytes = detail::wide_to_utf8(value);
    if (bytes.empty()) {
        return {};
    }
    DWORD size{};
    if (CryptBinaryToStringW(
            reinterpret_cast<const BYTE*>(bytes.data()),
            static_cast<DWORD>(bytes.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            nullptr,
            &size) == FALSE || size == 0U) {
        return {};
    }
    std::wstring encoded(size, L'\0');
    if (CryptBinaryToStringW(
            reinterpret_cast<const BYTE*>(bytes.data()),
            static_cast<DWORD>(bytes.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            encoded.data(),
            &size) == FALSE) {
        return {};
    }
    if (!encoded.empty() && encoded.back() == L'\0') {
        encoded.pop_back();
    }
    return encoded;
}

[[nodiscard]] std::optional<JsonObject> post_json(
    HttpClient& client,
    const std::wstring_view host,
    const JsonObject& body,
    unsigned long& system_error,
    unsigned long& status)
{
    const std::string bytes = detail::wide_to_utf8(std::wstring(body.Stringify()));
    const HttpResponse response = client.post(
        host,
        L"/cgi-bin/musicu.fcg",
        L"Accept: application/json\r\n"
        L"Content-Type: application/json\r\n"
        L"Cookie: tmeLoginType=-1;\r\n"
        L"User-Agent: okhttp/3.14.9\r\n",
        std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()),
        4U * 1'024U * 1'024U);
    system_error = response.system_error;
    status = response.status;
    if (response.system_error != ERROR_SUCCESS || response.status != 200U) {
        return std::nullopt;
    }
    try {
        return JsonObject::Parse(body_text(response));
    } catch (const winrt::hresult_error&) {
        system_error = ERROR_INVALID_DATA;
        return std::nullopt;
    }
}

[[nodiscard]] JsonObject qq_comm()
{
    JsonObject comm;
    insert_number(comm, L"ct", 11);
    insert_string(comm, L"cv", L"1003006");
    insert_string(comm, L"v", L"1003006");
    insert_string(comm, L"os_ver", L"15");
    insert_string(comm, L"phonetype", L"24122RKC7C");
    insert_string(comm, L"rom", L"Redmi/miro/miro:15/CD404");
    insert_string(comm, L"tmeAppID", L"qqmusiclight");
    insert_string(comm, L"nettype", L"NETWORK_WIFI");
    insert_string(comm, L"udid", L"0");
    return comm;
}

[[nodiscard]] std::optional<JsonObject> qq_request(
    HttpClient& client,
    const JsonObject& comm,
    const std::wstring_view method,
    const std::wstring_view module,
    const JsonObject& parameters,
    unsigned long& system_error,
    unsigned long& status)
{
    JsonObject request;
    insert_string(request, L"method", method);
    insert_string(request, L"module", module);
    request.Insert(L"param", parameters);
    JsonObject root;
    root.Insert(L"comm", comm);
    root.Insert(L"request", request);
    const auto response = post_json(
        client, L"u.y.qq.com", root, system_error, status);
    if (!response || json_unsigned(*response, L"code") != 0U) {
        return std::nullopt;
    }
    try {
        const JsonObject response_request = response->GetNamedObject(L"request");
        return json_unsigned(response_request, L"code") == 0U
            ? std::optional(response_request.GetNamedObject(L"data"))
            : std::nullopt;
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::vector<ProviderCandidate> qq_candidates(
    HttpClient& client,
    const core::LyricsMatchQuery& query,
    unsigned long& system_error,
    unsigned long& status)
{
    const std::wstring keyword = query.artist.empty()
        ? query.title
        : query.artist + L" " + query.title;
    const HttpResponse search_response = client.get(
        L"c.y.qq.com",
        L"/soso/fcgi-bin/client_search_cp?p=1&n=20&format=json&w=" +
            detail::percent_encode_utf8(keyword),
        L"Accept: application/json\r\n"
        L"Referer: https://y.qq.com/\r\n"
        L"User-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
        4U * 1'024U * 1'024U);
    system_error = search_response.system_error;
    status = search_response.status;
    if (search_response.system_error != ERROR_SUCCESS ||
        search_response.status != 200U) {
        return {};
    }
    struct SearchCandidate final {
        std::uint64_t song_id{};
        core::LyricsMatchCandidate identity;
        double score{};
    };
    std::vector<SearchCandidate> search;
    try {
        const JsonObject root = JsonObject::Parse(body_text(search_response));
        const JsonArray songs = root.GetNamedObject(L"data")
            .GetNamedObject(L"song").GetNamedArray(L"list");
        for (std::uint32_t index{}; index < songs.Size(); ++index) {
            const JsonObject song = songs.GetObjectAt(index);
            SearchCandidate candidate;
            candidate.song_id = json_unsigned(song, L"songid");
            std::wstring artists;
            const JsonArray singers = song.GetNamedArray(L"singer", JsonArray{});
            for (std::uint32_t singer_index{}; singer_index < singers.Size(); ++singer_index) {
                const std::wstring name = json_string(singers.GetObjectAt(singer_index), L"name");
                if (!name.empty()) {
                    artists += (artists.empty() ? L"" : L" / ") + name;
                }
            }
            candidate.identity = {
                json_string(song, L"songname"),
                artists,
                json_string(song, L"albumname"),
                static_cast<core::LyricTime>(json_unsigned(song, L"interval") * 1'000U),
                true,
                true,
            };
            candidate.score = core::lyrics_match_score(query, candidate.identity);
            if (candidate.song_id != 0U && candidate.score >= 55.0) {
                search.push_back(std::move(candidate));
            }
        }
    } catch (const winrt::hresult_error&) {
        system_error = ERROR_INVALID_DATA;
        return {};
    }
    std::ranges::sort(search, std::greater{}, &SearchCandidate::score);
    if (search.size() > 2U) {
        search.resize(2U);
    }
    if (search.empty()) {
        return {};
    }

    JsonObject comm = qq_comm();
    JsonObject session_parameters;
    insert_number(session_parameters, L"caller", 0);
    insert_string(session_parameters, L"uid", L"0");
    insert_number(session_parameters, L"vkey", 0);
    const auto session_data = qq_request(
        client,
        comm,
        L"GetSession",
        L"music.getSession.session",
        session_parameters,
        system_error,
        status);
    if (!session_data) {
        return {};
    }
    try {
        const JsonObject session = session_data->GetNamedObject(L"session");
        comm.Insert(L"uid", session.GetNamedValue(L"uid"));
        insert_string(comm, L"sid", json_string(session, L"sid"));
        insert_string(comm, L"userip", json_string(session, L"userip"));
    } catch (const winrt::hresult_error&) {
        return {};
    }

    std::vector<ProviderCandidate> result;
    for (auto& candidate : search) {
        JsonObject parameters;
        insert_string(parameters, L"albumName", encode_base64_utf8(candidate.identity.album));
        insert_number(parameters, L"crypt", 1);
        insert_number(parameters, L"ct", 19);
        insert_number(parameters, L"cv", 2111);
        insert_number(parameters, L"interval", static_cast<double>(
            candidate.identity.duration_milliseconds / 1'000));
        insert_number(parameters, L"lrc_t", 0);
        insert_number(parameters, L"qrc", 1);
        insert_number(parameters, L"qrc_t", 0);
        insert_number(parameters, L"roma", 1);
        insert_number(parameters, L"roma_t", 0);
        insert_string(parameters, L"singerName", encode_base64_utf8(candidate.identity.artist));
        insert_number(parameters, L"songID", static_cast<double>(candidate.song_id));
        insert_string(parameters, L"songName", encode_base64_utf8(candidate.identity.title));
        insert_number(parameters, L"trans", 1);
        insert_number(parameters, L"trans_t", 0);
        insert_number(parameters, L"type", 0);
        const auto data = qq_request(
            client,
            comm,
            L"GetPlayLyricInfo",
            L"music.musichallSong.PlayLyricInfo",
            parameters,
            system_error,
            status);
        if (!data) {
            continue;
        }
        const std::wstring encrypted = json_string(*data, L"lyric");
        const auto plain = detail::decode_qrc(detail::wide_to_utf8(encrypted));
        if (!plain) {
            continue;
        }
        ProviderCandidate resolved;
        resolved.document = parse_qrc_payload(*plain);
        if (resolved.document.lines.empty()) {
            continue;
        }
        const std::wstring encrypted_translation = json_string(*data, L"trans");
        if (!encrypted_translation.empty()) {
            if (const auto translation = detail::decode_qrc(
                    detail::wide_to_utf8(encrypted_translation))) {
                attach_translations(
                    resolved.document, parse_qrc_payload(*translation));
            }
        }
        resolved.document.source = L"QQ Music";
        candidate.identity.has_word_timing = resolved.document.has_word_timing;
        resolved.identity = candidate.identity;
        resolved.score = core::lyrics_match_score(query, resolved.identity);
        if (payload_identity_matches(resolved)) {
            result.push_back(std::move(resolved));
        }
    }
    return result;
}

[[nodiscard]] std::vector<ProviderCandidate> kugou_candidates(
    HttpClient& client,
    const core::LyricsMatchQuery& query,
    unsigned long& system_error,
    unsigned long& status)
{
    const std::wstring keyword = query.artist.empty()
        ? query.title
        : query.artist + L" " + query.title;
    const HttpResponse search_response = client.get(
        L"songsearch.kugou.com",
        L"/song_search_v2?keyword=" + detail::percent_encode_utf8(keyword) +
            L"&page=1&pagesize=20&platform=WebFilter",
        L"Accept: application/json\r\n"
        L"User-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
        4U * 1'024U * 1'024U);
    system_error = search_response.system_error;
    status = search_response.status;
    if (search_response.system_error != ERROR_SUCCESS || search_response.status != 200U) {
        return {};
    }
    struct SearchCandidate final {
        std::wstring hash;
        core::LyricsMatchCandidate identity;
        double score{};
    };
    std::vector<SearchCandidate> search;
    try {
        const JsonObject root = JsonObject::Parse(body_text(search_response));
        const JsonArray songs = root.GetNamedObject(L"data").GetNamedArray(L"lists");
        for (std::uint32_t index{}; index < songs.Size(); ++index) {
            const JsonObject song = songs.GetObjectAt(index);
            SearchCandidate candidate;
            candidate.hash = json_string(song, L"FileHash");
            candidate.identity = {
                json_string(song, L"SongName"),
                json_string(song, L"SingerName"),
                json_string(song, L"AlbumName"),
                static_cast<core::LyricTime>(json_unsigned(song, L"Duration") * 1'000U),
                true,
                true,
            };
            candidate.score = core::lyrics_match_score(query, candidate.identity);
            if (!candidate.hash.empty() && candidate.score >= 55.0) {
                search.push_back(std::move(candidate));
            }
        }
    } catch (const winrt::hresult_error&) {
        system_error = ERROR_INVALID_DATA;
        return {};
    }
    std::ranges::sort(search, std::greater{}, &SearchCandidate::score);
    if (search.size() > 2U) {
        search.resize(2U);
    }
    std::vector<ProviderCandidate> result;
    for (auto& candidate : search) {
        const HttpResponse lyric_search = client.get(
            L"lyrics.kugou.com",
            L"/search?ver=1&man=yes&client=pc&keyword=" +
                detail::percent_encode_utf8(candidate.identity.title) + L"&hash=" +
                detail::percent_encode_utf8(candidate.hash) + L"&duration=" +
                std::to_wstring(candidate.identity.duration_milliseconds),
            L"Accept: application/json\r\nUser-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
            4U * 1'024U * 1'024U);
        system_error = lyric_search.system_error;
        status = lyric_search.status;
        if (lyric_search.system_error != ERROR_SUCCESS || lyric_search.status != 200U) {
            continue;
        }
        try {
            const JsonObject root = JsonObject::Parse(body_text(lyric_search));
            const JsonArray lyrics = root.GetNamedArray(L"candidates", JsonArray{});
            if (lyrics.Size() == 0U) {
                continue;
            }
            const JsonObject lyric = lyrics.GetObjectAt(0);
            const std::wstring id = json_string(lyric, L"id");
            const std::wstring access_key = json_string(lyric, L"accesskey");
            if (id.empty() || access_key.empty()) {
                continue;
            }
            const HttpResponse download = client.get(
                L"lyrics.kugou.com",
                L"/download?ver=1&client=pc&id=" + detail::percent_encode_utf8(id) +
                    L"&accesskey=" + detail::percent_encode_utf8(access_key) +
                    L"&fmt=krc&charset=utf8",
                L"Accept: application/json\r\nUser-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
                8U * 1'024U * 1'024U);
            system_error = download.system_error;
            status = download.status;
            if (download.system_error != ERROR_SUCCESS || download.status != 200U) {
                continue;
            }
            const JsonObject download_root = JsonObject::Parse(body_text(download));
            const auto encrypted = detail::decode_base64(
                json_string(download_root, L"content"));
            if (!encrypted) {
                continue;
            }
            const auto plain = detail::decode_krc(*encrypted);
            if (!plain) {
                continue;
            }
            ProviderCandidate resolved;
            resolved.document = core::parse_krc(*plain);
            if (resolved.document.lines.empty()) {
                continue;
            }
            detail::attach_krc_translations(resolved.document, *plain);
            resolved.document.source = L"Kugou";
            candidate.identity.has_word_timing = resolved.document.has_word_timing;
            resolved.identity = candidate.identity;
            resolved.score = core::lyrics_match_score(query, resolved.identity);
            if (payload_identity_matches(resolved)) {
                result.push_back(std::move(resolved));
            }
        } catch (const winrt::hresult_error&) {
            system_error = ERROR_INVALID_DATA;
        }
    }
    return result;
}

[[nodiscard]] std::optional<std::uint64_t> unsigned_key(
    const std::wstring_view value) noexcept
{
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint64_t result{};
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9' ||
            result > (std::numeric_limits<std::uint64_t>::max() -
                static_cast<unsigned int>(character - L'0')) / 10U) {
            return std::nullopt;
        }
        result = result * 10U + static_cast<unsigned int>(character - L'0');
    }
    return result;
}

void append_lrclib_catalog(
    HttpClient& client,
    const std::wstring_view keywords,
    const core::LyricsMatchQuery& expected,
    OnlineLyricsCatalogResult& result)
{
    const HttpResponse response = client.get(
        L"lrclib.net",
        L"/api/search?q=" + detail::percent_encode_utf8(keywords),
        L"Accept: application/json\r\n"
        L"User-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
        4U * 1'024U * 1'024U);
    result.system_error = response.system_error;
    result.status = response.status;
    if (response.system_error != ERROR_SUCCESS || response.status != 200U) {
        return;
    }
    try {
        const JsonArray records = JsonArray::Parse(body_text(response));
        for (std::uint32_t index{}; index < records.Size(); ++index) {
            const JsonObject record = records.GetObjectAt(index);
            const std::uint64_t id = json_unsigned(record, L"id");
            if (id == 0U || json_string(record, L"syncedLyrics").empty()) {
                continue;
            }
            OnlineLyricsSearchItem item;
            item.provider = OnlineLyricsProvider::lrclib;
            item.provider_key = std::to_wstring(id);
            item.source = L"LRCLIB";
            item.identity = {
                json_string(record, L"trackName"),
                json_string(record, L"artistName"),
                json_string(record, L"albumName"),
                static_cast<core::LyricTime>(json_unsigned(record, L"duration") * 1'000U),
                true,
                false,
            };
            item.score = core::lyrics_match_score(expected, item.identity);
            result.items.push_back(std::move(item));
        }
    } catch (const winrt::hresult_error&) {
        result.system_error = ERROR_INVALID_DATA;
    }
}

void append_netease_catalog(
    HttpClient& client,
    const std::wstring_view keywords,
    const core::LyricsMatchQuery& expected,
    OnlineLyricsCatalogResult& result)
{
    const auto songs = netease_search_keyword(
        client, keywords, result.system_error, result.status);
    if (!songs) {
        return;
    }
    for (std::uint32_t index{}; index < songs->Size(); ++index) {
        try {
            const JsonObject song = songs->GetObjectAt(index);
            const std::uint64_t id = json_unsigned(song, L"id");
            if (id == 0U) {
                continue;
            }
            const JsonObject album = song.GetNamedObject(L"album", JsonObject{});
            OnlineLyricsSearchItem item;
            item.provider = OnlineLyricsProvider::netease;
            item.provider_key = std::to_wstring(id);
            item.source = L"NetEase";
            item.identity = {
                json_string(song, L"name"),
                joined_artists(song),
                json_string(album, L"name"),
                static_cast<core::LyricTime>(json_unsigned(song, L"duration")),
                true,
                false,
            };
            item.score = core::lyrics_match_score(expected, item.identity);
            result.items.push_back(std::move(item));
        } catch (const winrt::hresult_error&) {
            continue;
        }
    }
}

void append_qq_catalog(
    HttpClient& client,
    const std::wstring_view keywords,
    const core::LyricsMatchQuery& expected,
    OnlineLyricsCatalogResult& result)
{
    const HttpResponse response = client.get(
        L"c.y.qq.com",
        L"/soso/fcgi-bin/client_search_cp?p=1&n=20&format=json&w=" +
            detail::percent_encode_utf8(keywords),
        L"Accept: application/json\r\n"
        L"Referer: https://y.qq.com/\r\n"
        L"User-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
        4U * 1'024U * 1'024U);
    result.system_error = response.system_error;
    result.status = response.status;
    if (response.system_error != ERROR_SUCCESS || response.status != 200U) {
        return;
    }
    try {
        const JsonArray songs = JsonObject::Parse(body_text(response))
            .GetNamedObject(L"data").GetNamedObject(L"song").GetNamedArray(L"list");
        for (std::uint32_t index{}; index < songs.Size(); ++index) {
            const JsonObject song = songs.GetObjectAt(index);
            const std::uint64_t id = json_unsigned(song, L"songid");
            if (id == 0U) {
                continue;
            }
            std::wstring artists;
            const JsonArray singers = song.GetNamedArray(L"singer", JsonArray{});
            for (std::uint32_t singer{}; singer < singers.Size(); ++singer) {
                const std::wstring name = json_string(singers.GetObjectAt(singer), L"name");
                if (!name.empty()) {
                    artists += (artists.empty() ? L"" : L" / ") + name;
                }
            }
            OnlineLyricsSearchItem item;
            item.provider = OnlineLyricsProvider::qq_music;
            item.provider_key = std::to_wstring(id);
            item.source = L"QQ Music";
            item.identity = {
                json_string(song, L"songname"),
                std::move(artists),
                json_string(song, L"albumname"),
                static_cast<core::LyricTime>(json_unsigned(song, L"interval") * 1'000U),
                true,
                true,
            };
            item.score = core::lyrics_match_score(expected, item.identity);
            result.items.push_back(std::move(item));
        }
    } catch (const winrt::hresult_error&) {
        result.system_error = ERROR_INVALID_DATA;
    }
}

void append_kugou_catalog(
    HttpClient& client,
    const std::wstring_view keywords,
    const core::LyricsMatchQuery& expected,
    OnlineLyricsCatalogResult& result)
{
    const HttpResponse response = client.get(
        L"songsearch.kugou.com",
        L"/song_search_v2?keyword=" + detail::percent_encode_utf8(keywords) +
            L"&page=1&pagesize=20&platform=WebFilter",
        L"Accept: application/json\r\n"
        L"User-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
        4U * 1'024U * 1'024U);
    result.system_error = response.system_error;
    result.status = response.status;
    if (response.system_error != ERROR_SUCCESS || response.status != 200U) {
        return;
    }
    try {
        const JsonArray songs = JsonObject::Parse(body_text(response))
            .GetNamedObject(L"data").GetNamedArray(L"lists");
        for (std::uint32_t index{}; index < songs.Size(); ++index) {
            const JsonObject song = songs.GetObjectAt(index);
            const std::wstring hash = json_string(song, L"FileHash");
            if (hash.empty()) {
                continue;
            }
            OnlineLyricsSearchItem item;
            item.provider = OnlineLyricsProvider::kugou;
            item.provider_key = hash;
            item.source = L"Kugou";
            item.identity = {
                json_string(song, L"SongName"),
                json_string(song, L"SingerName"),
                json_string(song, L"AlbumName"),
                static_cast<core::LyricTime>(json_unsigned(song, L"Duration") * 1'000U),
                true,
                true,
            };
            item.score = core::lyrics_match_score(expected, item.identity);
            result.items.push_back(std::move(item));
        }
    } catch (const winrt::hresult_error&) {
        result.system_error = ERROR_INVALID_DATA;
    }
}

[[nodiscard]] std::optional<ProviderCandidate> resolve_lrclib_item(
    HttpClient& client,
    const OnlineLyricsSearchItem& item,
    unsigned long& system_error,
    unsigned long& status)
{
    const HttpResponse response = client.get(
        L"lrclib.net",
        L"/api/get/" + detail::percent_encode_utf8(item.provider_key),
        L"Accept: application/json\r\n"
        L"User-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
        4U * 1'024U * 1'024U);
    system_error = response.system_error;
    status = response.status;
    if (response.system_error != ERROR_SUCCESS || response.status != 200U) {
        return std::nullopt;
    }
    try {
        const JsonObject root = JsonObject::Parse(body_text(response));
        ProviderCandidate candidate;
        candidate.identity = item.identity;
        candidate.document = core::parse_lrc(json_string(root, L"syncedLyrics"));
        candidate.document.source = L"LRCLIB";
        candidate.identity.has_word_timing = candidate.document.has_word_timing;
        return candidate.document.lines.empty() ? std::nullopt
            : std::optional(std::move(candidate));
    } catch (const winrt::hresult_error&) {
        system_error = ERROR_INVALID_DATA;
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<ProviderCandidate> resolve_netease_item(
    HttpClient& client,
    const OnlineLyricsSearchItem& item,
    unsigned long& system_error,
    unsigned long& status)
{
    const auto id = unsigned_key(item.provider_key);
    if (!id) {
        system_error = ERROR_INVALID_PARAMETER;
        return std::nullopt;
    }
    const HttpResponse response = client.get(
        L"music.163.com",
        std::format(L"/api/song/lyric?os=pc&id={}&lv=-1&kv=-1&tv=-1&yv=-1&rv=-1", *id),
        L"Accept: application/json\r\n"
        L"Referer: https://music.163.com/\r\n"
        L"User-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
        4U * 1'024U * 1'024U);
    system_error = response.system_error;
    status = response.status;
    if (response.system_error != ERROR_SUCCESS || response.status != 200U) {
        return std::nullopt;
    }
    try {
        const JsonObject root = JsonObject::Parse(body_text(response));
        const std::wstring yrc = json_string(root.GetNamedObject(L"yrc", JsonObject{}), L"lyric");
        const std::wstring lrc = json_string(root.GetNamedObject(L"lrc", JsonObject{}), L"lyric");
        ProviderCandidate candidate;
        candidate.identity = item.identity;
        candidate.document = !yrc.empty() ? core::parse_yrc(yrc) : core::parse_lrc(lrc);
        if (candidate.document.lines.empty()) {
            return std::nullopt;
        }
        const std::wstring translation = json_string(
            root.GetNamedObject(L"tlyric", JsonObject{}), L"lyric");
        if (!translation.empty()) {
            attach_translations(candidate.document, core::parse_lrc(translation));
        }
        candidate.document.source = L"NetEase";
        candidate.identity.has_word_timing = candidate.document.has_word_timing;
        return candidate;
    } catch (const winrt::hresult_error&) {
        system_error = ERROR_INVALID_DATA;
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<ProviderCandidate> resolve_qq_item(
    HttpClient& client,
    const OnlineLyricsSearchItem& item,
    unsigned long& system_error,
    unsigned long& status)
{
    const auto id = unsigned_key(item.provider_key);
    if (!id) {
        system_error = ERROR_INVALID_PARAMETER;
        return std::nullopt;
    }
    JsonObject comm = qq_comm();
    JsonObject session_parameters;
    insert_number(session_parameters, L"caller", 0);
    insert_string(session_parameters, L"uid", L"0");
    insert_number(session_parameters, L"vkey", 0);
    const auto session_data = qq_request(
        client, comm, L"GetSession", L"music.getSession.session",
        session_parameters, system_error, status);
    if (!session_data) {
        return std::nullopt;
    }
    try {
        const JsonObject session = session_data->GetNamedObject(L"session");
        comm.Insert(L"uid", session.GetNamedValue(L"uid"));
        insert_string(comm, L"sid", json_string(session, L"sid"));
        insert_string(comm, L"userip", json_string(session, L"userip"));
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
    JsonObject parameters;
    insert_string(parameters, L"albumName", encode_base64_utf8(item.identity.album));
    insert_number(parameters, L"crypt", 1);
    insert_number(parameters, L"ct", 19);
    insert_number(parameters, L"cv", 2111);
    insert_number(parameters, L"interval", static_cast<double>(
        item.identity.duration_milliseconds / 1'000));
    insert_number(parameters, L"lrc_t", 0);
    insert_number(parameters, L"qrc", 1);
    insert_number(parameters, L"qrc_t", 0);
    insert_number(parameters, L"roma", 1);
    insert_number(parameters, L"roma_t", 0);
    insert_string(parameters, L"singerName", encode_base64_utf8(item.identity.artist));
    insert_number(parameters, L"songID", static_cast<double>(*id));
    insert_string(parameters, L"songName", encode_base64_utf8(item.identity.title));
    insert_number(parameters, L"trans", 1);
    insert_number(parameters, L"trans_t", 0);
    insert_number(parameters, L"type", 0);
    const auto data = qq_request(
        client, comm, L"GetPlayLyricInfo",
        L"music.musichallSong.PlayLyricInfo", parameters, system_error, status);
    if (!data) {
        return std::nullopt;
    }
    const auto plain = detail::decode_qrc(
        detail::wide_to_utf8(json_string(*data, L"lyric")));
    if (!plain) {
        return std::nullopt;
    }
    ProviderCandidate candidate;
    candidate.identity = item.identity;
    candidate.document = parse_qrc_payload(*plain);
    if (candidate.document.lines.empty()) {
        return std::nullopt;
    }
    const std::wstring encrypted_translation = json_string(*data, L"trans");
    if (!encrypted_translation.empty()) {
        if (const auto translation = detail::decode_qrc(
                detail::wide_to_utf8(encrypted_translation))) {
            attach_translations(candidate.document, parse_qrc_payload(*translation));
        }
    }
    candidate.document.source = L"QQ Music";
    candidate.identity.has_word_timing = candidate.document.has_word_timing;
    return candidate;
}

[[nodiscard]] std::optional<ProviderCandidate> resolve_kugou_item(
    HttpClient& client,
    const OnlineLyricsSearchItem& item,
    unsigned long& system_error,
    unsigned long& status)
{
    const HttpResponse lyric_search = client.get(
        L"lyrics.kugou.com",
        L"/search?ver=1&man=yes&client=pc&keyword=" +
            detail::percent_encode_utf8(item.identity.title) + L"&hash=" +
            detail::percent_encode_utf8(item.provider_key) + L"&duration=" +
            std::to_wstring(item.identity.duration_milliseconds),
        L"Accept: application/json\r\nUser-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
        4U * 1'024U * 1'024U);
    system_error = lyric_search.system_error;
    status = lyric_search.status;
    if (lyric_search.system_error != ERROR_SUCCESS || lyric_search.status != 200U) {
        return std::nullopt;
    }
    try {
        const JsonArray lyrics = JsonObject::Parse(body_text(lyric_search))
            .GetNamedArray(L"candidates", JsonArray{});
        if (lyrics.Size() == 0U) {
            return std::nullopt;
        }
        const JsonObject lyric = lyrics.GetObjectAt(0);
        const std::wstring id = json_string(lyric, L"id");
        const std::wstring key = json_string(lyric, L"accesskey");
        if (id.empty() || key.empty()) {
            return std::nullopt;
        }
        const HttpResponse download = client.get(
            L"lyrics.kugou.com",
            L"/download?ver=1&client=pc&id=" + detail::percent_encode_utf8(id) +
                L"&accesskey=" + detail::percent_encode_utf8(key) +
                L"&fmt=krc&charset=utf8",
            L"Accept: application/json\r\nUser-Agent: CD.404/0.2.0 (admin@416.best)\r\n",
            8U * 1'024U * 1'024U);
        system_error = download.system_error;
        status = download.status;
        if (download.system_error != ERROR_SUCCESS || download.status != 200U) {
            return std::nullopt;
        }
        const auto encrypted = detail::decode_base64(json_string(
            JsonObject::Parse(body_text(download)), L"content"));
        if (!encrypted) {
            return std::nullopt;
        }
        const auto plain = detail::decode_krc(*encrypted);
        if (!plain) {
            return std::nullopt;
        }
        ProviderCandidate candidate;
        candidate.identity = item.identity;
        candidate.document = core::parse_krc(*plain);
        if (candidate.document.lines.empty()) {
            return std::nullopt;
        }
        detail::attach_krc_translations(candidate.document, *plain);
        candidate.document.source = L"Kugou";
        candidate.identity.has_word_timing = candidate.document.has_word_timing;
        return candidate;
    } catch (const winrt::hresult_error&) {
        system_error = ERROR_INVALID_DATA;
        return std::nullopt;
    }
}

[[nodiscard]] std::filesystem::path cache_path(const core::LyricsMatchQuery& query)
{
    PWSTR local_app_data{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data))) {
        return {};
    }
    const std::filesystem::path base(local_app_data);
    CoTaskMemFree(local_app_data);
    const std::wstring identity = query.title + L"\n" + query.artist + L"\n" +
        query.album + L"\n" + std::to_wstring(query.duration_milliseconds);
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const wchar_t character : identity) {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= 1'099'511'628'211ULL;
    }
    return base / L"CD.404" / L"Cache" / L"lyrics" /
        (std::format(L"{:016x}.json", hash));
}

[[nodiscard]] JsonArray encode_tokens(const std::vector<core::LyricToken>& tokens)
{
    JsonArray result;
    for (const auto& token : tokens) {
        JsonObject object;
        object.Insert(L"text", winrt::Windows::Data::Json::JsonValue::CreateStringValue(token.text));
        object.Insert(L"start", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
            static_cast<double>(token.start_milliseconds)));
        if (token.end_milliseconds) {
            object.Insert(L"end", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
                static_cast<double>(*token.end_milliseconds)));
        }
        result.Append(object);
    }
    return result;
}

[[nodiscard]] std::vector<core::LyricToken> decode_tokens(const JsonArray& array)
{
    std::vector<core::LyricToken> result;
    for (std::uint32_t index{}; index < array.Size(); ++index) {
        const JsonObject object = array.GetObjectAt(index);
        core::LyricToken token;
        token.text = json_string(object, L"text");
        token.start_milliseconds = static_cast<core::LyricTime>(json_unsigned(object, L"start"));
        if (object.HasKey(L"end")) {
            token.end_milliseconds = static_cast<core::LyricTime>(json_unsigned(object, L"end"));
        }
        if (!token.text.empty()) {
            result.push_back(std::move(token));
        }
    }
    return result;
}

[[nodiscard]] std::optional<core::LyricsDocument> load_cache(
    const core::LyricsMatchQuery& query)
{
    try {
        const auto path = cache_path(query);
        if (path.empty() || !std::filesystem::is_regular_file(path)) {
            return std::nullopt;
        }
        std::ifstream stream(path, std::ios::binary);
        const std::string bytes{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()
        };
        const JsonObject root = JsonObject::Parse(detail::utf8_to_wide(bytes));
        if (json_unsigned(root, L"version") != 3U) {
            return std::nullopt;
        }
        core::LyricsDocument document;
        document.source = json_string(root, L"source");
        document.has_word_timing = root.GetNamedBoolean(L"word_timed", false);
        const JsonArray lines = root.GetNamedArray(L"lines", JsonArray{});
        for (std::uint32_t index{}; index < lines.Size(); ++index) {
            const JsonObject object = lines.GetObjectAt(index);
            core::LyricLine line;
            line.start_milliseconds = static_cast<core::LyricTime>(json_unsigned(object, L"start"));
            if (object.HasKey(L"end")) {
                line.end_milliseconds = static_cast<core::LyricTime>(json_unsigned(object, L"end"));
            }
            line.text = json_string(object, L"text");
            line.translation = json_string(object, L"translation");
            line.tokens = decode_tokens(object.GetNamedArray(L"tokens", JsonArray{}));
            line.translation_tokens = decode_tokens(
                object.GetNamedArray(L"translation_tokens", JsonArray{}));
            if (!line.text.empty()) {
                document.lines.push_back(std::move(line));
            }
        }
        return document.lines.empty()
            ? std::nullopt
            : std::optional(std::move(document));
    } catch (const std::exception&) {
        return std::nullopt;
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
}

void save_cache(
    const core::LyricsMatchQuery& query,
    const core::LyricsDocument& document) noexcept
{
    try {
        JsonObject root;
        root.Insert(L"version", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(3));
        root.Insert(L"source", winrt::Windows::Data::Json::JsonValue::CreateStringValue(document.source));
        root.Insert(L"word_timed", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(
            document.has_word_timing));
        JsonArray lines;
        for (const auto& line : document.lines) {
            JsonObject object;
            object.Insert(L"start", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
                static_cast<double>(line.start_milliseconds)));
            if (line.end_milliseconds) {
                object.Insert(L"end", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(
                    static_cast<double>(*line.end_milliseconds)));
            }
            object.Insert(L"text", winrt::Windows::Data::Json::JsonValue::CreateStringValue(line.text));
            object.Insert(L"translation", winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                line.translation));
            object.Insert(L"tokens", encode_tokens(line.tokens));
            object.Insert(L"translation_tokens", encode_tokens(line.translation_tokens));
            lines.Append(object);
        }
        root.Insert(L"lines", lines);
        const auto path = cache_path(query);
        std::filesystem::create_directories(path.parent_path());
        const std::string bytes = detail::wide_to_utf8(std::wstring(root.Stringify()));
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    } catch (const std::exception&) {
    } catch (const winrt::hresult_error&) {
    }
}

struct ProviderSearchResult final {
    std::vector<ProviderCandidate> candidates;
    unsigned long system_error{};
    unsigned long status{};
};

[[nodiscard]] ProviderSearchResult collect_provider_candidates(
    HttpClient& client,
    const core::LyricsMatchQuery& query)
{
    ProviderSearchResult result;
    result.candidates = lrclib_candidates(
        client, query, result.system_error, result.status);
    auto netease = netease_candidates(
        client, query, result.system_error, result.status);
    result.candidates.insert(
        result.candidates.end(),
        std::make_move_iterator(netease.begin()),
        std::make_move_iterator(netease.end()));
    auto qq = qq_candidates(client, query, result.system_error, result.status);
    result.candidates.insert(
        result.candidates.end(),
        std::make_move_iterator(qq.begin()),
        std::make_move_iterator(qq.end()));
    auto kugou = kugou_candidates(
        client, query, result.system_error, result.status);
    result.candidates.insert(
        result.candidates.end(),
        std::make_move_iterator(kugou.begin()),
        std::make_move_iterator(kugou.end()));
    std::ranges::stable_sort(
        result.candidates, std::greater{}, &ProviderCandidate::score);
    return result;
}

} // namespace

void detail::attach_krc_translations(
    core::LyricsDocument& document,
    const std::wstring_view plain)
{
    attach_krc_translations_impl(document, plain);
}

OnlineLyricsCatalogResult search_online_lyrics_catalog(
    const std::wstring_view keywords,
    const core::LyricsMatchQuery& expected,
    std::shared_ptr<HttpClient> http_client)
{
    if (keywords.empty() || keywords.size() > 512U) {
        return {{}, ERROR_INVALID_PARAMETER, 0};
    }
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        return {{}, static_cast<unsigned long>(error.code().value), 0};
    }
    struct ApartmentGuard final { ~ApartmentGuard() { winrt::uninit_apartment(); } } guard;
    if (!http_client) {
        http_client = make_win_http_client();
    }
    OnlineLyricsCatalogResult result;
    append_lrclib_catalog(*http_client, keywords, expected, result);
    append_netease_catalog(*http_client, keywords, expected, result);
    append_qq_catalog(*http_client, keywords, expected, result);
    append_kugou_catalog(*http_client, keywords, expected, result);
    std::ranges::stable_sort(
        result.items,
        [](const OnlineLyricsSearchItem& left,
           const OnlineLyricsSearchItem& right) {
            const bool left_kugou = left.provider == OnlineLyricsProvider::kugou;
            const bool right_kugou = right.provider == OnlineLyricsProvider::kugou;
            if (left_kugou != right_kugou) {
                return !left_kugou;
            }
            return left.score > right.score;
        });
    return result;
}

OnlineLyricsLookupResult resolve_online_lyrics_item(
    const OnlineLyricsSearchItem& item,
    std::shared_ptr<HttpClient> http_client)
{
    if (item.provider_key.empty() || item.identity.title.empty()) {
        return {std::nullopt, ERROR_INVALID_PARAMETER, 0};
    }
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        return {std::nullopt, static_cast<unsigned long>(error.code().value), 0};
    }
    struct ApartmentGuard final { ~ApartmentGuard() { winrt::uninit_apartment(); } } guard;
    if (!http_client) {
        http_client = make_win_http_client();
    }
    unsigned long system_error{};
    unsigned long status{};
    std::optional<ProviderCandidate> candidate;
    switch (item.provider) {
    case OnlineLyricsProvider::lrclib:
        candidate = resolve_lrclib_item(
            *http_client, item, system_error, status);
        break;
    case OnlineLyricsProvider::netease:
        candidate = resolve_netease_item(
            *http_client, item, system_error, status);
        break;
    case OnlineLyricsProvider::qq_music:
        candidate = resolve_qq_item(
            *http_client, item, system_error, status);
        break;
    case OnlineLyricsProvider::kugou:
        candidate = resolve_kugou_item(
            *http_client, item, system_error, status);
        break;
    }
    if (!candidate || !payload_identity_matches(*candidate)) {
        return {std::nullopt, system_error, status};
    }
    static_cast<void>(attach_machine_translations(
        *http_client, candidate->document));
    return {std::move(candidate->document), ERROR_SUCCESS, 200};
}

OnlineLyricsSearchResult search_online_lyrics(
    const core::LyricsMatchQuery& query,
    std::shared_ptr<HttpClient> http_client)
{
    if (query.title.empty() || query.duration_milliseconds <= 0 ||
        query.title.starts_with(L"音轨 ")) {
        return {{}, ERROR_INVALID_PARAMETER, 0};
    }
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        return {{}, static_cast<unsigned long>(error.code().value), 0};
    }
    struct ApartmentGuard final { ~ApartmentGuard() { winrt::uninit_apartment(); } } guard;
    if (!http_client) {
        http_client = make_win_http_client();
    }
    ProviderSearchResult found = collect_provider_candidates(*http_client, query);
    OnlineLyricsSearchResult result;
    result.system_error = found.system_error;
    result.status = found.status;
    result.candidates.reserve(found.candidates.size());
    for (auto& candidate : found.candidates) {
        result.candidates.push_back({
            std::move(candidate.identity),
            std::move(candidate.document),
            candidate.score,
        });
    }
    return result;
}

std::optional<std::size_t> select_online_lyrics_candidate(
    const std::vector<OnlineLyricsCandidate>& candidates) noexcept
{
    if (candidates.empty()) {
        return std::nullopt;
    }
    const bool has_non_kugou = std::ranges::any_of(
        candidates,
        [](const OnlineLyricsCandidate& candidate) {
            return candidate.lyrics.source != L"Kugou";
        });
    std::optional<std::size_t> selected;
    for (std::size_t index{}; index < candidates.size(); ++index) {
        const auto& candidate = candidates[index];
        if (has_non_kugou && candidate.lyrics.source == L"Kugou") {
            continue;
        }
        if (!selected) {
            selected = index;
            continue;
        }
        const auto& current = candidates[*selected];
        if (candidate.score > current.score + 0.001 ||
            (std::abs(candidate.score - current.score) <= 0.001 &&
             candidate.lyrics.has_word_timing &&
             !current.lyrics.has_word_timing)) {
            selected = index;
        }
    }
    return selected;
}

void cache_online_lyrics_selection(
    const core::LyricsMatchQuery& query,
    const core::LyricsDocument& lyrics) noexcept
{
    save_cache(query, lyrics);
}

OnlineLyricsLookupResult lookup_online_lyrics(
    const core::LyricsMatchQuery& query,
    std::shared_ptr<HttpClient> http_client,
    const bool use_cache)
{
    if (query.title.empty() || query.duration_milliseconds <= 0 ||
        query.title.starts_with(L"音轨 ")) {
        return {std::nullopt, ERROR_INVALID_PARAMETER, 0};
    }
    std::optional<core::LyricsDocument> cached = use_cache
        ? load_cache(query)
        : std::nullopt;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        return {std::nullopt, static_cast<unsigned long>(error.code().value), 0};
    }
    struct ApartmentGuard final { ~ApartmentGuard() { winrt::uninit_apartment(); } } guard;
    if (!http_client) {
        http_client = make_win_http_client();
    }
    if (cached) {
        if (attach_machine_translations(*http_client, *cached)) {
            save_cache(query, *cached);
        }
        return {std::move(cached), ERROR_SUCCESS, 200};
    }
    ProviderSearchResult found = collect_provider_candidates(*http_client, query);
    if (found.candidates.empty()) {
        return {std::nullopt, found.system_error, found.status};
    }
    std::vector<OnlineLyricsCandidate> candidates;
    candidates.reserve(found.candidates.size());
    for (auto& candidate : found.candidates) {
        candidates.push_back({
            std::move(candidate.identity),
            std::move(candidate.document),
            candidate.score,
        });
    }
    const auto selected_index = select_online_lyrics_candidate(candidates);
    if (!selected_index) {
        return {std::nullopt, found.system_error, found.status};
    }
    auto& selected = candidates[*selected_index];
    static_cast<void>(attach_machine_translations(
        *http_client, selected.lyrics));
    if (use_cache) {
        save_cache(query, selected.lyrics);
    }
    return {std::move(selected.lyrics), ERROR_SUCCESS, 200};
}

} // namespace cd404::platform::windows
