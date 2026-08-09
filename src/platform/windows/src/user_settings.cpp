#include <windows.h>

#include <shlobj.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cd404/platform/windows/user_settings.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

namespace cd404::platform::windows {
namespace {

using namespace winrt::Windows::Data::Json;

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;
constexpr std::uintmax_t kMaximumSettingsBytes = 1U * 1'024U * 1'024U;
constexpr std::size_t kMaximumEndpointIdCharacters = 4'096;

void hash_value(std::uint64_t& hash, std::uint64_t value) noexcept
{
    for (unsigned int byte = 0; byte < sizeof(value); ++byte) {
        hash ^= (value >> (byte * 8U)) & 0xffU;
        hash *= kFnvPrime;
    }
}

[[nodiscard]] std::filesystem::path settings_path()
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
    return base / L"CD.404" / L"settings.json";
}

[[nodiscard]] std::string wide_to_utf8(const std::wstring& value)
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
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    return written == required ? result : std::string{};
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string& value)
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
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required);
    return written == required ? result : std::wstring{};
}

} // namespace

std::wstring make_disc_settings_key(const disc::Toc& toc)
{
    std::uint64_t hash = kFnvOffset;
    hash_value(hash, static_cast<std::uint64_t>(toc.origin_lba()));
    hash_value(hash, static_cast<std::uint64_t>(toc.lead_out_lba()));
    hash_value(hash, static_cast<std::uint64_t>(toc.tracks().size()));
    for (const auto& track : toc.tracks()) {
        hash_value(hash, track.number);
        hash_value(hash, static_cast<std::uint64_t>(track.start_lba));
        hash_value(hash, static_cast<std::uint64_t>(track.end_lba));
        hash_value(hash, track.is_audio ? 1U : 0U);
    }
    return std::format(L"{:016x}", hash);
}

std::wstring encode_user_settings(const UserSettings& settings)
{
    JsonObject root;
    root.Insert(L"version", JsonValue::CreateNumberValue(3));
    root.Insert(
        L"volume",
        JsonValue::CreateNumberValue(std::clamp(settings.volume, 0.0F, 1.0F)));
    root.Insert(
        L"listenbrainz_reporting_enabled",
        JsonValue::CreateBooleanValue(settings.listenbrainz_reporting_enabled));
    root.Insert(
        L"audio_output_engine",
        JsonValue::CreateStringValue(L"wasapi"));
    if (!settings.audio_endpoint_id.empty() &&
        settings.audio_endpoint_id.size() <= kMaximumEndpointIdCharacters) {
        root.Insert(
            L"audio_endpoint_id",
            JsonValue::CreateStringValue(settings.audio_endpoint_id));
    }
    root.Insert(
        L"audio_exclusive_mode",
        JsonValue::CreateBooleanValue(settings.audio_exclusive_mode));
    root.Insert(
        L"audio_allow_shared_fallback",
        JsonValue::CreateBooleanValue(settings.audio_allow_shared_fallback));

    JsonObject positions;
    for (const auto& [disc_key, position] : settings.playback_positions) {
        if (disc_key.empty() || position.track_number == 0 ||
            position.offset_frames < 0) {
            continue;
        }
        JsonObject entry;
        entry.Insert(
            L"track_number",
            JsonValue::CreateNumberValue(position.track_number));
        entry.Insert(
            L"offset_frames",
            JsonValue::CreateNumberValue(
                static_cast<double>(position.offset_frames)));
        positions.Insert(disc_key, entry);
    }
    root.Insert(L"playback_positions", positions);
    return root.Stringify().c_str();
}

UserSettings decode_user_settings(const std::wstring& json) noexcept
{
    UserSettings settings;
    if (json.empty()) {
        return settings;
    }
    try {
        const JsonObject root = JsonObject::Parse(json);
        const double volume = root.GetNamedNumber(L"volume", settings.volume);
        settings.volume = std::isfinite(volume)
            ? std::clamp(static_cast<float>(volume), 0.0F, 1.0F)
            : settings.volume;
        settings.listenbrainz_reporting_enabled = root.GetNamedBoolean(
            L"listenbrainz_reporting_enabled",
            settings.listenbrainz_reporting_enabled);
        const std::wstring output_engine = root.GetNamedString(
            L"audio_output_engine",
            L"wasapi").c_str();
        settings.audio_output_engine = output_engine == L"wasapi"
            ? AudioOutputEngine::wasapi
            : AudioOutputEngine::wasapi;
        settings.audio_endpoint_id = root.GetNamedString(
            L"audio_endpoint_id",
            L"").c_str();
        if (settings.audio_endpoint_id.size() > kMaximumEndpointIdCharacters) {
            settings.audio_endpoint_id.clear();
        }
        settings.audio_exclusive_mode = root.GetNamedBoolean(
            L"audio_exclusive_mode",
            false);
        settings.audio_allow_shared_fallback = root.GetNamedBoolean(
            L"audio_allow_shared_fallback",
            false);

        const JsonObject positions = root.GetNamedObject(
            L"playback_positions",
            JsonObject{});
        for (const auto& pair : positions) {
            const std::wstring key = pair.Key().c_str();
            if (key.empty() || pair.Value().ValueType() != JsonValueType::Object) {
                continue;
            }
            const JsonObject entry = pair.Value().GetObject();
            const double track = entry.GetNamedNumber(L"track_number", 0);
            const double offset = entry.GetNamedNumber(L"offset_frames", -1);
            if (!std::isfinite(track) || !std::isfinite(offset) ||
                track < 1 || track > 99 || offset < 0 ||
                offset > static_cast<double>(
                    std::numeric_limits<core::SampleFrame>::max())) {
                continue;
            }
            settings.playback_positions.emplace(
                key,
                SavedPlaybackPosition{
                    static_cast<unsigned int>(track),
                    static_cast<core::SampleFrame>(offset),
                });
        }
    } catch (const winrt::hresult_error&) {
        return UserSettings{};
    }
    return settings;
}

UserSettings load_user_settings() noexcept
{
    try {
        const auto path = settings_path();
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0 || size > kMaximumSettingsBytes) {
            return {};
        }
        std::ifstream input(path, std::ios::binary);
        const std::string bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        return decode_user_settings(utf8_to_wide(bytes));
    } catch (...) {
        return {};
    }
}

bool save_user_settings(const UserSettings& settings) noexcept
{
    try {
        const auto path = settings_path();
        if (path.empty()) {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return false;
        }
        const std::string bytes = wide_to_utf8(encode_user_settings(settings));
        if (bytes.empty() || bytes.size() > kMaximumSettingsBytes) {
            return false;
        }
        const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
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
