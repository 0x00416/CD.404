#include <windows.h>

#include <shlwapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <xmllite.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>

#include <cd404/platform/windows/musicbrainz_client.hpp>
#include <cd404/disc/musicbrainz_disc_id.hpp>

#include "http_client.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <cwchar>
#include <chrono>
#include <format>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cd404::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint64_t kMaximumAverageTrackDifferenceMilliseconds = 2'000U;
constexpr std::uint64_t kMaximumSingleTrackDifferenceMilliseconds = 5'000U;

void wait_for_musicbrainz_request_slot()
{
    using namespace std::chrono_literals;
    static std::mutex mutex;
    static std::chrono::steady_clock::time_point previous;
    const std::scoped_lock lock(mutex);
    const auto earliest = previous + 1'100ms;
    if (previous != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() < earliest) {
        std::this_thread::sleep_until(earliest);
    }
    previous = std::chrono::steady_clock::now();
}

class InternetHandle final {
public:
    explicit InternetHandle(HINTERNET handle = nullptr) noexcept : handle_(handle) {}
    ~InternetHandle()
    {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }

private:
    HINTERNET handle_{};
};

[[nodiscard]] std::wstring make_fuzzy_lookup_path(const disc::Toc& toc)
{
    const auto tracks = toc.tracks();
    if (tracks.empty()) {
        return {};
    }

    constexpr core::Sector kLeadInSectors = 150;
    std::wstring toc_value = std::format(
        L"{}+{}+{}",
        tracks.front().number,
        tracks.back().number,
        toc.lead_out_lba() + kLeadInSectors);
    for (const auto& track : tracks) {
        toc_value += std::format(L"+{}", track.start_lba + kLeadInSectors);
    }
    return L"/ws/2/discid/-?toc=" + toc_value +
           L"&inc=recordings+artist-credits+release-groups&cdstubs=no";
}

[[nodiscard]] bool read_response(
    HINTERNET request,
    std::vector<std::uint8_t>& body,
    const std::size_t maximum_response_bytes = 2U * 1'024U * 1'024U)
{
    for (;;) {
        DWORD available{};
        if (WinHttpQueryDataAvailable(request, &available) == FALSE) {
            return false;
        }
        if (available == 0) {
            return true;
        }
        if (available > maximum_response_bytes - body.size()) {
            SetLastError(ERROR_FILE_TOO_LARGE);
            return false;
        }

        const std::size_t offset = body.size();
        body.resize(offset + available);
        DWORD bytes_read{};
        if (WinHttpReadData(
                request,
                body.data() + offset,
                available,
                &bytes_read) == FALSE) {
            return false;
        }
        body.resize(offset + bytes_read);
    }
}

[[nodiscard]] std::filesystem::path cover_cache_path(const std::wstring& release_id)
{
    wchar_t* local_app_data{};
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData,
            KF_FLAG_CREATE,
            nullptr,
            &local_app_data)) ||
        local_app_data == nullptr) {
        return {};
    }
    const std::filesystem::path base(local_app_data);
    CoTaskMemFree(local_app_data);
    return base / L"CD.404" / L"Cache" / L"covers" /
           (release_id + L"-1200.jpg");
}

[[nodiscard]] bool is_musicbrainz_identifier(const std::wstring_view value)
{
    if (value.size() != 36U) {
        return false;
    }
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != L'-') {
                return false;
            }
        } else if (std::iswxdigit(value[index]) == 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool looks_like_image(const std::span<const std::uint8_t> bytes)
{
    constexpr std::uint8_t kPngSignature[]{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    };
    const bool jpeg = bytes.size() >= 3 && bytes[0] == 0xff &&
                      bytes[1] == 0xd8 && bytes[2] == 0xff;
    const bool png = bytes.size() >= std::size(kPngSignature) &&
                     std::equal(
                         std::begin(kPngSignature),
                         std::end(kPngSignature),
                         bytes.begin());
    return jpeg || png;
}

[[nodiscard]] bool cached_cover_is_image(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::array<std::uint8_t, 8> signature{};
    input.read(
        reinterpret_cast<char*>(signature.data()),
        static_cast<std::streamsize>(signature.size()));
    return looks_like_image(std::span(
        signature.data(),
        static_cast<std::size_t>(std::max<std::streamsize>(input.gcount(), 0))));
}

[[nodiscard]] std::filesystem::path download_cover_art(
    const std::wstring& release_id,
    const std::wstring& release_group_id)
{
    if (!is_musicbrainz_identifier(release_id) ||
        (!release_group_id.empty() &&
         !is_musicbrainz_identifier(release_group_id))) {
        return {};
    }
    const std::filesystem::path cache_path = cover_cache_path(release_id);
    std::error_code filesystem_error;
    if (!cache_path.empty() && std::filesystem::file_size(cache_path, filesystem_error) > 0 &&
        !filesystem_error && cached_cover_is_image(cache_path)) {
        return cache_path;
    }
    if (cache_path.empty()) {
        return {};
    }

    InternetHandle session(WinHttpOpen(
        L"CD.404/0.2.0 (https://github.com/0x00416/CD.404)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (session.get() == nullptr) {
        return {};
    }
    static_cast<void>(WinHttpSetTimeouts(session.get(), 3'000, 5'000, 5'000, 10'000));
    InternetHandle connection(WinHttpConnect(
        session.get(),
        L"coverartarchive.org",
        INTERNET_DEFAULT_HTTPS_PORT,
        0));
    if (connection.get() == nullptr) {
        return {};
    }
    std::vector<std::uint8_t> image;
    const std::array paths{
        L"/release/" + release_id + L"/front-1200",
        release_group_id.empty()
            ? std::wstring{}
            : L"/release-group/" + release_group_id + L"/front-1200",
    };
    for (const auto& path : paths) {
        if (path.empty()) {
            continue;
        }
        InternetHandle request(WinHttpOpenRequest(
            connection.get(),
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE));
        if (request.get() == nullptr ||
            WinHttpSendRequest(
                request.get(),
                WINHTTP_NO_ADDITIONAL_HEADERS,
                0,
                WINHTTP_NO_REQUEST_DATA,
                0,
                0,
                0) == FALSE ||
            WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
            return {};
        }

        DWORD status{};
        DWORD status_size = sizeof(status);
        if (WinHttpQueryHeaders(
                request.get(),
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &status_size,
                WINHTTP_NO_HEADER_INDEX) == FALSE) {
            return {};
        }
        if (status == 404) {
            continue;
        }
        if (status != 200) {
            return {};
        }

        image.clear();
        constexpr std::size_t kMaximumCoverBytes = 12U * 1'024U * 1'024U;
        if (read_response(request.get(), image, kMaximumCoverBytes) &&
            looks_like_image(image)) {
            break;
        }
        image.clear();
    }
    if (image.empty()) {
        return {};
    }

    std::filesystem::create_directories(cache_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return {};
    }
    const std::filesystem::path temporary_path = cache_path.wstring() + L".tmp";
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output.write(
                reinterpret_cast<const char*>(image.data()),
                static_cast<std::streamsize>(image.size()))) {
            return {};
        }
    }
    std::filesystem::rename(temporary_path, cache_path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary_path, filesystem_error);
        return {};
    }
    return cache_path;
}

[[nodiscard]] std::optional<unsigned long> unsigned_attribute(
    IXmlReader& reader,
    const wchar_t* name)
{
    const wchar_t* value{};
    if (reader.MoveToAttributeByName(name, nullptr) != S_OK ||
        FAILED(reader.GetValue(&value, nullptr)) || value == nullptr) {
        return std::nullopt;
    }
    wchar_t* end{};
    const unsigned long parsed = std::wcstoul(value, &end, 10);
    return end != value && *end == L'\0' ? std::optional(parsed) : std::nullopt;
}

[[nodiscard]] std::wstring string_attribute(IXmlReader& reader, const wchar_t* name)
{
    const wchar_t* value{};
    if (reader.MoveToAttributeByName(name, nullptr) != S_OK ||
        FAILED(reader.GetValue(&value, nullptr)) || value == nullptr) {
        return {};
    }
    return value;
}

struct XmlState final {
    MusicBrainzMetadata candidate;
    std::vector<std::pair<std::uint64_t, MusicBrainzMetadata>> matches;
    std::vector<std::wstring> element_stack;
    std::vector<std::uint64_t> track_lengths;
    std::wstring text;
    std::wstring pending_join_phrase;
    std::wstring current_track_artist;
    std::wstring current_track_id;
    std::wstring current_recording_id;
    std::vector<std::wstring> current_track_artist_ids;
    std::wstring track_pending_join_phrase;
    std::optional<std::uint64_t> current_track_length;
    std::size_t release_depth{std::numeric_limits<std::size_t>::max()};
    std::size_t medium_depth{std::numeric_limits<std::size_t>::max()};
    std::size_t track_list_depth{std::numeric_limits<std::size_t>::max()};
    std::size_t track_depth{std::numeric_limits<std::size_t>::max()};
    std::size_t release_artist_depth{std::numeric_limits<std::size_t>::max()};
    std::size_t recording_depth{std::numeric_limits<std::size_t>::max()};
    std::size_t track_artist_depth{std::numeric_limits<std::size_t>::max()};
    bool medium_matches{};
};

void begin_element(
    XmlState& state,
    IXmlReader& reader,
    const std::wstring_view name,
    const std::size_t expected_tracks)
{
    state.element_stack.emplace_back(name);
    const std::size_t depth = state.element_stack.size();
    state.text.clear();

    if (name == L"release") {
        state.candidate = {};
        state.candidate.release_id = string_attribute(reader, L"id");
        state.release_depth = depth;
    } else if (name == L"release-group" && depth == state.release_depth + 1U) {
        state.candidate.release_group_id = string_attribute(reader, L"id");
    } else if (name == L"artist-credit" && depth == state.release_depth + 1U) {
        state.release_artist_depth = depth;
    } else if (name == L"name-credit" && state.release_artist_depth !=
               std::numeric_limits<std::size_t>::max()) {
        state.pending_join_phrase = string_attribute(reader, L"joinphrase");
    } else if (name == L"medium" && state.release_depth !=
               std::numeric_limits<std::size_t>::max()) {
        state.medium_depth = depth;
        state.medium_matches = false;
        state.candidate.track_titles.clear();
        state.candidate.track_artists.clear();
        state.candidate.track_ids.clear();
        state.candidate.recording_ids.clear();
        state.candidate.track_artist_ids.clear();
        state.track_lengths.clear();
    } else if (name == L"track-list" && depth == state.medium_depth + 1U) {
        state.track_list_depth = depth;
        const auto count = unsigned_attribute(reader, L"count");
        state.medium_matches = count && *count == expected_tracks;
    } else if (name == L"track" && state.medium_matches &&
               state.track_list_depth != std::numeric_limits<std::size_t>::max()) {
        state.track_depth = depth;
        state.current_track_length.reset();
        state.current_track_artist.clear();
        state.current_track_id = string_attribute(reader, L"id");
        state.current_recording_id.clear();
        state.current_track_artist_ids.clear();
    } else if (name == L"recording" && state.medium_matches &&
               state.track_list_depth != std::numeric_limits<std::size_t>::max()) {
        state.recording_depth = depth;
        state.current_recording_id = string_attribute(reader, L"id");
    } else if (name == L"artist-credit" && state.recording_depth !=
               std::numeric_limits<std::size_t>::max()) {
        state.track_artist_depth = depth;
    } else if (name == L"name-credit" && state.track_artist_depth !=
               std::numeric_limits<std::size_t>::max()) {
        state.track_pending_join_phrase = string_attribute(reader, L"joinphrase");
    } else if (name == L"artist" && state.track_artist_depth !=
               std::numeric_limits<std::size_t>::max()) {
        const std::wstring artist_id = string_attribute(reader, L"id");
        if (!artist_id.empty() &&
            std::ranges::find(state.current_track_artist_ids, artist_id) ==
                state.current_track_artist_ids.end()) {
            state.current_track_artist_ids.push_back(artist_id);
        }
    }
}

void end_element(
    XmlState& state,
    const std::wstring_view name,
    const std::span<const std::uint64_t> expected_lengths)
{
    const std::size_t expected_tracks = expected_lengths.size();
    const std::size_t depth = state.element_stack.size();
    if (name == L"name" && state.track_artist_depth !=
            std::numeric_limits<std::size_t>::max()) {
        state.current_track_artist += state.text;
        state.current_track_artist += state.track_pending_join_phrase;
        state.track_pending_join_phrase.clear();
    } else if (name == L"title" && depth == state.release_depth + 1U &&
        state.candidate.album_title.empty()) {
        state.candidate.album_title = state.text;
    } else if (name == L"name" && state.release_artist_depth !=
               std::numeric_limits<std::size_t>::max() &&
               state.recording_depth == std::numeric_limits<std::size_t>::max()) {
        state.candidate.album_artist += state.text;
        state.candidate.album_artist += state.pending_join_phrase;
        state.pending_join_phrase.clear();
    } else if (name == L"title" && state.recording_depth !=
               std::numeric_limits<std::size_t>::max()) {
        state.candidate.track_titles.push_back(state.text);
    } else if (name == L"length" && depth == state.track_depth + 1U) {
        wchar_t* end{};
        const unsigned long long milliseconds = std::wcstoull(state.text.c_str(), &end, 10);
        if (end != state.text.c_str() && *end == L'\0') {
            state.current_track_length = milliseconds;
        }
    } else if (name == L"recording" && depth == state.recording_depth) {
        state.recording_depth = std::numeric_limits<std::size_t>::max();
    } else if (name == L"track" && depth == state.track_depth) {
        state.track_lengths.push_back(state.current_track_length.value_or(0));
        state.candidate.track_artists.push_back(state.current_track_artist);
        state.candidate.track_ids.push_back(state.current_track_id);
        state.candidate.recording_ids.push_back(state.current_recording_id);
        state.candidate.track_artist_ids.push_back(state.current_track_artist_ids);
        state.track_depth = std::numeric_limits<std::size_t>::max();
        state.current_track_length.reset();
        state.current_track_artist.clear();
        state.current_track_id.clear();
        state.current_recording_id.clear();
        state.current_track_artist_ids.clear();
    } else if (name == L"track-list" && depth == state.track_list_depth) {
        state.track_list_depth = std::numeric_limits<std::size_t>::max();
    } else if (name == L"medium" && depth == state.medium_depth) {
        if (state.medium_matches &&
            state.candidate.track_titles.size() == expected_tracks &&
            !state.candidate.album_title.empty() &&
            state.track_lengths.size() == expected_tracks &&
            std::ranges::none_of(state.track_lengths, [](const std::uint64_t value) {
                return value == 0;
            })) {
            std::uint64_t score{};
            std::uint64_t maximum_difference{};
            for (std::size_t index = 0; index < expected_tracks; ++index) {
                const auto actual = state.track_lengths[index];
                const auto expected = expected_lengths[index];
                const auto difference = actual > expected
                    ? actual - expected
                    : expected - actual;
                score += difference;
                maximum_difference = std::max(maximum_difference, difference);
            }
            const bool durations_match =
                maximum_difference <= kMaximumSingleTrackDifferenceMilliseconds &&
                score <= kMaximumAverageTrackDifferenceMilliseconds * expected_tracks;
            if (durations_match) {
                state.candidate.track_lengths_milliseconds = state.track_lengths;
                state.matches.emplace_back(score, state.candidate);
            }
        }
        state.medium_depth = std::numeric_limits<std::size_t>::max();
        state.medium_matches = false;
        state.candidate.track_titles.clear();
        state.candidate.track_artists.clear();
        state.candidate.track_ids.clear();
        state.candidate.recording_ids.clear();
        state.candidate.track_artist_ids.clear();
        state.candidate.track_lengths_milliseconds.clear();
        state.track_lengths.clear();
    } else if (name == L"artist-credit" && depth == state.track_artist_depth) {
        state.track_artist_depth = std::numeric_limits<std::size_t>::max();
    } else if (name == L"artist-credit" && depth == state.release_artist_depth) {
        state.release_artist_depth = std::numeric_limits<std::size_t>::max();
    } else if (name == L"release" && depth == state.release_depth) {
        state.release_depth = std::numeric_limits<std::size_t>::max();
    }
    if (!state.element_stack.empty()) {
        state.element_stack.pop_back();
    }
    state.text.clear();
}

[[nodiscard]] std::vector<MusicBrainzMetadata> parse_response(
    const std::span<const std::uint8_t> body,
    const std::span<const std::uint64_t> expected_lengths)
{
    if (body.empty() || body.size() > std::numeric_limits<UINT>::max()) {
        return {};
    }

    ComPtr<IStream> stream(SHCreateMemStream(body.data(), static_cast<UINT>(body.size())));
    if (!stream) {
        return {};
    }
    ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(
            __uuidof(IXmlReader),
            reinterpret_cast<void**>(reader.ReleaseAndGetAddressOf()),
            nullptr)) ||
        FAILED(reader->SetInput(stream.Get()))) {
        return {};
    }
    static_cast<void>(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit));

    XmlState state;
    XmlNodeType node_type{};
    while (reader->Read(&node_type) == S_OK) {
        if (node_type == XmlNodeType_Element) {
            const wchar_t* local_name{};
            if (FAILED(reader->GetLocalName(&local_name, nullptr)) || local_name == nullptr) {
                return {};
            }
            begin_element(state, *reader.Get(), local_name, expected_lengths.size());
            if (reader->IsEmptyElement() != FALSE) {
                end_element(state, local_name, expected_lengths);
            }
        } else if (node_type == XmlNodeType_Text ||
                   node_type == XmlNodeType_CDATA ||
                   node_type == XmlNodeType_Whitespace) {
            const wchar_t* value{};
            if (SUCCEEDED(reader->GetValue(&value, nullptr)) && value != nullptr) {
                state.text += value;
            }
        } else if (node_type == XmlNodeType_EndElement) {
            const wchar_t* local_name{};
            if (FAILED(reader->GetLocalName(&local_name, nullptr)) || local_name == nullptr) {
                return {};
            }
            end_element(state, local_name, expected_lengths);
        }
    }
    std::ranges::sort(state.matches, {}, &decltype(state.matches)::value_type::first);
    std::vector<MusicBrainzMetadata> candidates;
    for (auto& [score, candidate] : state.matches) {
        static_cast<void>(score);
        if (candidate.release_id.empty() ||
            std::ranges::find(
                candidates,
                candidate.release_id,
                &MusicBrainzMetadata::release_id) != candidates.end()) {
            continue;
        }
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

[[nodiscard]] std::wstring normalized_content_name(const std::wstring_view value)
{
    std::wstring_view title = value;
    const std::size_t qualifier = title.find_last_of(L"(（[");
    if (qualifier != std::wstring_view::npos) {
        std::wstring suffix;
        for (const wchar_t character : title.substr(qualifier)) {
            if (std::iswalnum(character) != 0) {
                suffix.push_back(static_cast<wchar_t>(std::towlower(character)));
            }
        }
        if (suffix.find(L"remix") != std::wstring::npos) {
            title = title.substr(0U, qualifier);
        }
    }
    std::wstring normalized;
    normalized.reserve(title.size());
    for (const wchar_t character : title) {
        if (std::iswalnum(character) != 0) {
            normalized.push_back(static_cast<wchar_t>(std::towlower(character)));
        }
    }
    constexpr std::wstring_view remix = L"remix";
    if (normalized.size() > remix.size() && normalized.ends_with(remix)) {
        normalized.resize(normalized.size() - remix.size());
    }
    return normalized;
}

[[nodiscard]] std::uint32_t name_similarity(
    const std::wstring_view left,
    const std::wstring_view right)
{
    const std::wstring a = normalized_content_name(left);
    const std::wstring b = normalized_content_name(right);
    if (a.empty() || b.empty()) {
        return 0U;
    }
    std::vector<std::size_t> previous(b.size() + 1U);
    std::vector<std::size_t> current(b.size() + 1U);
    std::iota(previous.begin(), previous.end(), 0U);
    for (std::size_t row = 1U; row <= a.size(); ++row) {
        current[0] = row;
        for (std::size_t column = 1U; column <= b.size(); ++column) {
            current[column] = std::min({
                previous[column] + 1U,
                current[column - 1U] + 1U,
                previous[column - 1U] +
                    (a[row - 1U] == b[column - 1U] ? 0U : 1U),
            });
        }
        std::swap(previous, current);
    }
    const std::size_t maximum = std::max(a.size(), b.size());
    return static_cast<std::uint32_t>(
        (maximum - std::min(maximum, previous.back())) * 1'000U / maximum);
}

[[nodiscard]] std::vector<std::size_t> minimum_cost_assignment(
    const std::vector<std::vector<std::uint64_t>>& costs)
{
    const std::size_t count = costs.size();
    if (count == 0U || std::ranges::any_of(costs, [count](const auto& row) {
            return row.size() != count;
        })) {
        return {};
    }
    std::vector<std::int64_t> u(count + 1U);
    std::vector<std::int64_t> v(count + 1U);
    std::vector<std::size_t> p(count + 1U);
    std::vector<std::size_t> way(count + 1U);
    for (std::size_t row = 1U; row <= count; ++row) {
        p[0] = row;
        std::size_t column0{};
        std::vector<std::int64_t> minimum(
            count + 1U,
            std::numeric_limits<std::int64_t>::max());
        std::vector<bool> used(count + 1U);
        do {
            used[column0] = true;
            const std::size_t row0 = p[column0];
            std::int64_t delta = std::numeric_limits<std::int64_t>::max();
            std::size_t column1{};
            for (std::size_t column = 1U; column <= count; ++column) {
                if (used[column]) {
                    continue;
                }
                const auto current = static_cast<std::int64_t>(
                    costs[row0 - 1U][column - 1U]) - u[row0] - v[column];
                if (current < minimum[column]) {
                    minimum[column] = current;
                    way[column] = column0;
                }
                if (minimum[column] < delta) {
                    delta = minimum[column];
                    column1 = column;
                }
            }
            for (std::size_t column = 0U; column <= count; ++column) {
                if (used[column]) {
                    u[p[column]] += delta;
                    v[column] -= delta;
                } else {
                    minimum[column] -= delta;
                }
            }
            column0 = column1;
        } while (p[column0] != 0U);
        do {
            const std::size_t column1 = way[column0];
            p[column0] = p[column1];
            column0 = column1;
        } while (column0 != 0U);
    }
    std::vector<std::size_t> assignment(count);
    for (std::size_t column = 1U; column <= count; ++column) {
        assignment[p[column] - 1U] = column - 1U;
    }
    return assignment;
}

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;

[[nodiscard]] std::wstring json_string(
    const JsonObject& object,
    const wchar_t* key)
{
    return object.HasKey(key)
        ? std::wstring(object.GetNamedString(key, L""))
        : std::wstring{};
}

[[nodiscard]] std::uint64_t json_unsigned(
    const JsonObject& object,
    const wchar_t* key)
{
    if (!object.HasKey(key)) {
        return 0U;
    }
    const double value = object.GetNamedNumber(key, 0.0);
    return value > 0.0 && value <=
            static_cast<double>(std::numeric_limits<std::uint64_t>::max())
        ? static_cast<std::uint64_t>(value)
        : 0U;
}

[[nodiscard]] std::wstring artist_credit_name(const JsonArray& credit)
{
    std::wstring result;
    for (std::uint32_t index = 0U; index < credit.Size(); ++index) {
        const JsonObject item = credit.GetObjectAt(index);
        std::wstring name = json_string(item, L"name");
        if (name.empty() && item.HasKey(L"artist")) {
            name = json_string(item.GetNamedObject(L"artist"), L"name");
        }
        result += name;
        result += json_string(item, L"joinphrase");
    }
    return result;
}

[[nodiscard]] std::vector<std::wstring> artist_credit_ids(const JsonArray& credit)
{
    std::vector<std::wstring> result;
    for (std::uint32_t index = 0U; index < credit.Size(); ++index) {
        const JsonObject item = credit.GetObjectAt(index);
        if (!item.HasKey(L"artist")) {
            continue;
        }
        const std::wstring id = json_string(item.GetNamedObject(L"artist"), L"id");
        if (!id.empty() && std::ranges::find(result, id) == result.end()) {
            result.push_back(id);
        }
    }
    return result;
}

[[nodiscard]] std::vector<MusicBrainzMetadata> parse_release_detail_json_impl(
    const std::span<const std::uint8_t> body,
    const std::size_t expected_tracks)
{
    const std::string utf8(
        reinterpret_cast<const char*>(body.data()),
        body.size());
    try {
        const JsonObject release = JsonObject::Parse(detail::utf8_to_wide(utf8));
        MusicBrainzMetadata base;
        base.release_id = json_string(release, L"id");
        base.album_title = json_string(release, L"title");
        if (release.HasKey(L"release-group")) {
            base.release_group_id = json_string(
                release.GetNamedObject(L"release-group"), L"id");
        }
        if (release.HasKey(L"artist-credit")) {
            base.album_artist = artist_credit_name(
                release.GetNamedArray(L"artist-credit"));
        }
        if (base.release_id.empty() || base.album_title.empty() ||
            !release.HasKey(L"media")) {
            return {};
        }
        std::vector<MusicBrainzMetadata> candidates;
        const JsonArray media = release.GetNamedArray(L"media");
        for (std::uint32_t medium_index = 0U;
             medium_index < media.Size();
             ++medium_index) {
            const JsonObject medium = media.GetObjectAt(medium_index);
            if (json_unsigned(medium, L"track-count") != expected_tracks ||
                !medium.HasKey(L"tracks")) {
                continue;
            }
            const JsonArray tracks = medium.GetNamedArray(L"tracks");
            if (tracks.Size() != expected_tracks) {
                continue;
            }
            MusicBrainzMetadata candidate = base;
            for (std::uint32_t track_index = 0U;
                 track_index < tracks.Size();
                 ++track_index) {
                const JsonObject track = tracks.GetObjectAt(track_index);
                const JsonObject recording = track.HasKey(L"recording")
                    ? track.GetNamedObject(L"recording")
                    : JsonObject{};
                std::wstring title = json_string(track, L"title");
                if (title.empty()) {
                    title = json_string(recording, L"title");
                }
                std::uint64_t length = json_unsigned(track, L"length");
                if (length == 0U) {
                    length = json_unsigned(recording, L"length");
                }
                JsonArray credit;
                if (track.HasKey(L"artist-credit")) {
                    credit = track.GetNamedArray(L"artist-credit");
                } else if (recording.HasKey(L"artist-credit")) {
                    credit = recording.GetNamedArray(L"artist-credit");
                }
                candidate.track_titles.push_back(std::move(title));
                candidate.track_artists.push_back(artist_credit_name(credit));
                candidate.track_ids.push_back(json_string(track, L"id"));
                candidate.recording_ids.push_back(json_string(recording, L"id"));
                candidate.track_artist_ids.push_back(artist_credit_ids(credit));
                candidate.track_lengths_milliseconds.push_back(length);
            }
            candidates.push_back(std::move(candidate));
        }
        return candidates;
    } catch (const winrt::hresult_error&) {
        return {};
    }
}

[[nodiscard]] std::vector<std::wstring> parse_release_search_json(
    const std::vector<std::uint8_t>& body,
    const MusicBrainzContentQuery& query)
{
    const std::string utf8(
        reinterpret_cast<const char*>(body.data()),
        body.size());
    try {
        const JsonObject root = JsonObject::Parse(detail::utf8_to_wide(utf8));
        if (!root.HasKey(L"releases")) {
            return {};
        }
        std::vector<std::wstring> ids;
        const JsonArray releases = root.GetNamedArray(L"releases");
        for (std::uint32_t index = 0U; index < releases.Size() && ids.size() < 3U;
             ++index) {
            const JsonObject release = releases.GetObjectAt(index);
            if (json_unsigned(release, L"track-count") != query.track_titles.size() ||
                name_similarity(json_string(release, L"title"), query.album_title) < 900U) {
                continue;
            }
            const std::wstring id = json_string(release, L"id");
            if (!id.empty() && std::ranges::find(ids, id) == ids.end()) {
                ids.push_back(id);
            }
        }
        return ids;
    } catch (const winrt::hresult_error&) {
        return {};
    }
}

} // namespace

std::optional<MusicBrainzLookupPaths> make_musicbrainz_lookup_paths(
    const disc::Toc& toc)
{
    const auto identity = disc::make_musicbrainz_disc_identity(toc);
    const std::wstring fuzzy = make_fuzzy_lookup_path(toc);
    if (!identity || fuzzy.empty()) {
        return std::nullopt;
    }
    const std::wstring disc_id(
        identity->disc_id.begin(),
        identity->disc_id.end());
    constexpr std::wstring_view includes =
        L"inc=recordings+artist-credits+release-groups&cdstubs=no";
    return MusicBrainzLookupPaths{
        L"/ws/2/discid/" + disc_id + L"?" + std::wstring(includes),
        fuzzy,
    };
}

std::vector<MusicBrainzMetadata> parse_musicbrainz_candidates(
    const std::span<const std::uint8_t> body,
    const std::span<const std::uint64_t> expected_lengths,
    const bool exact_disc_id_match)
{
    auto candidates = parse_response(body, expected_lengths);
    for (auto& candidate : candidates) {
        candidate.exact_disc_id_match = exact_disc_id_match;
    }
    return candidates;
}

std::vector<MusicBrainzMetadata> parse_musicbrainz_content_release(
    const std::span<const std::uint8_t> body,
    const std::size_t expected_track_count)
{
    return parse_release_detail_json_impl(body, expected_track_count);
}

std::optional<MusicBrainzMetadata> match_musicbrainz_content(
    const MusicBrainzContentQuery& query,
    const std::span<const MusicBrainzMetadata> candidates)
{
    const std::size_t count = query.track_titles.size();
    if (count == 0U || query.track_lengths_milliseconds.size() != count ||
        query.album_title.empty()) {
        return std::nullopt;
    }
    struct RankedMatch final {
        std::uint64_t score{};
        MusicBrainzMetadata metadata;
    };
    std::vector<RankedMatch> matches;
    for (const auto& candidate : candidates) {
        if (candidate.track_titles.size() != count ||
            candidate.track_lengths_milliseconds.size() != count ||
            candidate.recording_ids.size() != count ||
            name_similarity(query.album_title, candidate.album_title) < 850U) {
            continue;
        }
        std::vector<std::vector<std::uint64_t>> costs(
            count,
            std::vector<std::uint64_t>(count));
        for (std::size_t local = 0U; local < count; ++local) {
            for (std::size_t remote = 0U; remote < count; ++remote) {
                const std::uint32_t similarity = name_similarity(
                    query.track_titles[local], candidate.track_titles[remote]);
                const auto local_length = query.track_lengths_milliseconds[local];
                const auto remote_length = candidate.track_lengths_milliseconds[remote];
                const auto difference = local_length > remote_length
                    ? local_length - remote_length
                    : remote_length - local_length;
                costs[local][remote] =
                    static_cast<std::uint64_t>(1'000U - similarity) * 10'000U +
                    std::min<std::uint64_t>(difference, 60'000U);
            }
        }
        const std::vector<std::size_t> assignment = minimum_cost_assignment(costs);
        if (assignment.size() != count) {
            continue;
        }
        std::uint64_t total_duration_difference{};
        std::uint64_t total_similarity{};
        std::uint64_t score{};
        bool valid = true;
        for (std::size_t local = 0U; local < count; ++local) {
            const std::size_t remote = assignment[local];
            const std::uint32_t similarity = name_similarity(
                query.track_titles[local], candidate.track_titles[remote]);
            const auto local_length = query.track_lengths_milliseconds[local];
            const auto remote_length = candidate.track_lengths_milliseconds[remote];
            const auto difference = local_length > remote_length
                ? local_length - remote_length
                : remote_length - local_length;
            if (similarity < 600U ||
                difference > kMaximumSingleTrackDifferenceMilliseconds ||
                candidate.recording_ids[remote].empty()) {
                valid = false;
                break;
            }
            total_similarity += similarity;
            total_duration_difference += difference;
            score += costs[local][remote];
        }
        if (!valid || total_similarity < 850U * count ||
            total_duration_difference >
                kMaximumAverageTrackDifferenceMilliseconds * count) {
            continue;
        }

        MusicBrainzMetadata reordered = candidate;
        reordered.track_titles.clear();
        reordered.track_artists.clear();
        reordered.track_ids.clear();
        reordered.recording_ids.clear();
        reordered.track_artist_ids.clear();
        reordered.track_lengths_milliseconds.clear();
        for (const std::size_t remote : assignment) {
            reordered.track_titles.push_back(candidate.track_titles[remote]);
            reordered.track_artists.push_back(
                remote < candidate.track_artists.size()
                    ? candidate.track_artists[remote]
                    : std::wstring{});
            reordered.recording_ids.push_back(candidate.recording_ids[remote]);
            reordered.track_artist_ids.push_back(
                remote < candidate.track_artist_ids.size()
                    ? candidate.track_artist_ids[remote]
                    : std::vector<std::wstring>{});
            reordered.track_lengths_milliseconds.push_back(
                candidate.track_lengths_milliseconds[remote]);
        }
        reordered.exact_disc_id_match = false;
        reordered.content_match = true;
        matches.push_back({score, std::move(reordered)});
    }
    std::ranges::sort(matches, {}, &RankedMatch::score);
    if (matches.empty()) {
        return std::nullopt;
    }
    if (matches.size() > 1U) {
        const std::uint64_t ambiguity_margin = 200'000U * count;
        const bool near_tie = matches[1].score <= matches[0].score + ambiguity_margin;
        const bool same_recordings =
            matches[1].metadata.recording_ids == matches[0].metadata.recording_ids;
        if (near_tie && !same_recordings) {
            return std::nullopt;
        }
    }
    return std::move(matches.front().metadata);
}

std::filesystem::path download_musicbrainz_cover_art(
    const std::wstring_view release_id,
    const std::wstring_view release_group_id)
{
    return download_cover_art(
        std::wstring(release_id),
        std::wstring(release_group_id));
}

MusicBrainzLookupResult lookup_musicbrainz_by_content(
    const MusicBrainzContentQuery& query)
{
    if (query.album_title.empty() || query.track_titles.empty() ||
        query.track_titles.size() != query.track_lengths_milliseconds.size()) {
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

    const std::wstring expression = std::format(
        L"release:\"{}\" AND tracks:{}",
        query.album_title,
        query.track_titles.size());
    const std::wstring search_path = L"/ws/2/release/?query=" +
        detail::percent_encode_utf8(expression) + L"&fmt=json&limit=10";
    wait_for_musicbrainz_request_slot();
    const auto search = detail::https_get(
        L"musicbrainz.org", search_path, 4U * 1'024U * 1'024U);
    if (search.system_error != ERROR_SUCCESS || search.status != 200U) {
        return {std::nullopt, search.system_error, search.status};
    }
    const std::vector<std::wstring> release_ids =
        parse_release_search_json(search.body, query);
    if (release_ids.empty()) {
        return {std::nullopt, ERROR_NOT_FOUND, search.status};
    }

    std::vector<MusicBrainzMetadata> candidates;
    for (const auto& release_id : release_ids) {
        const std::wstring path = L"/ws/2/release/" + release_id +
            L"?inc=recordings+artist-credits+release-groups&fmt=json";
        wait_for_musicbrainz_request_slot();
        const auto response = detail::https_get(
            L"musicbrainz.org", path, 4U * 1'024U * 1'024U);
        if (response.system_error != ERROR_SUCCESS || response.status != 200U) {
            continue;
        }
        auto parsed = parse_musicbrainz_content_release(
            response.body, query.track_titles.size());
        candidates.insert(
            candidates.end(),
            std::make_move_iterator(parsed.begin()),
            std::make_move_iterator(parsed.end()));
    }
    auto metadata = match_musicbrainz_content(query, candidates);
    if (metadata) {
        metadata->cover_art_path = download_cover_art(
            metadata->release_id,
            metadata->release_group_id);
    }
    MusicBrainzLookupResult result;
    result.metadata = std::move(metadata);
    result.system_error = result.metadata ? ERROR_SUCCESS : ERROR_NOT_FOUND;
    result.http_status = search.status;
    result.candidates = std::move(candidates);
    return result;
}

MusicBrainzLookupResult lookup_musicbrainz(
    const disc::Toc& toc,
    const std::wstring_view preferred_release_id)
{
    const auto paths = make_musicbrainz_lookup_paths(toc);
    if (!paths) {
        return MusicBrainzLookupResult{std::nullopt, ERROR_INVALID_PARAMETER, 0};
    }

    InternetHandle session(WinHttpOpen(
        L"CD.404/0.2.0 (https://github.com/0x00416/CD.404)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (session.get() == nullptr) {
        return MusicBrainzLookupResult{std::nullopt, GetLastError(), 0};
    }
    static_cast<void>(WinHttpSetTimeouts(session.get(), 3'000, 5'000, 5'000, 8'000));

    InternetHandle connection(WinHttpConnect(
        session.get(),
        L"musicbrainz.org",
        INTERNET_DEFAULT_HTTPS_PORT,
        0));
    if (connection.get() == nullptr) {
        return MusicBrainzLookupResult{std::nullopt, GetLastError(), 0};
    }
    std::vector<std::uint64_t> expected_lengths;
    expected_lengths.reserve(toc.tracks().size());
    for (const auto& track : toc.tracks()) {
        expected_lengths.push_back(static_cast<std::uint64_t>(
            track.frame_count * 1'000 / core::kCdSampleFramesPerSecond));
    }
    unsigned long last_http_status{};
    for (const std::wstring* path : {&paths->exact, &paths->fuzzy}) {
        wait_for_musicbrainz_request_slot();
        InternetHandle request(WinHttpOpenRequest(
            connection.get(),
            L"GET",
            path->c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE));
        if (request.get() == nullptr) {
            return MusicBrainzLookupResult{std::nullopt, GetLastError(), 0};
        }
        if (WinHttpSendRequest(
                request.get(),
                WINHTTP_NO_ADDITIONAL_HEADERS,
                0,
                WINHTTP_NO_REQUEST_DATA,
                0,
                0,
                0) == FALSE ||
            WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
            return MusicBrainzLookupResult{std::nullopt, GetLastError(), 0};
        }

        DWORD status{};
        DWORD status_size = sizeof(status);
        if (WinHttpQueryHeaders(
                request.get(),
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &status_size,
                WINHTTP_NO_HEADER_INDEX) == FALSE) {
            return MusicBrainzLookupResult{std::nullopt, GetLastError(), 0};
        }
        last_http_status = status;
        if (status != 200) {
            if (path == &paths->exact && status == 404) {
                continue;
            }
            return MusicBrainzLookupResult{std::nullopt, ERROR_SUCCESS, status};
        }
        std::vector<std::uint8_t> body;
        if (!read_response(request.get(), body)) {
            return MusicBrainzLookupResult{std::nullopt, GetLastError(), status};
        }
        auto candidates = parse_musicbrainz_candidates(
            body,
            expected_lengths,
            path == &paths->exact);
        if (candidates.empty() && path == &paths->exact) {
            continue;
        }
        const bool exact = path == &paths->exact;
        const auto preferred = std::ranges::find(
            candidates,
            preferred_release_id,
            &MusicBrainzMetadata::release_id);
        const std::size_t selected = preferred == candidates.end()
            ? 0U
            : static_cast<std::size_t>(std::distance(candidates.begin(), preferred));
        std::optional<MusicBrainzMetadata> metadata;
        if (!candidates.empty()) {
            metadata = candidates[selected];
            metadata->cover_art_path = download_cover_art(
                metadata->release_id,
                metadata->release_group_id);
        }
        MusicBrainzLookupResult lookup;
        lookup.metadata = std::move(metadata);
        lookup.system_error = lookup.metadata ? ERROR_SUCCESS : ERROR_NOT_FOUND;
        lookup.http_status = status;
        lookup.candidates = std::move(candidates);
        lookup.used_fuzzy_fallback = !exact;
        return lookup;
    }
    return MusicBrainzLookupResult{
        std::nullopt,
        ERROR_NOT_FOUND,
        last_http_status,
    };
}

} // namespace cd404::platform::windows
