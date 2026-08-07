#include <windows.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>

#include <cd404/platform/windows/itunes_client.hpp>

#include "http_client.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <format>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace cd404::platform::windows {
namespace {

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;

struct AlbumCandidate final {
    std::uint64_t collection_id{};
    std::wstring title;
    std::wstring artist;
    std::uint32_t track_count{};
    std::uint64_t score{};
};

struct TrackCandidate final {
    std::uint32_t number{};
    std::uint64_t duration_milliseconds{};
    std::wstring title;
    std::wstring artist;
};

[[nodiscard]] std::wstring normalized_name(const std::wstring_view value)
{
    std::wstring normalized;
    normalized.reserve(value.size());
    for (const wchar_t character : value) {
        if (std::iswalnum(character) != 0) {
            normalized.push_back(static_cast<wchar_t>(std::towlower(character)));
        }
    }
    return normalized;
}

[[nodiscard]] std::wstring json_string(
    const JsonObject& object,
    const wchar_t* key)
{
    return object.HasKey(key) ? std::wstring(object.GetNamedString(key, L"")) : std::wstring{};
}

[[nodiscard]] std::uint64_t json_unsigned(
    const JsonObject& object,
    const wchar_t* key)
{
    if (!object.HasKey(key)) {
        return 0;
    }
    const double value = object.GetNamedNumber(key, 0.0);
    return std::isfinite(value) && value >= 0.0 &&
            value <= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
        ? static_cast<std::uint64_t>(value)
        : 0;
}

[[nodiscard]] std::optional<JsonArray> parse_results(
    const std::vector<std::uint8_t>& body)
{
    const std::string utf8(
        reinterpret_cast<const char*>(body.data()),
        body.size());
    const std::wstring json_text = detail::utf8_to_wide(utf8);
    if (json_text.empty()) {
        return std::nullopt;
    }
    try {
        const JsonObject root = JsonObject::Parse(json_text);
        return root.HasKey(L"results")
            ? std::optional(root.GetNamedArray(L"results"))
            : std::nullopt;
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<AlbumCandidate> select_album(
    const JsonArray& results,
    const std::wstring_view expected_title,
    const std::wstring_view expected_artist,
    const std::size_t expected_tracks)
{
    const std::wstring title_key = normalized_name(expected_title);
    const std::wstring artist_key = normalized_name(expected_artist);
    std::optional<AlbumCandidate> best;
    for (std::uint32_t index = 0; index < results.Size(); ++index) {
        try {
            const JsonObject object = results.GetObjectAt(index);
            AlbumCandidate candidate;
            candidate.collection_id = json_unsigned(object, L"collectionId");
            candidate.title = json_string(object, L"collectionName");
            candidate.artist = json_string(object, L"artistName");
            candidate.track_count = static_cast<std::uint32_t>(
                json_unsigned(object, L"trackCount"));
            if (candidate.collection_id == 0U || normalized_name(candidate.title) != title_key ||
                (!artist_key.empty() && normalized_name(candidate.artist) != artist_key)) {
                continue;
            }
            const std::uint64_t track_difference = candidate.track_count > expected_tracks
                ? candidate.track_count - expected_tracks
                : expected_tracks - candidate.track_count;
            candidate.score = track_difference;
            if (!best || candidate.score < best->score) {
                best = std::move(candidate);
            }
        } catch (const winrt::hresult_error&) {
            continue;
        }
    }
    return best;
}

[[nodiscard]] std::optional<ItunesMetadata> select_tracks(
    const JsonArray& results,
    const AlbumCandidate& album,
    const disc::Toc& toc)
{
    std::map<std::uint32_t, std::vector<TrackCandidate>> discs;
    for (std::uint32_t index = 0; index < results.Size(); ++index) {
        try {
            const JsonObject object = results.GetObjectAt(index);
            if (json_string(object, L"wrapperType") != L"track" ||
                json_string(object, L"kind") != L"song" ||
                json_unsigned(object, L"collectionId") != album.collection_id) {
                continue;
            }
            const auto disc_number = static_cast<std::uint32_t>(
                std::max<std::uint64_t>(json_unsigned(object, L"discNumber"), 1U));
            TrackCandidate track;
            track.number = static_cast<std::uint32_t>(json_unsigned(object, L"trackNumber"));
            track.duration_milliseconds = json_unsigned(object, L"trackTimeMillis");
            track.title = json_string(object, L"trackName");
            track.artist = json_string(object, L"artistName");
            if (track.number != 0U && !track.title.empty()) {
                discs[disc_number].push_back(std::move(track));
            }
        } catch (const winrt::hresult_error&) {
            continue;
        }
    }

    const auto& toc_tracks = toc.tracks();
    std::vector<TrackCandidate>* best_tracks{};
    std::uint64_t best_score = std::numeric_limits<std::uint64_t>::max();
    for (auto& [disc_number, tracks] : discs) {
        static_cast<void>(disc_number);
        if (tracks.size() != toc_tracks.size()) {
            continue;
        }
        std::ranges::sort(tracks, {}, &TrackCandidate::number);
        std::uint64_t score{};
        bool valid = true;
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            if (tracks[index].number != index + 1U ||
                tracks[index].duration_milliseconds == 0U) {
                valid = false;
                break;
            }
            const std::uint64_t expected = static_cast<std::uint64_t>(
                toc_tracks[index].frame_count * 1'000 /
                core::kCdSampleFramesPerSecond);
            const std::uint64_t actual = tracks[index].duration_milliseconds;
            score += actual > expected ? actual - expected : expected - actual;
        }
        constexpr std::uint64_t kMaximumAverageDifferenceMilliseconds = 15'000U;
        if (valid && score <= kMaximumAverageDifferenceMilliseconds * tracks.size() &&
            score < best_score) {
            best_score = score;
            best_tracks = &tracks;
        }
    }
    if (best_tracks == nullptr) {
        return std::nullopt;
    }

    ItunesMetadata metadata;
    metadata.album_title = album.title;
    metadata.album_artist = album.artist;
    metadata.track_titles.reserve(best_tracks->size());
    metadata.track_artists.reserve(best_tracks->size());
    for (const auto& track : *best_tracks) {
        metadata.track_titles.push_back(track.title);
        metadata.track_artists.push_back(track.artist);
    }
    return metadata;
}

} // namespace

ItunesLookupResult lookup_itunes(
    const disc::Toc& toc,
    const std::wstring_view album_title,
    const std::wstring_view album_artist)
{
    if (toc.tracks().empty() || album_title.empty()) {
        return {std::nullopt, ERROR_INVALID_PARAMETER, 0};
    }

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& error) {
        return {
            std::nullopt,
            static_cast<unsigned long>(error.code().value),
            0,
        };
    }
    struct ApartmentGuard final {
        ~ApartmentGuard() { winrt::uninit_apartment(); }
    } apartment_guard;

    const std::wstring term = std::wstring(album_title) + L" " +
                              std::wstring(album_artist);
    const std::wstring search_path = L"/search?term=" +
        detail::percent_encode_utf8(term) +
        L"&media=music&entity=album&limit=10";
    const auto search_response = detail::https_get(
        L"itunes.apple.com", search_path, 4U * 1'024U * 1'024U);
    if (search_response.system_error != ERROR_SUCCESS || search_response.status != 200) {
        return {std::nullopt, search_response.system_error, search_response.status};
    }
    const auto search_results = parse_results(search_response.body);
    if (!search_results) {
        return {std::nullopt, ERROR_INVALID_DATA, search_response.status};
    }
    const auto album = select_album(
        *search_results,
        album_title,
        album_artist,
        toc.tracks().size());
    if (!album) {
        return {std::nullopt, ERROR_SUCCESS, search_response.status};
    }

    const std::wstring lookup_path = std::format(
        L"/lookup?id={}&entity=song&limit=200",
        album->collection_id);
    const auto lookup_response = detail::https_get(
        L"itunes.apple.com", lookup_path, 4U * 1'024U * 1'024U);
    if (lookup_response.system_error != ERROR_SUCCESS || lookup_response.status != 200) {
        return {std::nullopt, lookup_response.system_error, lookup_response.status};
    }
    const auto lookup_results = parse_results(lookup_response.body);
    if (!lookup_results) {
        return {std::nullopt, ERROR_INVALID_DATA, lookup_response.status};
    }
    return {
        select_tracks(*lookup_results, *album, toc),
        ERROR_SUCCESS,
        lookup_response.status,
    };
}

} // namespace cd404::platform::windows
