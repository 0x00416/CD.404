#pragma once

#include <cd404/core/lyrics.hpp>
#include <cd404/platform/windows/http_client.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::platform::windows {

struct OnlineLyricsLookupResult final {
    std::optional<core::LyricsDocument> lyrics;
    unsigned long system_error{};
    unsigned long status{};
};

struct OnlineLyricsCandidate final {
    core::LyricsMatchCandidate identity;
    core::LyricsDocument lyrics;
    double score{};
};

struct OnlineLyricsSearchResult final {
    std::vector<OnlineLyricsCandidate> candidates;
    unsigned long system_error{};
    unsigned long status{};
};

enum class OnlineLyricsProvider {
    lrclib,
    netease,
    qq_music,
    kugou,
};

struct OnlineLyricsSearchItem final {
    OnlineLyricsProvider provider{};
    std::wstring provider_key;
    std::wstring source;
    core::LyricsMatchCandidate identity;
    double score{};
};

struct OnlineLyricsCatalogResult final {
    std::vector<OnlineLyricsSearchItem> items;
    unsigned long system_error{};
    unsigned long status{};
};

[[nodiscard]] OnlineLyricsCatalogResult search_online_lyrics_catalog(
    std::wstring_view keywords,
    const core::LyricsMatchQuery& expected,
    std::shared_ptr<HttpClient> http_client = {});

[[nodiscard]] OnlineLyricsLookupResult resolve_online_lyrics_item(
    const OnlineLyricsSearchItem& item,
    std::shared_ptr<HttpClient> http_client = {});

[[nodiscard]] OnlineLyricsSearchResult search_online_lyrics(
    const core::LyricsMatchQuery& query,
    std::shared_ptr<HttpClient> http_client = {});

[[nodiscard]] std::optional<std::size_t> select_online_lyrics_candidate(
    const std::vector<OnlineLyricsCandidate>& candidates) noexcept;

void cache_online_lyrics_selection(
    const core::LyricsMatchQuery& query,
    const core::LyricsDocument& lyrics) noexcept;

[[nodiscard]] OnlineLyricsLookupResult lookup_online_lyrics(
    const core::LyricsMatchQuery& query,
    std::shared_ptr<HttpClient> http_client = {},
    bool use_cache = true);

namespace detail {

void attach_krc_translations(
    core::LyricsDocument& document,
    std::wstring_view plain);

} // namespace detail

} // namespace cd404::platform::windows
