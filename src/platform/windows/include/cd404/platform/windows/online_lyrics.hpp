#pragma once

#include <cd404/core/lyrics.hpp>
#include <cd404/platform/windows/http_client.hpp>

#include <memory>
#include <optional>

namespace cd404::platform::windows {

struct OnlineLyricsLookupResult final {
    std::optional<core::LyricsDocument> lyrics;
    unsigned long system_error{};
    unsigned long status{};
};

[[nodiscard]] OnlineLyricsLookupResult lookup_online_lyrics(
    const core::LyricsMatchQuery& query,
    std::shared_ptr<HttpClient> http_client = {},
    bool use_cache = true);

} // namespace cd404::platform::windows
