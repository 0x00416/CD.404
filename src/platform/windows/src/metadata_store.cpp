#include <windows.h>

#include <shlobj.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cd404/platform/windows/metadata_store.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace cd404::platform::windows {
namespace {

using namespace winrt::Windows::Data::Json;

constexpr std::uintmax_t kMaximumCacheBytes = 2U * 1'024U * 1'024U;

[[nodiscard]] int source_priority(const MetadataSource source) noexcept
{
    return static_cast<int>(source);
}

[[nodiscard]] JsonObject encode_value(const SourcedMetadataValue& field)
{
    JsonObject object;
    object.Insert(L"value", JsonValue::CreateStringValue(field.value));
    object.Insert(
        L"source",
        JsonValue::CreateNumberValue(static_cast<int>(field.source)));
    return object;
}

[[nodiscard]] SourcedMetadataValue decode_value(
    const JsonObject& object,
    const MetadataSource legacy_source = MetadataSource::unknown)
{
    const double raw_source = object.GetNamedNumber(
        L"source",
        static_cast<int>(legacy_source));
    const int source = std::clamp(
        static_cast<int>(raw_source),
        static_cast<int>(MetadataSource::unknown),
        static_cast<int>(MetadataSource::user));
    return {
        object.GetNamedString(L"value", L"").c_str(),
        static_cast<MetadataSource>(source),
    };
}

[[nodiscard]] bool valid_disc_key(const std::wstring_view key) noexcept
{
    return !key.empty() && key.size() <= 64U &&
        std::ranges::all_of(key, [](const wchar_t character) {
            return (character >= L'0' && character <= L'9') ||
                   (character >= L'a' && character <= L'f') ||
                   (character >= L'A' && character <= L'F') ||
                   character == L'-';
        });
}

[[nodiscard]] std::filesystem::path cache_path(const std::wstring_view disc_key)
{
    if (!valid_disc_key(disc_key)) {
        return {};
    }
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
    return base / L"CD.404" / L"Cache" / L"metadata" /
        (std::wstring(disc_key) + L".json");
}

[[nodiscard]] std::string wide_to_utf8(const std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    return WideCharToMultiByte(
               CP_UTF8,
               WC_ERR_INVALID_CHARS,
               value.data(),
               static_cast<int>(value.size()),
               result.data(),
               required,
               nullptr,
               nullptr) == required
        ? result
        : std::string{};
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    return MultiByteToWideChar(
               CP_UTF8,
               MB_ERR_INVALID_CHARS,
               value.data(),
               static_cast<int>(value.size()),
               result.data(),
               required) == required
        ? result
        : std::wstring{};
}

} // namespace

const wchar_t* to_string(const MetadataSource source) noexcept
{
    switch (source) {
    case MetadataSource::unknown:
        return L"未知";
    case MetadataSource::itunes:
        return L"iTunes";
    case MetadataSource::gnudb:
        return L"GnuDB";
    case MetadataSource::musicbrainz:
        return L"MusicBrainz";
    case MetadataSource::cd_text:
        return L"CD-TEXT";
    case MetadataSource::user:
        return L"用户修订";
    }
    return L"未知";
}

bool merge_metadata_value(
    SourcedMetadataValue& destination,
    const std::wstring_view value,
    const MetadataSource source)
{
    if (value.empty() || destination.source == MetadataSource::user ||
        (!destination.value.empty() &&
         source_priority(source) <= source_priority(destination.source))) {
        return false;
    }
    destination.value = value;
    destination.source = source;
    return true;
}

void revise_metadata_value(
    SourcedMetadataValue& destination,
    const std::wstring_view value)
{
    destination.value = value;
    destination.source = MetadataSource::user;
}

std::size_t select_metadata_candidate(
    const std::vector<MetadataReleaseCandidate>& candidates,
    const std::wstring_view preferred_release_id) noexcept
{
    const auto match = std::ranges::find(
        candidates,
        preferred_release_id,
        &MetadataReleaseCandidate::release_id);
    return match == candidates.end()
        ? 0U
        : static_cast<std::size_t>(std::distance(candidates.begin(), match));
}

std::wstring encode_metadata_cache(const MetadataCacheEntry& entry)
{
    JsonObject root;
    root.Insert(L"version", JsonValue::CreateNumberValue(1));
    root.Insert(L"disc_key", JsonValue::CreateStringValue(entry.disc_key));
    root.Insert(
        L"selected_release_id",
        JsonValue::CreateStringValue(entry.selected_release_id));
    root.Insert(
        L"updated_unix_seconds",
        JsonValue::CreateNumberValue(
            static_cast<double>(entry.updated_unix_seconds)));
    root.Insert(L"album_title", encode_value(entry.metadata.album_title));
    root.Insert(L"album_artist", encode_value(entry.metadata.album_artist));
    JsonArray tracks;
    for (const auto& track : entry.metadata.tracks) {
        JsonObject object;
        object.Insert(L"title", encode_value(track.title));
        object.Insert(L"artist", encode_value(track.artist));
        tracks.Append(object);
    }
    root.Insert(L"tracks", tracks);
    return root.Stringify().c_str();
}

std::optional<MetadataCacheEntry> decode_metadata_cache(
    const std::wstring_view json) noexcept
{
    try {
        const JsonObject root = JsonObject::Parse(json);
        const int version = static_cast<int>(root.GetNamedNumber(L"version", 0));
        if (version < 0 || version > 1) {
            return std::nullopt;
        }
        MetadataCacheEntry entry;
        entry.disc_key = root.GetNamedString(L"disc_key", L"").c_str();
        if (!valid_disc_key(entry.disc_key)) {
            return std::nullopt;
        }
        entry.selected_release_id = root.GetNamedString(
            L"selected_release_id",
            L"").c_str();
        const double updated = root.GetNamedNumber(L"updated_unix_seconds", 0);
        if (updated < 0 || updated > static_cast<double>(
                std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        entry.updated_unix_seconds = static_cast<std::int64_t>(updated);
        if (version == 0) {
            entry.metadata.album_title = {
                root.GetNamedString(L"album_title", L"").c_str(),
                MetadataSource::unknown};
            entry.metadata.album_artist = {
                root.GetNamedString(L"album_artist", L"").c_str(),
                MetadataSource::unknown};
        } else {
            entry.metadata.album_title = decode_value(
                root.GetNamedObject(L"album_title", JsonObject{}));
            entry.metadata.album_artist = decode_value(
                root.GetNamedObject(L"album_artist", JsonObject{}));
        }
        const JsonArray tracks = root.GetNamedArray(L"tracks", JsonArray{});
        if (tracks.Size() > 99U) {
            return std::nullopt;
        }
        for (const auto& item : tracks) {
            if (item.ValueType() != JsonValueType::Object) {
                return std::nullopt;
            }
            const JsonObject object = item.GetObject();
            if (version == 0) {
                entry.metadata.tracks.push_back({
                    {object.GetNamedString(L"title", L"").c_str(), MetadataSource::unknown},
                    {object.GetNamedString(L"artist", L"").c_str(), MetadataSource::unknown},
                });
            } else {
                entry.metadata.tracks.push_back({
                    decode_value(object.GetNamedObject(L"title", JsonObject{})),
                    decode_value(object.GetNamedObject(L"artist", JsonObject{})),
                });
            }
        }
        return entry;
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
}

bool metadata_cache_is_fresh(
    const MetadataCacheEntry& entry,
    const std::int64_t now_unix_seconds,
    const std::int64_t maximum_age_seconds) noexcept
{
    return maximum_age_seconds >= 0 && entry.updated_unix_seconds >= 0 &&
        now_unix_seconds >= entry.updated_unix_seconds &&
        now_unix_seconds - entry.updated_unix_seconds <= maximum_age_seconds;
}

std::optional<MetadataCacheEntry> load_metadata_cache(
    const std::wstring_view disc_key) noexcept
{
    try {
        const auto path = cache_path(disc_key);
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (path.empty() || error || size == 0 || size > kMaximumCacheBytes) {
            return std::nullopt;
        }
        std::ifstream input(path, std::ios::binary);
        const std::string bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        return decode_metadata_cache(utf8_to_wide(bytes));
    } catch (...) {
        return std::nullopt;
    }
}

bool save_metadata_cache(const MetadataCacheEntry& entry) noexcept
{
    try {
        const auto path = cache_path(entry.disc_key);
        if (path.empty()) {
            return false;
        }
        const std::string bytes = wide_to_utf8(encode_metadata_cache(entry));
        if (bytes.empty() || bytes.size() > kMaximumCacheBytes) {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return false;
        }
        const std::filesystem::path temporary(path.wstring() + L".tmp");
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output) {
                return false;
            }
        }
        return MoveFileExW(
                   temporary.c_str(),
                   path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } catch (...) {
        return false;
    }
}

} // namespace cd404::platform::windows
