#include <cd404/core/lyrics.hpp>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <map>
#include <numeric>
#include <utility>

namespace cd404::core {
namespace {

enum class TimestampKind { line, word };

struct TimestampMarker final {
    std::size_t begin{};
    std::size_t end{};
    LyricTime time{};
    TimestampKind kind{TimestampKind::line};
};

struct TimedEntry final {
    std::optional<LyricTime> end;
    bool boundary{};
    bool trailing_timestamp{};
    std::size_t order{};
    std::wstring text;
    LyricTime time{};
    std::vector<LyricToken> tokens;
};

enum class VendorLyricsFormat { yrc, qrc, krc };

[[nodiscard]] bool whitespace(const wchar_t value) noexcept
{
    return std::iswspace(value) != 0 || value == 0xfeff;
}

[[nodiscard]] std::wstring trim(const std::wstring_view value)
{
    std::size_t begin{};
    while (begin < value.size() && whitespace(value[begin])) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && whitespace(value[end - 1U])) {
        --end;
    }
    return std::wstring(value.substr(begin, end - begin));
}

[[nodiscard]] std::wstring normalized_text(const std::wstring_view value)
{
    std::wstring result = trim(value);
    const std::pair<std::wstring_view, std::wstring_view> entities[]{
        {L"&apos;", L"'"}, {L"&amp;", L"&"},
    };
    for (const auto& [from, to] : entities) {
        for (std::size_t position{};
             (position = result.find(from, position)) != std::wstring::npos;) {
            result.replace(position, from.size(), to);
            position += to.size();
        }
    }
    return result;
}

[[nodiscard]] bool parse_timestamp(
    const std::wstring_view value,
    LyricTime& result) noexcept
{
    const std::size_t separator = value.find(L':');
    if (separator == std::wstring_view::npos || separator == 0U ||
        separator + 2U >= value.size()) {
        return false;
    }
    const auto parse_digits = [](const std::wstring_view digits,
                                  std::uint64_t& number) noexcept {
        if (digits.empty()) {
            return false;
        }
        number = 0;
        for (const wchar_t digit : digits) {
            if (digit < L'0' || digit > L'9') {
                return false;
            }
            number = number * 10U + static_cast<std::uint64_t>(digit - L'0');
        }
        return true;
    };

    std::uint64_t minutes{};
    if (!parse_digits(value.substr(0, separator), minutes)) {
        return false;
    }
    const std::wstring_view seconds_and_fraction = value.substr(separator + 1U);
    const std::size_t fraction_separator =
        seconds_and_fraction.find_first_of(L".:");
    const std::wstring_view seconds_text = fraction_separator == std::wstring_view::npos
        ? seconds_and_fraction
        : seconds_and_fraction.substr(0, fraction_separator);
    std::uint64_t seconds{};
    if (seconds_text.size() != 2U || !parse_digits(seconds_text, seconds) ||
        seconds >= 60U) {
        return false;
    }
    std::uint64_t milliseconds{};
    if (fraction_separator != std::wstring_view::npos) {
        const std::wstring_view fraction = seconds_and_fraction.substr(
            fraction_separator + 1U);
        if (fraction.empty() || fraction.size() > 3U ||
            !parse_digits(fraction, milliseconds)) {
            return false;
        }
        if (fraction.size() == 1U) {
            milliseconds *= 100U;
        } else if (fraction.size() == 2U) {
            milliseconds *= 10U;
        }
    }
    if (minutes > static_cast<std::uint64_t>(
            std::numeric_limits<LyricTime>::max() / 60'000)) {
        return false;
    }
    result = static_cast<LyricTime>(minutes * 60'000U + seconds * 1'000U +
                                    milliseconds);
    return true;
}

[[nodiscard]] std::vector<TimestampMarker> markers(const std::wstring_view line)
{
    std::vector<TimestampMarker> result;
    for (std::size_t position{}; position < line.size(); ++position) {
        const wchar_t opening = line[position];
        if (opening != L'[' && opening != L'<') {
            continue;
        }
        const wchar_t closing = opening == L'[' ? L']' : L'>';
        const std::size_t end = line.find(closing, position + 1U);
        if (end == std::wstring_view::npos) {
            continue;
        }
        LyricTime time{};
        if (parse_timestamp(line.substr(position + 1U, end - position - 1U), time)) {
            result.push_back({position, end + 1U, time,
                opening == L'[' ? TimestampKind::line : TimestampKind::word});
            position = end;
        }
    }
    return result;
}

[[nodiscard]] bool has_timestamp(const std::wstring_view value)
{
    return !markers(value).empty();
}

[[nodiscard]] bool metadata_line(const std::wstring_view line)
{
    if (line.size() < 4U || line.front() != L'[' || line.back() != L']' ||
        std::iswalpha(line[1]) == 0) {
        return false;
    }
    const std::size_t colon = line.find(L':');
    return colon != std::wstring_view::npos &&
           line.find(L']', 1U) == line.size() - 1U;
}

[[nodiscard]] bool contains_kana(const std::wstring_view text) noexcept
{
    return std::ranges::any_of(text, [](const wchar_t value) {
        return (value >= 0x3040 && value <= 0x30ff);
    });
}

[[nodiscard]] bool contains_han(const std::wstring_view text) noexcept
{
    return std::ranges::any_of(text, [](const wchar_t value) {
        return (value >= 0x3400 && value <= 0x4dbf) ||
               (value >= 0x4e00 && value <= 0x9fff) ||
               (value >= 0xf900 && value <= 0xfaff);
    });
}

[[nodiscard]] bool credit_line(const std::wstring_view value)
{
    const std::wstring line = trim(value);
    if (line.empty()) {
        return false;
    }
    const std::wstring compact = [&] {
        std::wstring result;
        for (const wchar_t character : line) {
            if (!whitespace(character)) {
                result.push_back(static_cast<wchar_t>(std::towlower(character)));
            }
        }
        return result;
    }();
    const std::wstring_view prefixes[]{
        L"作词:", L"作词：", L"作曲:", L"作曲：", L"编曲:", L"编曲：",
        L"词:", L"词：", L"曲:", L"曲：", L"制作人:", L"制作人：",
        L"翻译:", L"翻译：", L"歌词:", L"歌词：", L"lyricsby",
    };
    if (std::ranges::any_of(prefixes, [&](const std::wstring_view prefix) {
            return compact.starts_with(prefix);
        })) {
        return true;
    }
    return compact.find(L"qq音乐享有") != std::wstring::npos ||
           compact.find(L"著作权") != std::wstring::npos ||
           compact.find(L"未经许可") != std::wstring::npos;
}

[[nodiscard]] std::vector<LyricToken> timed_tokens(
    const std::wstring_view line,
    const std::vector<TimestampMarker>& line_markers)
{
    std::vector<LyricToken> result;
    if (line_markers.front().begin != 0U) {
        result.push_back({
            std::wstring(line.substr(0, line_markers.front().begin)),
            line_markers.front().time,
            line_markers.front().time,
        });
    }
    for (std::size_t index{}; index < line_markers.size(); ++index) {
        const auto& marker = line_markers[index];
        const std::size_t text_end = index + 1U < line_markers.size()
            ? line_markers[index + 1U].begin
            : line.size();
        if (text_end <= marker.end) {
            continue;
        }
        std::wstring text(line.substr(marker.end, text_end - marker.end));
        if (text.empty()) {
            continue;
        }
        result.push_back({
            std::move(text),
            marker.time,
            index + 1U < line_markers.size()
                ? std::optional<LyricTime>(line_markers[index + 1U].time)
                : std::nullopt,
        });
    }
    while (!result.empty()) {
        result.front().text = trim(result.front().text);
        if (!result.front().text.empty()) {
            break;
        }
        result.erase(result.begin());
    }
    while (!result.empty()) {
        result.back().text = trim(result.back().text);
        if (!result.back().text.empty()) {
            break;
        }
        result.pop_back();
    }
    return result;
}

[[nodiscard]] bool displayable_tokens(const std::vector<LyricToken>& tokens) noexcept
{
    return tokens.size() > 1U && std::ranges::any_of(tokens, [](const LyricToken& token) {
        return token.end_milliseconds &&
               *token.end_milliseconds - token.start_milliseconds > 20;
    });
}

[[nodiscard]] std::optional<TimedEntry> parse_line(
    const std::wstring_view raw,
    const std::size_t order)
{
    const std::wstring line = normalized_text(raw);
    if (line.empty() || metadata_line(line)) {
        return std::nullopt;
    }
    const auto line_markers = markers(line);
    if (line_markers.empty()) {
        return std::nullopt;
    }

    // Two leading timestamps followed by plain text encode a line range in
    // several enhanced-LRC exporters. More timestamps in the remainder mean
    // that the same square-bracket syntax is being used for word cues.
    if (line_markers.size() == 2U && line_markers[0].begin == 0U &&
        line_markers[0].end == line_markers[1].begin) {
        const std::wstring text = trim(line.substr(line_markers[1].end));
        if (!text.empty() && !has_timestamp(text) && !credit_line(text)) {
            return TimedEntry{
                line_markers[1].time,
                false,
                false,
                order,
                text,
                line_markers[0].time,
                {},
            };
        }
    }

    auto tokens = timed_tokens(line, line_markers);
    std::wstring text;
    for (const auto& token : tokens) {
        text += token.text;
    }
    text = trim(text);
    if (text.empty() || credit_line(text)) {
        return std::nullopt;
    }
    const bool boundary = line_markers.size() >= 2U &&
        std::ranges::all_of(line_markers, [](const TimestampMarker& marker) {
            return marker.kind == TimestampKind::line;
        }) && line_markers.front().begin == 0U &&
        line_markers.back().end == line.size() &&
        std::llabs(line_markers.back().time - line_markers.front().time) <= 50;
    const bool trailing = line_markers.size() == 1U &&
        line_markers.front().begin != 0U && line_markers.front().end == line.size();
    const std::optional<LyricTime> end = tokens.empty()
        ? std::nullopt
        : tokens.back().end_milliseconds;
    const LyricTime start = tokens.empty()
        ? line_markers.front().time
        : tokens.front().start_milliseconds;
    if (!displayable_tokens(tokens)) {
        tokens.clear();
    }
    return TimedEntry{end, boundary, trailing, order, text, start, std::move(tokens)};
}

[[nodiscard]] bool bilingual_pair(
    const std::wstring_view first,
    const std::wstring_view second) noexcept
{
    return contains_kana(first) != contains_kana(second) ||
           contains_han(first) != contains_han(second);
}

[[nodiscard]] bool prefer_candidate(
    const TimedEntry& candidate,
    const TimedEntry& current) noexcept
{
    if (displayable_tokens(candidate.tokens) != displayable_tokens(current.tokens)) {
        return displayable_tokens(candidate.tokens);
    }
    return contains_kana(candidate.text) && !contains_kana(current.text);
}

[[nodiscard]] LyricLine make_line(
    const TimedEntry& first,
    const TimedEntry* second)
{
    const TimedEntry* primary = &first;
    const TimedEntry* translation = second;
    if (second != nullptr && prefer_candidate(*second, first)) {
        primary = second;
        translation = &first;
    }
    LyricLine result;
    result.start_milliseconds = primary->time;
    result.end_milliseconds = primary->end;
    result.text = primary->text;
    result.tokens = primary->tokens;
    if (translation != nullptr) {
        result.translation = translation->text;
        result.translation_tokens = translation->tokens;
        if (translation->end &&
            (!result.end_milliseconds || *translation->end > *result.end_milliseconds)) {
            result.end_milliseconds = translation->end;
        }
    }
    return result;
}

[[nodiscard]] std::wstring match_key(const std::wstring_view value)
{
    std::wstring result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (std::iswalnum(character) != 0) {
            result.push_back(static_cast<wchar_t>(std::towlower(character)));
        }
    }
    return result;
}

[[nodiscard]] double dice_similarity(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    const std::wstring first = match_key(left);
    const std::wstring second = match_key(right);
    if (first == second) {
        return first.empty() ? 0.0 : 1.0;
    }
    if (first.empty() || second.empty()) {
        return 0.0;
    }
    if (first.size() == 1U || second.size() == 1U) {
        return first.find(second) != std::wstring::npos ||
                       second.find(first) != std::wstring::npos
            ? 0.8
            : 0.0;
    }
    std::map<std::pair<wchar_t, wchar_t>, std::size_t> counts;
    for (std::size_t index{}; index + 1U < first.size(); ++index) {
        ++counts[{first[index], first[index + 1U]}];
    }
    std::size_t intersection{};
    for (std::size_t index{}; index + 1U < second.size(); ++index) {
        auto found = counts.find({second[index], second[index + 1U]});
        if (found != counts.end() && found->second != 0U) {
            --found->second;
            ++intersection;
        }
    }
    return 2.0 * static_cast<double>(intersection) /
           static_cast<double>(first.size() + second.size() - 2U);
}

[[nodiscard]] bool parse_unsigned(
    const std::wstring_view text,
    std::size_t& position,
    LyricTime& value) noexcept
{
    if (position >= text.size() || text[position] < L'0' || text[position] > L'9') {
        return false;
    }
    value = 0;
    while (position < text.size() && text[position] >= L'0' && text[position] <= L'9') {
        const int digit = text[position++] - L'0';
        if (value > (std::numeric_limits<LyricTime>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    return true;
}

[[nodiscard]] bool consume(const std::wstring_view text,
                           std::size_t& position,
                           const wchar_t expected) noexcept
{
    if (position >= text.size() || text[position] != expected) {
        return false;
    }
    ++position;
    return true;
}

[[nodiscard]] std::wstring xml_unescape(std::wstring value)
{
    const std::pair<std::wstring_view, std::wstring_view> entities[]{
        {L"&quot;", L"\""}, {L"&apos;", L"'"}, {L"&lt;", L"<"},
        {L"&gt;", L">"}, {L"&#13;", L"\r"}, {L"&#10;", L"\n"},
        {L"&amp;", L"&"},
    };
    for (const auto& [from, to] : entities) {
        for (std::size_t position{};
             (position = value.find(from, position)) != std::wstring::npos;) {
            value.replace(position, from.size(), to);
            position += to.size();
        }
    }
    return value;
}

[[nodiscard]] std::wstring qrc_content(const std::wstring_view raw)
{
    const std::wstring_view prefix = L"LyricContent=\"";
    const std::size_t begin = raw.find(prefix);
    if (begin == std::wstring_view::npos) {
        return std::wstring(raw);
    }
    const std::size_t content_begin = begin + prefix.size();
    const std::size_t content_end = raw.find(L"\"/>", content_begin);
    return xml_unescape(std::wstring(raw.substr(
        content_begin,
        content_end == std::wstring_view::npos
            ? raw.size() - content_begin
            : content_end - content_begin)));
}

[[nodiscard]] std::optional<LyricLine> parse_vendor_line(
    const std::wstring_view raw,
    const VendorLyricsFormat format)
{
    const std::wstring line = trim(raw);
    if (line.empty() || line.front() != L'[') {
        return std::nullopt;
    }
    std::size_t position = 1U;
    LyricTime line_start{};
    LyricTime line_duration{};
    if (!parse_unsigned(line, position, line_start) ||
        !consume(line, position, L',') ||
        !parse_unsigned(line, position, line_duration) ||
        !consume(line, position, L']')) {
        return std::nullopt;
    }
    if (line_start < 0 || line_duration < 0 ||
        line_start > std::numeric_limits<LyricTime>::max() - line_duration) {
        return std::nullopt;
    }

    LyricLine result;
    result.start_milliseconds = line_start;
    result.end_milliseconds = line_start + line_duration;
    while (position < line.size()) {
        const wchar_t opening = format == VendorLyricsFormat::krc ? L'<' : L'(';
        const wchar_t closing = format == VendorLyricsFormat::krc ? L'>' : L')';
        const std::size_t marker = line.find(opening, position);
        if (marker == std::wstring::npos) {
            break;
        }

        // QRC stores text before its (start,duration) marker; YRC/KRC store
        // text after the marker and before the following one.
        if (format == VendorLyricsFormat::qrc && marker > position) {
            const std::wstring word = std::wstring(line.substr(position, marker - position));
            std::size_t number_position = marker + 1U;
            LyricTime word_start{};
            LyricTime word_duration{};
            if (!parse_unsigned(line, number_position, word_start) ||
                !consume(line, number_position, L',') ||
                !parse_unsigned(line, number_position, word_duration)) {
                position = marker + 1U;
                continue;
            }
            const std::size_t close = line.find(closing, number_position);
            if (close == std::wstring::npos || word_start >
                    std::numeric_limits<LyricTime>::max() - word_duration) {
                break;
            }
            result.tokens.push_back({word, word_start, word_start + word_duration});
            result.text += word;
            position = close + 1U;
            continue;
        }

        std::size_t number_position = marker + 1U;
        LyricTime word_start{};
        LyricTime word_duration{};
        if (!parse_unsigned(line, number_position, word_start) ||
            !consume(line, number_position, L',') ||
            !parse_unsigned(line, number_position, word_duration)) {
            position = marker + 1U;
            continue;
        }
        const std::size_t close = line.find(closing, number_position);
        if (close == std::wstring::npos) {
            break;
        }
        const std::size_t next = line.find(opening, close + 1U);
        const std::size_t word_end = next == std::wstring::npos ? line.size() : next;
        std::wstring word = std::wstring(line.substr(close + 1U, word_end - close - 1U));
        if (word.empty()) {
            position = word_end;
            continue;
        }
        if (format == VendorLyricsFormat::krc) {
            if (line_start > std::numeric_limits<LyricTime>::max() - word_start) {
                break;
            }
            word_start += line_start;
        }
        if (word_start > std::numeric_limits<LyricTime>::max() - word_duration) {
            break;
        }
        result.tokens.push_back({word, word_start, word_start + word_duration});
        result.text += word;
        position = word_end;
    }

    result.text = trim(result.text);
    if (result.text.empty() || credit_line(result.text)) {
        return std::nullopt;
    }
    if (!displayable_tokens(result.tokens)) {
        result.tokens.clear();
    }
    return result;
}

[[nodiscard]] LyricsDocument parse_vendor(
    const std::wstring_view raw,
    const VendorLyricsFormat format)
{
    const std::wstring storage = format == VendorLyricsFormat::qrc
        ? qrc_content(raw)
        : std::wstring(raw);
    LyricsDocument document;
    std::size_t position{};
    while (position <= storage.size()) {
        const std::size_t newline = storage.find_first_of(L"\r\n", position);
        const std::size_t end = newline == std::wstring::npos ? storage.size() : newline;
        if (auto line = parse_vendor_line(
                std::wstring_view(storage).substr(position, end - position), format)) {
            document.has_word_timing = document.has_word_timing ||
                displayable_tokens(line->tokens);
            document.lines.push_back(std::move(*line));
        }
        if (newline == std::wstring::npos) {
            break;
        }
        position = newline + 1U;
        if (storage[newline] == L'\r' && position < storage.size() &&
            storage[position] == L'\n') {
            ++position;
        }
    }
    std::ranges::stable_sort(document.lines, {}, &LyricLine::start_milliseconds);
    return document;
}

} // namespace

LyricsDocument parse_lrc(const std::wstring_view raw)
{
    std::vector<TimedEntry> entries;
    std::size_t position{};
    std::size_t order{};
    while (position <= raw.size()) {
        const std::size_t newline = raw.find_first_of(L"\r\n", position);
        const std::size_t end = newline == std::wstring_view::npos ? raw.size() : newline;
        if (auto parsed = parse_line(raw.substr(position, end - position), order++)) {
            entries.push_back(std::move(*parsed));
        }
        if (newline == std::wstring_view::npos) {
            break;
        }
        position = newline + 1U;
        if (raw[newline] == L'\r' && position < raw.size() && raw[position] == L'\n') {
            ++position;
        }
    }

    LyricsDocument document;
    for (std::size_t index{}; index < entries.size();) {
        const TimedEntry& current = entries[index];
        const TimedEntry* translation{};
        if (index + 1U < entries.size()) {
            const TimedEntry& candidate = entries[index + 1U];
            const bool same_start =
                std::llabs(candidate.time - current.time) <= 1;
            const LyricTime primary_end = current.end.value_or(current.time);
            const bool boundary_pair =
                (candidate.boundary || candidate.trailing_timestamp) &&
                !current.trailing_timestamp &&
                (bilingual_pair(current.text, candidate.text) ||
                 displayable_tokens(current.tokens)) &&
                candidate.time >= current.time &&
                candidate.time <= primary_end +
                    (candidate.boundary ? 4'000 : 2'500);
            if (same_start || boundary_pair) {
                translation = &candidate;
                ++index;
            }
        }
        document.lines.push_back(make_line(current, translation));
        ++index;
    }
    std::ranges::stable_sort(document.lines, {}, &LyricLine::start_milliseconds);
    for (std::size_t index{}; index < document.lines.size(); ++index) {
        auto& line = document.lines[index];
        const std::optional<LyricTime> next = index + 1U < document.lines.size()
            ? std::optional<LyricTime>(document.lines[index + 1U].start_milliseconds)
            : std::nullopt;
        if (!line.end_milliseconds || *line.end_milliseconds <= line.start_milliseconds) {
            line.end_milliseconds = next;
        } else if (next && *line.end_milliseconds > *next + 4'000) {
            line.end_milliseconds = next;
        }
        if (!line.tokens.empty()) {
            auto& last = line.tokens.back();
            if (!last.end_milliseconds) {
                last.end_milliseconds = line.end_milliseconds;
            }
        }
        document.has_word_timing = document.has_word_timing ||
            displayable_tokens(line.tokens);
    }
    return document;
}

LyricsDocument parse_yrc(const std::wstring_view raw)
{
    return parse_vendor(raw, VendorLyricsFormat::yrc);
}

LyricsDocument parse_qrc(const std::wstring_view raw)
{
    return parse_vendor(raw, VendorLyricsFormat::qrc);
}

LyricsDocument parse_krc(const std::wstring_view raw)
{
    return parse_vendor(raw, VendorLyricsFormat::krc);
}

std::optional<std::size_t> active_lyric_line(
    const LyricsDocument& lyrics,
    const LyricTime position_milliseconds) noexcept
{
    const auto found = std::ranges::upper_bound(
        lyrics.lines, position_milliseconds, {}, &LyricLine::start_milliseconds);
    if (found == lyrics.lines.begin()) {
        return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(
        std::distance(lyrics.lines.begin(), found) - 1);
    const auto& line = lyrics.lines[index];
    if (index + 1U == lyrics.lines.size() && line.end_milliseconds &&
        position_milliseconds >= *line.end_milliseconds) {
        return std::nullopt;
    }
    return index;
}

double lyric_token_progress(
    const LyricToken& token,
    const LyricTime position_milliseconds,
    const std::optional<LyricTime> fallback_end_milliseconds) noexcept
{
    const LyricTime end = token.end_milliseconds.value_or(
        fallback_end_milliseconds.value_or(token.start_milliseconds));
    if (position_milliseconds <= token.start_milliseconds) {
        return 0.0;
    }
    if (end <= token.start_milliseconds || position_milliseconds >= end) {
        return 1.0;
    }
    return std::clamp(
        static_cast<double>(position_milliseconds - token.start_milliseconds) /
            static_cast<double>(end - token.start_milliseconds),
        0.0,
        1.0);
}

double lyrics_match_score(
    const LyricsMatchQuery& query,
    const LyricsMatchCandidate& candidate) noexcept
{
    if (!candidate.has_synced_lyrics || query.title.empty() || candidate.title.empty()) {
        return 0.0;
    }
    if (query.duration_milliseconds > 0 && candidate.duration_milliseconds > 0 &&
        std::llabs(query.duration_milliseconds - candidate.duration_milliseconds) > 4'000) {
        return 0.0;
    }
    const double title = dice_similarity(query.title, candidate.title) * 100.0;
    const double artist = query.artist.empty() || candidate.artist.empty()
        ? -1.0
        : dice_similarity(query.artist, candidate.artist) * 100.0;
    const double album = query.album.empty() || candidate.album.empty()
        ? -1.0
        : dice_similarity(query.album, candidate.album) * 100.0;
    double score = title;
    if (artist >= 0.0) {
        score = album >= 0.0
            ? std::max(title * 0.5 + artist * 0.5,
                  title * 0.5 + artist * 0.35 + album * 0.15)
            : title * 0.5 + artist * 0.5;
    } else if (album >= 0.0) {
        score = std::max(title * 0.7 + album * 0.3, title * 0.8);
    }
    if (title < 30.0) {
        score = std::max(0.0, score - 35.0);
    }
    if (candidate.has_word_timing) {
        score += 2.0;
    }
    return std::clamp(score, 0.0, 100.0);
}

} // namespace cd404::core
