#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::core {

using LyricTime = std::int64_t;

struct LyricToken final {
    std::wstring text;
    LyricTime start_milliseconds{};
    std::optional<LyricTime> end_milliseconds;
};

struct LyricLine final {
    LyricTime start_milliseconds{};
    std::optional<LyricTime> end_milliseconds;
    std::wstring text;
    std::vector<LyricToken> tokens;
    std::wstring translation;
    std::vector<LyricToken> translation_tokens;
};

struct LyricsDocument final {
    std::vector<LyricLine> lines;
    std::wstring source;
    bool has_word_timing{};
};

struct LyricsMatchQuery final {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    LyricTime duration_milliseconds{};
};

struct LyricsMatchCandidate final {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    LyricTime duration_milliseconds{};
    bool has_synced_lyrics{};
    bool has_word_timing{};
};

[[nodiscard]] LyricsDocument parse_lrc(std::wstring_view text);
[[nodiscard]] LyricsDocument parse_yrc(std::wstring_view text);
[[nodiscard]] LyricsDocument parse_qrc(std::wstring_view text);
[[nodiscard]] LyricsDocument parse_krc(std::wstring_view text);
[[nodiscard]] std::optional<std::size_t> active_lyric_line(
    const LyricsDocument& lyrics,
    LyricTime position_milliseconds) noexcept;
[[nodiscard]] double lyric_token_progress(
    const LyricToken& token,
    LyricTime position_milliseconds,
    std::optional<LyricTime> fallback_end_milliseconds = std::nullopt) noexcept;
[[nodiscard]] double lyrics_match_score(
    const LyricsMatchQuery& query,
    const LyricsMatchCandidate& candidate) noexcept;

} // namespace cd404::core
