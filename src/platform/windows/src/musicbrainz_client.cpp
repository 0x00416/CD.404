#include <windows.h>

#include <shlwapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <xmllite.h>
#include <wrl/client.h>

#include <cd404/platform/windows/musicbrainz_client.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <format>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cd404::platform::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint64_t kMaximumAverageTrackDifferenceMilliseconds = 2'000U;
constexpr std::uint64_t kMaximumSingleTrackDifferenceMilliseconds = 5'000U;

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

[[nodiscard]] std::wstring make_lookup_path(const disc::Toc& toc)
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

[[nodiscard]] bool read_response(HINTERNET request, std::vector<std::uint8_t>& body)
{
    constexpr std::size_t kMaximumResponseBytes = 2U * 1'024U * 1'024U;
    for (;;) {
        DWORD available{};
        if (WinHttpQueryDataAvailable(request, &available) == FALSE) {
            return false;
        }
        if (available == 0) {
            return true;
        }
        if (available > kMaximumResponseBytes - body.size()) {
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

[[nodiscard]] std::filesystem::path download_cover_art(const std::wstring& release_id)
{
    const std::filesystem::path cache_path = cover_cache_path(release_id);
    std::error_code filesystem_error;
    if (!cache_path.empty() && std::filesystem::file_size(cache_path, filesystem_error) > 0 &&
        !filesystem_error) {
        return cache_path;
    }
    if (cache_path.empty()) {
        return {};
    }

    InternetHandle session(WinHttpOpen(
        L"CD.404/0.1 (https://github.com/0x00416/CD.404)",
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
    const std::wstring path = L"/release/" + release_id + L"/front-1200";
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
            WINHTTP_NO_HEADER_INDEX) == FALSE ||
        status != 200) {
        return {};
    }

    std::vector<std::uint8_t> image;
    if (!read_response(request.get(), image) || !looks_like_image(image)) {
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
    std::optional<MusicBrainzMetadata> best_match;
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
    std::uint64_t best_score{std::numeric_limits<std::uint64_t>::max()};
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
            if (durations_match && score < state.best_score) {
                state.best_score = score;
                state.best_match = state.candidate;
            }
        }
        state.medium_depth = std::numeric_limits<std::size_t>::max();
        state.medium_matches = false;
        state.candidate.track_titles.clear();
        state.candidate.track_artists.clear();
        state.candidate.track_ids.clear();
        state.candidate.recording_ids.clear();
        state.candidate.track_artist_ids.clear();
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

[[nodiscard]] std::optional<MusicBrainzMetadata> parse_response(
    const std::span<const std::uint8_t> body,
    const std::span<const std::uint64_t> expected_lengths)
{
    if (body.empty() || body.size() > std::numeric_limits<UINT>::max()) {
        return std::nullopt;
    }

    ComPtr<IStream> stream(SHCreateMemStream(body.data(), static_cast<UINT>(body.size())));
    if (!stream) {
        return std::nullopt;
    }
    ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(
            __uuidof(IXmlReader),
            reinterpret_cast<void**>(reader.ReleaseAndGetAddressOf()),
            nullptr)) ||
        FAILED(reader->SetInput(stream.Get()))) {
        return std::nullopt;
    }
    static_cast<void>(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit));

    XmlState state;
    XmlNodeType node_type{};
    while (reader->Read(&node_type) == S_OK) {
        if (node_type == XmlNodeType_Element) {
            const wchar_t* local_name{};
            if (FAILED(reader->GetLocalName(&local_name, nullptr)) || local_name == nullptr) {
                return std::nullopt;
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
                return std::nullopt;
            }
            end_element(state, local_name, expected_lengths);
        }
    }
    return state.best_match;
}

} // namespace

MusicBrainzLookupResult lookup_musicbrainz(const disc::Toc& toc)
{
    const std::wstring path = make_lookup_path(toc);
    if (path.empty()) {
        return MusicBrainzLookupResult{std::nullopt, ERROR_INVALID_PARAMETER, 0};
    }

    InternetHandle session(WinHttpOpen(
        L"CD.404/0.1 (https://github.com/0x00416/CD.404)",
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
    InternetHandle request(WinHttpOpenRequest(
        connection.get(),
        L"GET",
        path.c_str(),
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
    if (status != 200) {
        return MusicBrainzLookupResult{std::nullopt, ERROR_SUCCESS, status};
    }

    std::vector<std::uint8_t> body;
    if (!read_response(request.get(), body)) {
        return MusicBrainzLookupResult{std::nullopt, GetLastError(), status};
    }
    std::vector<std::uint64_t> expected_lengths;
    expected_lengths.reserve(toc.tracks().size());
    for (const auto& track : toc.tracks()) {
        expected_lengths.push_back(static_cast<std::uint64_t>(
            track.frame_count * 1'000 / core::kCdSampleFramesPerSecond));
    }
    auto metadata = parse_response(body, expected_lengths);
    if (metadata) {
        metadata->cover_art_path = download_cover_art(metadata->release_id);
    }
    return MusicBrainzLookupResult{
        std::move(metadata),
        static_cast<unsigned long>(metadata ? ERROR_SUCCESS : ERROR_NOT_FOUND),
        status,
    };
}

} // namespace cd404::platform::windows
