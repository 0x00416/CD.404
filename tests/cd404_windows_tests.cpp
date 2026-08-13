#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <dbt.h>
#include <objbase.h>
#include <winsqlite/winsqlite3.h>

#include <cd404/audio/playback_state_machine.hpp>
#include <cd404/core/version.hpp>
#include <cd404/platform/windows/autoplay_policy.hpp>
#include <cd404/platform/windows/cdda_playback_engine.hpp>
#include <cd404/platform/windows/device_lifecycle.hpp>
#include <cd404/platform/windows/diagnostics.hpp>
#include <cd404/platform/windows/gnudb_client.hpp>
#include <cd404/platform/windows/listenbrainz_reporter.hpp>
#include <cd404/platform/windows/listenbrainz_queue.hpp>
#include <cd404/platform/windows/lyrics_codecs.hpp>
#include <cd404/platform/windows/musicbrainz_client.hpp>
#include <cd404/platform/windows/online_lyrics.hpp>
#include <cd404/platform/windows/metadata_store.hpp>
#include <cd404/platform/windows/system_media_controls.hpp>
#include <cd404/platform/windows/user_settings.hpp>
#include <cd404/platform/windows/wasapi_output.hpp>
#include <cd404/ui/animation_timing.hpp>
#include <cd404/ui/application_launch.hpp>
#include <cd404/ui/theme.hpp>
#include <cd404/ui/playback_presenter.hpp>
#include <cd404/ui/metadata_source_model.hpp>
#include <cd404/ui/settings_model.hpp>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures{};

void expect(const bool condition, const std::string_view description)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << description << '\n';
    }
}

void test_result_semantics()
{
    using namespace cd404;

    platform::windows::CddaPlaybackResult result;
    result.final_state = audio::PlaybackState::completed;
    expect(
        result.succeeded(),
        "completed result without an error succeeds");

    result.error = platform::windows::CddaPlaybackError::incomplete;
    expect(
        !result.succeeded(),
        "completed state does not hide an explicit playback error");

    result.error = platform::windows::CddaPlaybackError::none;
    result.final_state = audio::PlaybackState::playing;
    expect(
        !result.succeeded(),
        "non-terminal playback is not reported as successful");
}

void test_autoplay_launch_arguments()
{
    using cd404::ui::normalize_autoplay_drive_root;
    expect(
        normalize_autoplay_drive_root(L"d:\\") == L"D:\\" &&
            normalize_autoplay_drive_root(L" E:/ ") == L"E:\\" &&
            normalize_autoplay_drive_root(L"F:") == L"F:\\",
        "AutoPlay normalizes shell-provided optical drive roots");
    expect(
        !normalize_autoplay_drive_root(L"C:\\music") &&
            !normalize_autoplay_drive_root(L"\\\\server\\disc") &&
            !normalize_autoplay_drive_root(L"not-a-drive"),
        "AutoPlay rejects paths that are not drive roots");

    const std::array<std::wstring_view, 3> autoplay{
        L"CD.404.exe", L"/autoplay", L"g:\\"};
    const auto options = cd404::ui::parse_application_launch_options(autoplay);
    expect(
        options.autoplay_drive_root == L"G:\\",
        "AutoPlay command line selects the drive supplied by Windows Shell");
    const std::array<std::wstring_view, 1> normal{L"CD.404.exe"};
    expect(
        !cd404::ui::parse_application_launch_options(normal).autoplay_drive_root,
        "normal launches do not request automatic playback");
}

void test_autoplay_policy_masks()
{
    using namespace cd404::platform::windows;

    expect(
        drive_type_mask_blocks_cdrom(0xB1U) &&
            !drive_type_mask_blocks_cdrom(0x91U) &&
            clear_cdrom_from_drive_type_mask(0xB1U) == 0x91U,
        "AutoPlay policy repair clears only the CD-ROM disable bit");

    AudioCdAutoplayPolicyStatus status;
    expect(
        status.enabled() && !status.repairable_for_current_user(),
        "AutoPlay status reports an unblocked policy as enabled");
    status.current_user_blocked = true;
    expect(
        !status.enabled() && status.repairable_for_current_user(),
        "current-user CD-ROM policy blocks AutoPlay and is repairable");
    status.current_user_blocked = false;
    status.machine_blocked = true;
    expect(
        !status.enabled() && !status.repairable_for_current_user(),
        "machine CD-ROM policy is detected without claiming user-level repair");
}

void test_autoplay_policy_repair_live()
{
    using namespace cd404::platform::windows;
    constexpr wchar_t policy_key[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    constexpr wchar_t value_name[] = L"NoDriveTypeAutoRun";

    HKEY key{};
    const LSTATUS open_status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        policy_key,
        0,
        nullptr,
        0,
        KEY_QUERY_VALUE | KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    expect(open_status == ERROR_SUCCESS, "live AutoPlay test opens the user policy key");
    if (open_status != ERROR_SUCCESS) {
        return;
    }

    DWORD original_type{};
    DWORD original_size{};
    const LSTATUS original_status = RegQueryValueExW(
        key, value_name, nullptr, &original_type, nullptr, &original_size);
    std::vector<BYTE> original_data(original_size);
    if (original_status == ERROR_SUCCESS && original_size != 0U) {
        DWORD read_size = original_size;
        static_cast<void>(RegQueryValueExW(
            key,
            value_name,
            nullptr,
            &original_type,
            original_data.data(),
            &read_size));
        original_data.resize(read_size);
    }

    const std::array<BYTE, 4> blocked{0xB1U, 0U, 0U, 0U};
    const LSTATUS seed_status = RegSetValueExW(
        key,
        value_name,
        0,
        REG_BINARY,
        blocked.data(),
        static_cast<DWORD>(blocked.size()));
    expect(seed_status == ERROR_SUCCESS, "live AutoPlay test seeds a blocked binary policy");

    const auto before = query_audio_cd_autoplay_policy();
    const auto repair = repair_audio_cd_autoplay_policy();
    DWORD repaired_type{};
    std::array<BYTE, 4> repaired{};
    DWORD repaired_size = static_cast<DWORD>(repaired.size());
    const LSTATUS repaired_status = RegQueryValueExW(
        key,
        value_name,
        nullptr,
        &repaired_type,
        repaired.data(),
        &repaired_size);
    expect(
        before.current_user_blocked && repair.succeeded &&
            repair.explorer_restart_required &&
            repaired_status == ERROR_SUCCESS && repaired_type == REG_BINARY &&
            repaired_size == repaired.size() && repaired[0] == 0x91U,
        "live AutoPlay repair changes the blocked binary policy from 0xB1 to 0x91");

    if (original_status == ERROR_SUCCESS) {
        static_cast<void>(RegSetValueExW(
            key,
            value_name,
            0,
            original_type,
            original_data.empty() ? nullptr : original_data.data(),
            static_cast<DWORD>(original_data.size())));
    } else {
        static_cast<void>(RegDeleteValueW(key, value_name));
    }
    RegCloseKey(key);
}

void test_theme_palettes()
{
    const auto dark = cd404::ui::make_theme_palette(true, false);
    const auto light = cd404::ui::make_theme_palette(false, false);
    const auto high = cd404::ui::make_theme_palette(false, true);
    expect(
        dark.dark && !dark.high_contrast && !light.dark &&
            dark.background != light.background && dark.text != light.text &&
            dark.accent_text != dark.accent,
        "dark and light system palettes provide distinct readable surfaces");
    expect(
        high.high_contrast && high.text != high.background &&
            high.border == high.text && high.accent != high.background &&
            high.accent_text != high.accent,
        "high-contrast palette has explicit system-independent foreground separation");
}

void test_animation_timing()
{
    using namespace cd404::ui;

    expect(
        display_refresh_interval_100ns(60U, 1U) == 166'667 &&
            display_refresh_interval_100ns(144U, 1U) == 69'444 &&
            display_refresh_interval_100ns(60'000U, 1'001U) == 166'833 &&
            display_refresh_interval_100ns(0U, 1U) ==
                kDefaultFrameInterval100ns &&
            display_refresh_interval_100ns(1'000U, 1U) ==
                kDefaultFrameInterval100ns,
        "display refresh ratios produce bounded high-resolution frame intervals");

    const auto beginning = lyric_transition_frame(0.0);
    const auto middle = lyric_transition_frame(0.14);
    const auto end = lyric_transition_frame(0.28);
    expect(
        beginning.active && beginning.progress == 0.0F &&
            beginning.offset_factor == 1.0F &&
            std::abs(middle.progress - 0.875F) < 0.0001F &&
            std::abs(middle.offset_factor - 0.125F) < 0.0001F &&
            !end.active && end.progress == 1.0F &&
            end.offset_factor == 0.0F && end.incoming_opacity == 1.0F,
        "lyric transition uses a frame-rate-independent cubic ease-out");
}

void test_settings_model()
{
    using namespace cd404;

    std::vector<platform::windows::WasapiEndpoint> endpoints{
        {L"endpoint-a", L"A", true},
        {L"endpoint-b", L"B", false},
    };
    platform::windows::UserSettings settings;
    std::size_t selected = 0;

    expect(
        ui::find_audio_endpoint_index(endpoints, L"endpoint-b") == 1 &&
            ui::find_audio_endpoint_index(endpoints, L"missing") == 0,
        "settings model restores a selected endpoint or uses the default entry");
    expect(
        ui::select_next_audio_endpoint(endpoints, selected, settings) &&
            selected == 1 && settings.audio_endpoint_id == L"endpoint-b",
        "settings model advances and persists endpoint selection");
    expect(
        ui::select_audio_endpoint(endpoints, 0, selected, settings) &&
            selected == 0 && settings.audio_endpoint_id == L"endpoint-a",
        "settings model applies an explicit dropdown endpoint selection");
    expect(
        !ui::select_audio_endpoint(endpoints, 9, selected, settings) &&
            selected == 0 && settings.audio_endpoint_id == L"endpoint-a",
        "settings model rejects an out-of-range dropdown selection");
    expect(
        ui::audio_output_engine_label(
            platform::windows::AudioOutputEngine::wasapi) ==
            L"WASAPI（Windows 音频）" &&
            ui::audio_output_engine_count() == 1U &&
            ui::audio_output_engine_at(0) ==
                platform::windows::AudioOutputEngine::wasapi &&
            !ui::audio_output_engine_at(1),
        "settings model exposes a future-ready audio engine selector");

    ui::toggle_exclusive_output(settings);
    expect(
        settings.audio_exclusive_mode &&
            !settings.audio_allow_shared_fallback,
        "exclusive output is explicit and does not silently enable fallback");
    expect(
        ui::toggle_shared_fallback(settings) &&
            settings.audio_allow_shared_fallback,
        "shared fallback can be enabled only while exclusive mode is active");
    ui::toggle_exclusive_output(settings);
    expect(
        !settings.audio_exclusive_mode &&
            !settings.audio_allow_shared_fallback &&
            !ui::toggle_shared_fallback(settings),
        "leaving exclusive mode clears fallback and shared mode cannot toggle it");
}

void test_playback_error_presentation()
{
    using namespace cd404;

    platform::windows::CddaPlaybackResult result;
    result.error = platform::windows::CddaPlaybackError::no_ready_audio_cd;
    expect(
        ui::playback_error_message(result) == L"当前光盘已不可用",
        "playback presenter explains unavailable media");

    result.error = platform::windows::CddaPlaybackError::endpoint_underrun;
    expect(
        ui::playback_error_message(result) == L"光驱供给不足，播放已停止",
        "playback presenter explains strict underrun termination");

    result.error = platform::windows::CddaPlaybackError::output_open_failed;
    result.audio_status = AUDCLNT_E_DEVICE_INVALIDATED;
    result.used_default_output_endpoint = true;
    expect(
        ui::playback_error_message(result).find(L"默认音频设备恢复失败") !=
            std::wstring::npos,
        "playback presenter distinguishes recoverable default endpoint failure");

    result.used_default_output_endpoint = false;
    expect(
        ui::playback_error_message(result).find(L"音频设备错误") !=
            std::wstring::npos,
        "playback presenter keeps selected endpoint failure explicit");
}

void test_metadata_source_capsules()
{
    using namespace cd404;

    const std::array<std::wstring, 5> online_sources{
        L"MusicBrainz", L"GnuDB", L"MusicBrainz", L"", L"iTunes"};
    const auto live = ui::make_metadata_source_labels(
        true, true, online_sources);
    expect(
        live == std::vector<std::wstring>{
                    L"CD-TEXT", L"MusicBrainz", L"GnuDB", L"iTunes"},
        "metadata acquisition sources remain separate, ordered and unique");

    const auto offline = ui::make_metadata_source_labels(true, true, {});
    expect(
        offline == std::vector<std::wstring>{L"CD-TEXT", L"Local"},
        "offline metadata identifies CD-TEXT and persistent cache separately");
}

void test_diagnostic_redaction_and_export()
{
    using namespace cd404::platform::windows;

    const std::wstring secret =
        L"Authorization: Token abc-secret token=second-secret "
        L"C:\\Users\\Alice\\AppData\\Local\\CD.404\\listenbrainz.db\n"
        L"endpoint={0.0.0.00000000}.{e1880c7c-30a8-4662-b49d-79db6df5824b}";
    const std::wstring redacted = redact_diagnostic_text(secret);
    expect(
        redacted.find(L"abc-secret") == std::wstring::npos &&
            redacted.find(L"second-secret") == std::wstring::npos &&
            redacted.find(L"Alice") == std::wstring::npos &&
            redacted.find(L"e1880c7c") == std::wstring::npos &&
            redacted.find(L"[REDACTED_TOKEN]") != std::wstring::npos &&
            redacted.find(L"[REDACTED_PATH]") != std::wstring::npos &&
            redacted.find(L"[REDACTED_ENDPOINT]") != std::wstring::npos,
        "diagnostic redaction removes tokens, local paths and stable endpoint IDs");

    DiagnosticLog log(2);
    log.record(L"first", L"discarded by bounded ring");
    log.record(L"http", secret);
    log.record(L"playback", L"error=0x88890004");
    const auto entries = log.snapshot();
    expect(
        entries.size() == 2 && entries.front().component == L"http" &&
            entries.front().message.find(L"abc-secret") == std::wstring::npos,
        "diagnostic log is bounded and redacts at ingestion");

    const auto path = std::filesystem::temp_directory_path() /
        L"cd404-diagnostic-redaction-test.txt";
    const bool exported = log.export_to(path);
    std::ifstream input(path, std::ios::binary);
    const std::string contents{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    std::error_code error;
    std::filesystem::remove(path, error);
    expect(
        exported && contents.find("abc-secret") == std::string::npos &&
            contents.find("Alice") == std::string::npos &&
            contents.find("e1880c7c") == std::string::npos &&
            contents.find("REDACTED_TOKEN") != std::string::npos,
        "diagnostic export applies a second redaction pass and writes no injected secret");
}

void test_device_failure_classification()
{
    using namespace cd404::platform::windows;

    CddaPlaybackResult endpoint;
    endpoint.error = CddaPlaybackError::output_failed;
    endpoint.audio_status = AUDCLNT_E_DEVICE_INVALIDATED;
    expect(
        is_recoverable_default_endpoint_failure(endpoint),
        "WASAPI device invalidation requests default-endpoint recovery");
    endpoint.audio_status = E_ACCESSDENIED;
    expect(
        !is_recoverable_default_endpoint_failure(endpoint),
        "unrelated WASAPI failures are not silently retried");
    endpoint.error = CddaPlaybackError::read_failed;
    endpoint.audio_status = AUDCLNT_E_DEVICE_INVALIDATED;
    expect(
        !is_recoverable_default_endpoint_failure(endpoint),
        "a disc read failure cannot be misclassified as an endpoint change");

    CddaPlaybackResult media;
    media.error = CddaPlaybackError::read_failed;
    media.system_error = ERROR_MEDIA_CHANGED;
    expect(
        is_media_unavailable_failure(media),
        "media-change read errors request a disc refresh");
    media.system_error = ERROR_CRC;
    expect(
        !is_media_unavailable_failure(media),
        "strict read failures remain visible instead of looking like removal");
    media.error = CddaPlaybackError::no_ready_audio_cd;
    expect(
        is_media_unavailable_failure(media),
        "a no-disc playback result refreshes stale UI state");
}

class ScriptedWasapiBackend final
    : public cd404::platform::windows::IWasapiSessionBackend {
public:
    std::int32_t exclusive_support{S_OK};
    std::int32_t shared_support{S_OK};
    std::int32_t exclusive_initialize{S_OK};
    std::int32_t shared_initialize{S_OK};
    std::vector<cd404::platform::windows::WasapiShareMode> support_checks;
    std::vector<cd404::platform::windows::WasapiShareMode> initializations;

    std::int32_t query_format_support(
        std::wstring_view,
        const cd404::platform::windows::WasapiShareMode mode,
        const cd404::platform::windows::WasapiPcmFormat& format) noexcept override
    {
        expect(
            format.sample_rate == 44'100 && format.bits_per_sample == 16 &&
                format.channel_count == 2,
            "WASAPI negotiation always requests native audio-CD PCM");
        support_checks.push_back(mode);
        return mode == cd404::platform::windows::WasapiShareMode::exclusive
            ? exclusive_support
            : shared_support;
    }

    std::int32_t initialize(
        std::wstring_view,
        const cd404::platform::windows::WasapiShareMode mode,
        const cd404::platform::windows::WasapiPcmFormat&) noexcept override
    {
        initializations.push_back(mode);
        return mode == cd404::platform::windows::WasapiShareMode::exclusive
            ? exclusive_initialize
            : shared_initialize;
    }
};

class ScriptedHttpClient final
    : public cd404::platform::windows::HttpClient {
public:
    cd404::platform::windows::HttpResponse validation;
    cd404::platform::windows::HttpResponse submission;
    std::atomic_uint get_calls{};
    std::atomic_uint post_calls{};

    cd404::platform::windows::HttpResponse get(
        std::wstring_view,
        std::wstring_view,
        std::wstring_view,
        std::size_t) override
    {
        get_calls.fetch_add(1, std::memory_order_relaxed);
        return validation;
    }

    cd404::platform::windows::HttpResponse post(
        std::wstring_view,
        std::wstring_view,
        std::wstring_view,
        std::span<const std::uint8_t>,
        std::size_t) override
    {
        post_calls.fetch_add(1, std::memory_order_relaxed);
        return submission;
    }
};

class LyricsHttpClient final
    : public cd404::platform::windows::HttpClient {
public:
    std::vector<std::wstring> hosts;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> headers;

    cd404::platform::windows::HttpResponse get(
        const std::wstring_view host,
        const std::wstring_view path,
        const std::wstring_view request_headers,
        std::size_t) override
    {
        hosts.emplace_back(host);
        paths.emplace_back(path);
        headers.emplace_back(request_headers);
        cd404::platform::windows::HttpResponse response;
        if (host == L"lrclib.net") {
            constexpr std::string_view body =
                R"json([{"id":1,"trackName":"Synthetic Karaoke","artistName":"Test Artist","albumName":"Test Album","duration":120,"instrumental":false,"plainLyrics":"AB","syncedLyrics":"[00:01.000]A[00:01.500]B[00:02.000]\n[00:01.000]\u8bd1\u6587"}])json";
            response.status = 200;
            response.body.assign(body.begin(), body.end());
        } else {
            response.status = 503;
        }
        return response;
    }

    cd404::platform::windows::HttpResponse post(
        std::wstring_view,
        std::wstring_view,
        std::wstring_view,
        std::span<const std::uint8_t>,
        std::size_t) override
    {
        return {};
    }
};

class WordPreferenceLyricsHttpClient final
    : public cd404::platform::windows::HttpClient {
public:
    cd404::platform::windows::HttpResponse get(
        const std::wstring_view host,
        const std::wstring_view path,
        std::wstring_view,
        std::size_t) override
    {
        cd404::platform::windows::HttpResponse response;
        if (host == L"lrclib.net") {
            constexpr std::string_view body =
                R"json([{"trackName":"Preference Song","artistName":"Alpha Beta","albumName":"Preference Album","duration":120,"syncedLyrics":"[00:01.000]Line one\n[00:02.000]Line two"}])json";
            response.status = 200;
            response.body.assign(body.begin(), body.end());
        } else if (host == L"music.163.com" &&
                   path.starts_with(L"/api/search/get")) {
            constexpr std::string_view body =
                R"json({"result":{"songs":[{"id":42,"name":"Preference Song Live","duration":120000,"artists":[{"name":"Alpha Beta"}],"album":{"name":"Preference Album"}}]}})json";
            response.status = 200;
            response.body.assign(body.begin(), body.end());
        } else if (host == L"music.163.com" &&
                   path.starts_with(L"/api/song/lyric")) {
            constexpr std::string_view body =
                R"json({"yrc":{"lyric":"[1000,900](1000,300,0)One (1300,600,0)Two"}})json";
            response.status = 200;
            response.body.assign(body.begin(), body.end());
        } else {
            response.status = 503;
        }
        return response;
    }

    cd404::platform::windows::HttpResponse post(
        const std::wstring_view host,
        std::wstring_view,
        std::wstring_view,
        std::span<const std::uint8_t>,
        std::size_t) override
    {
        cd404::platform::windows::HttpResponse response;
        if (host == L"edge.microsoft.com") {
            constexpr std::string_view body =
                R"json([{"translations":[{"text":"\u4e00\u4e8c","to":"zh-Hans"}]}])json";
            response.status = 200;
            response.body.assign(body.begin(), body.end());
        }
        return response;
    }
};

void test_online_lyrics_matching_and_parsing()
{
    using namespace cd404;
    auto client = std::make_shared<LyricsHttpClient>();
    std::optional<platform::windows::OnlineLyricsLookupResult> result;
    std::jthread worker([&] {
        result = platform::windows::lookup_online_lyrics(
            core::LyricsMatchQuery{
                L"Synthetic Karaoke",
                L"Test Artist",
                L"Test Album",
                120'000,
            },
            client,
            false);
    });
    worker.join();
    expect(
        result && result->lyrics && result->lyrics->has_word_timing &&
            result->lyrics->source == L"LRCLIB" &&
            result->lyrics->lines.size() == 1U &&
            result->lyrics->lines[0].text == L"AB" &&
            result->lyrics->lines[0].translation == L"\u8bd1\u6587",
        "online lyrics lookup scores metadata and preserves enhanced bilingual tokens");
    expect(
        client->hosts.size() == 4U &&
            client->hosts[0] == L"lrclib.net" &&
            client->hosts[1] == L"music.163.com" &&
            client->hosts[2] == L"c.y.qq.com" &&
            client->hosts[3] == L"songsearch.kugou.com" &&
            client->paths[0].find(L"track_name=Synthetic%20Karaoke") !=
                std::wstring::npos &&
            client->headers[0].find(L"admin@416.best") != std::wstring::npos,
        "lyrics providers receive encoded metadata and an identifying user agent");

    auto catalog_client = std::make_shared<LyricsHttpClient>();
    std::optional<platform::windows::OnlineLyricsCatalogResult> catalog;
    std::jthread catalog_worker([&] {
        catalog = platform::windows::search_online_lyrics_catalog(
            L"Synthetic Karaoke Test Artist",
            core::LyricsMatchQuery{
                L"Synthetic Karaoke", L"Test Artist", L"Test Album", 120'000},
            catalog_client);
    });
    catalog_worker.join();
    expect(
        catalog && catalog->items.size() == 1U &&
            catalog->items[0].source == L"LRCLIB" &&
            catalog->items[0].provider_key == L"1" &&
            !catalog_client->paths.empty() &&
            catalog_client->paths[0].find(
                L"q=Synthetic%20Karaoke%20Test%20Artist") != std::wstring::npos,
        "manual lyrics catalog uses user keywords and exposes every searchable item");

    std::optional<platform::windows::OnlineLyricsLookupResult> preference;
    std::jthread preference_worker([&] {
        preference = platform::windows::lookup_online_lyrics(
            core::LyricsMatchQuery{
                L"Preference Song",
                L"Alpha Beta",
                L"Preference Album",
                120'000,
            },
            std::make_shared<WordPreferenceLyricsHttpClient>(),
            false);
    });
    preference_worker.join();
    expect(
        preference && preference->lyrics &&
            preference->lyrics->source == L"LRCLIB" &&
            !preference->lyrics->has_word_timing &&
            preference->lyrics->lines.size() == 2U,
        "an exact identity match is not displaced by a weaker word-timed candidate");

    std::optional<platform::windows::OnlineLyricsLookupResult> resolved;
    std::jthread resolve_worker([&] {
        resolved = platform::windows::resolve_online_lyrics_item(
            platform::windows::OnlineLyricsSearchItem{
                platform::windows::OnlineLyricsProvider::netease,
                L"42",
                L"NetEase",
                {L"Preference Song Live", L"Alpha Beta", L"Preference Album",
                 120'000, true, true},
                90.0,
            },
            std::make_shared<WordPreferenceLyricsHttpClient>());
    });
    resolve_worker.join();
    expect(
        resolved && resolved->lyrics && resolved->lyrics->source == L"NetEase" &&
            resolved->lyrics->has_word_timing &&
            resolved->lyrics->lines[0].tokens.size() == 2U,
        "manual lyrics selection resolves only the chosen provider item");

    std::vector<platform::windows::OnlineLyricsCandidate> priority{
        {{L"Song", L"Artist", L"Album", 120'000, true, true},
         {{{1'000, 2'000, L"Wrong", {}, L"", {}}}, L"Kugou", true},
         99.0},
        {{L"Song", L"Artist", L"Album", 120'000, true, true},
         {{{1'000, 2'000, L"Correct", {}, L"", {}}}, L"QQ Music", true},
         82.0},
    };
    expect(
        platform::windows::select_online_lyrics_candidate(priority) ==
            std::optional<std::size_t>(1U),
        "Kugou remains an automatic fallback even when its metadata score is higher");
    priority.erase(priority.begin() + 1);
    expect(
        platform::windows::select_online_lyrics_candidate(priority) ==
            std::optional<std::size_t>(0U),
        "Kugou can still supply lyrics when every other provider is unavailable");
}

void test_cloud_lyrics_codecs()
{
    using namespace cd404;
    constexpr std::array<std::uint8_t, 41> krc{
        0x6b, 0x72, 0x63, 0x31, 0x38, 0xdb, 0xea, 0x41, 0x6a,
        0x02, 0x44, 0x97, 0xe0, 0x02, 0x01, 0xa5, 0x7b, 0xe3,
        0xbe, 0x58, 0x46, 0x75, 0x6c, 0x9b, 0x00, 0x84, 0x1a,
        0xf0, 0x50, 0x87, 0xfd, 0xed, 0x72, 0x35, 0xb3, 0xba,
        0x41, 0xef, 0x6f, 0x7d, 0x03,
    };
    const auto krc_plain = platform::windows::detail::decode_krc(krc);
    const auto krc_lyrics = krc_plain ? core::parse_krc(*krc_plain) : core::LyricsDocument{};
    expect(
        krc_lyrics.has_word_timing && krc_lyrics.lines.size() == 1U &&
            krc_lyrics.lines[0].text == L"\u9177\u72d7" &&
            krc_lyrics.lines[0].tokens[1].start_milliseconds == 1'300,
        "KRC XOR and zlib decoding feeds absolute word cues into the parser");

    constexpr std::wstring_view bilingual_krc =
        L"[language:eyJjb250ZW50IjpbeyJ0eXBlIjoxLCJseXJpY0NvbnRlbnQiOltbXSxbXSxbXSxbIlQxIl0sWyJUMiJdXX1dfQ==]\n"
        L"[100,100]<0,100,0>Artist - Title\n"
        L"[200,100]<0,100,0>作词: Author\n"
        L"[300,100]<0,100,0>作曲: Composer\n"
        L"[1000,500]<0,500,0>Line one\n"
        L"[2000,500]<0,500,0>Line two";
    auto bilingual = core::parse_krc(bilingual_krc);
    platform::windows::detail::attach_krc_translations(
        bilingual, bilingual_krc);
    expect(
        bilingual.lines.size() == 3U &&
            bilingual.lines[0].translation.empty() &&
            bilingual.lines[1].text == L"Line one" &&
            bilingual.lines[1].translation == L"T1" &&
            bilingual.lines[2].text == L"Line two" &&
            bilingual.lines[2].translation == L"T2",
        "KRC translations retain raw row alignment when credit rows are filtered");

    constexpr std::string_view qrc =
        "C90DB2E3F6940A43538B45865EB6753863C981F936A71A093B450246D48B65F0"
        "33262599ECFCF75E9EBBED19160162D3B56E50BC145B0D892B98EA6E463D5B7E"
        "DCABAE69AE1C1634DCA580EC427C9778";
    const auto qrc_plain = platform::windows::detail::decode_qrc(qrc);
    const auto qrc_lyrics = qrc_plain ? core::parse_qrc(*qrc_plain) : core::LyricsDocument{};
    expect(
        qrc_lyrics.has_word_timing && qrc_lyrics.lines.size() == 1U &&
            qrc_lyrics.lines[0].text.starts_with(L"\u817e\u8baf"),
        "QRC Triple-DES and zlib decoding preserves word-level QRC timing");

    expect(
        !platform::windows::detail::decode_krc(
            std::array<std::uint8_t, 5>{'k','r','c','1',0}).has_value() &&
            !platform::windows::detail::decode_qrc("not-hex").has_value(),
        "lyrics codecs reject truncated or malformed encrypted payloads");
}

void test_online_lyrics_live()
{
    using namespace cd404;
    std::optional<platform::windows::OnlineLyricsLookupResult> result;
    std::jthread worker([&] {
        result = platform::windows::lookup_online_lyrics(
            core::LyricsMatchQuery{
                L"INTERNET YAMERO",
                L"Aiobahn +81",
                L"INTERNET YAMERO",
                241'000,
            },
            {},
            false);
    });
    worker.join();
    const std::size_t translated_lines = result && result->lyrics
        ? static_cast<std::size_t>(std::ranges::count_if(
            result->lyrics->lines,
            [](const core::LyricLine& line) { return !line.translation.empty(); }))
        : 0U;
    expect(
        result && result->lyrics && result->lyrics->has_word_timing &&
            result->lyrics->lines.size() > 10U &&
            translated_lines > 10U &&
            (result->lyrics->source == L"QQ Music" ||
             result->lyrics->source == L"Kugou"),
        "live lyrics providers return a verified word-timed document");
}

void test_marchen_track_two_live()
{
    using namespace cd404;
    const core::LyricsMatchQuery query{
        L"深淵のマーメイド",
        L"Aiobahn",
        L"Märchen EP",
        178'560,
    };
    std::optional<platform::windows::OnlineLyricsLookupResult> result;
    std::jthread worker([&] {
        result = platform::windows::lookup_online_lyrics(
            query,
            {},
            false);
    });
    worker.join();
    expect(
        result && result->lyrics &&
            (result->lyrics->source == L"QQ Music" ||
             result->lyrics->source == L"NetEase") &&
            !result->lyrics->lines.empty() &&
            result->lyrics->lines.front().text == L"深い深い世界で",
        "Märchen track two rejects Kugou's mismatched generated subtitle");

    std::optional<platform::windows::OnlineLyricsCatalogResult> catalog;
    std::jthread catalog_worker([&] {
        catalog = platform::windows::search_online_lyrics_catalog(
            L"Aiobahn 深淵のマーメイド",
            query);
    });
    catalog_worker.join();
    expect(
        catalog && catalog->items.size() > 3U &&
            catalog->items.front().provider !=
                platform::windows::OnlineLyricsProvider::kugou &&
            catalog->items.back().provider ==
                platform::windows::OnlineLyricsProvider::kugou,
        "manual live search exposes the full catalog and keeps Kugou results last");
    if (catalog && !catalog->items.empty()) {
        std::optional<platform::windows::OnlineLyricsLookupResult> selected;
        std::jthread selected_worker([&] {
            selected = platform::windows::resolve_online_lyrics_item(
                catalog->items.front());
        });
        selected_worker.join();
        expect(
            selected && selected->lyrics && !selected->lyrics->lines.empty() &&
                selected->lyrics->lines.front().text == L"深い深い世界で",
            "manual live selection resolves the exact chosen result");
    }
}

void test_krc_translation_alignment_live()
{
    using namespace cd404;
    std::optional<platform::windows::OnlineLyricsLookupResult> result;
    std::jthread worker([&] {
        result = platform::windows::lookup_online_lyrics(
            core::LyricsMatchQuery{
                L"バンブーディスコ",
                L"Yunomi feat. TORIENA",
                L"大江戸コントローラー EP",
                196'000,
            },
            {},
            false);
    });
    worker.join();
    const auto translation_for = [&](const std::wstring_view text) {
        if (!result || !result->lyrics) {
            return std::wstring{};
        }
        const auto line = std::ranges::find(
            result->lyrics->lines, text, &core::LyricLine::text);
        return line == result->lyrics->lines.end()
            ? std::wstring{}
            : line->translation;
    };
    expect(
        result && result->lyrics && result->lyrics->source == L"Kugou" &&
            translation_for(L"今宵は月が出た") == L"今宵明月升起" &&
            translation_for(L"ニュートーキョー") == L"新东京" &&
            translation_for(L"悲しい歌が漏れる") == L"流淌出悲伤的歌谣",
        "live Kugou KRC translation rows survive filtered credits without shifting");
}

void test_wasapi_negotiation_and_fallback()
{
    using namespace cd404::platform::windows;

    expect(
        uses_event_driven_wasapi_buffering(WasapiShareMode::shared) &&
            !uses_event_driven_wasapi_buffering(WasapiShareMode::exclusive),
        "exclusive rendering polls exact available frames instead of requiring padded event packets");

    ScriptedWasapiBackend shared_backend;
    const auto shared = open_wasapi_session(shared_backend, {});
    expect(
        shared.succeeded() && shared.requested_mode == WasapiShareMode::shared &&
            shared.actual_mode == WasapiShareMode::shared &&
            !shared.fallback_attempted &&
            shared_backend.initializations ==
                std::vector{WasapiShareMode::shared},
        "shared mode is the compatible default and never reports a fallback");

    WasapiOpenOptions exclusive_options;
    exclusive_options.endpoint_id = L"stable-endpoint-id";
    exclusive_options.mode = WasapiShareMode::exclusive;
    ScriptedWasapiBackend exact_backend;
    const auto exact = open_wasapi_session(exact_backend, exclusive_options);
    expect(
        exact.succeeded() && exact.actual_mode == WasapiShareMode::exclusive &&
            exact_backend.support_checks ==
                std::vector{WasapiShareMode::exclusive} &&
            exact_backend.initializations ==
                std::vector{WasapiShareMode::exclusive},
        "explicit exclusive mode initializes only after exact format support");

    ScriptedWasapiBackend unsupported_backend;
    unsupported_backend.exclusive_support = AUDCLNT_E_UNSUPPORTED_FORMAT;
    const auto unsupported =
        open_wasapi_session(unsupported_backend, exclusive_options);
    expect(
        !unsupported.succeeded() &&
            unsupported.status == AUDCLNT_E_UNSUPPORTED_FORMAT &&
            unsupported_backend.initializations.empty() &&
            !unsupported.fallback_attempted,
        "unsupported exclusive PCM is explainable and cannot silently fall back");

    exclusive_options.allow_shared_fallback = true;
    const auto fallback =
        open_wasapi_session(unsupported_backend, exclusive_options);
    expect(
        fallback.succeeded() && fallback.fallback_attempted &&
            fallback.fallback_reason == AUDCLNT_E_UNSUPPORTED_FORMAT &&
            fallback.actual_mode == WasapiShareMode::shared &&
            unsupported_backend.initializations ==
                std::vector{WasapiShareMode::shared},
        "explicit fallback preserves the exclusive failure and reports shared mode");

    ScriptedWasapiBackend occupied_backend;
    occupied_backend.exclusive_initialize = AUDCLNT_E_DEVICE_IN_USE;
    const auto occupied =
        open_wasapi_session(occupied_backend, exclusive_options);
    expect(
        occupied.succeeded() && occupied.fallback_attempted &&
            occupied.fallback_reason == AUDCLNT_E_DEVICE_IN_USE &&
            occupied_backend.initializations == std::vector{
                WasapiShareMode::exclusive,
                WasapiShareMode::shared},
        "exclusive endpoint occupancy uses the same visible fallback policy");

    ScriptedWasapiBackend missing_backend;
    missing_backend.shared_initialize = AUDCLNT_E_DEVICE_INVALIDATED;
    WasapiOpenOptions selected_shared;
    selected_shared.endpoint_id = L"removed-endpoint";
    const auto missing = open_wasapi_session(missing_backend, selected_shared);
    expect(
        !missing.succeeded() && missing.status == AUDCLNT_E_DEVICE_INVALIDATED &&
            missing.actual_mode == WasapiShareMode::shared,
        "a disappeared selected endpoint remains a visible initialization failure");
}

void test_device_lifecycle_message_classification()
{
    using namespace cd404::platform::windows;

    DEV_BROADCAST_VOLUME media{};
    media.dbcv_size = sizeof(media);
    media.dbcv_devicetype = DBT_DEVTYP_VOLUME;
    media.dbcv_flags = DBTF_MEDIA;
    expect(
        classify_device_lifecycle_message(
            WM_DEVICECHANGE,
            DBT_DEVICEARRIVAL,
            &media) == DeviceLifecycleEvent::optical_media_changed,
        "optical volume arrival is injectable as a media-change event");

    DEV_BROADCAST_VOLUME unrelated_volume = media;
    unrelated_volume.dbcv_flags = 0;
    expect(
        classify_device_lifecycle_message(
            WM_DEVICECHANGE,
            DBT_DEVICEARRIVAL,
            &unrelated_volume) == DeviceLifecycleEvent::none,
        "an unrelated volume arrival does not interrupt CD playback");
    expect(
        classify_device_lifecycle_message(
            WM_DEVICECHANGE,
            DBT_DEVNODES_CHANGED,
            nullptr) == DeviceLifecycleEvent::none,
        "an unscoped device-node change does not interrupt CD playback");
    expect(
        classify_device_lifecycle_message(
            WM_POWERBROADCAST,
            PBT_APMSUSPEND,
            nullptr) == DeviceLifecycleEvent::suspending &&
            classify_device_lifecycle_message(
                WM_POWERBROADCAST,
                PBT_APMRESUMEAUTOMATIC,
                nullptr) == DeviceLifecycleEvent::resumed,
        "power broadcasts map to deterministic suspend and resume events");
}

void test_invalid_requests_without_device_access()
{
    using namespace cd404;

    platform::windows::CddaPlaybackEngine engine;
    const auto initial = engine.progress();
    expect(
        initial.state == audio::PlaybackState::idle &&
            initial.target_frames == 0 && initial.frames_produced == 0 &&
            initial.frames_submitted == 0 && initial.frames_rendered == 0,
        "new playback engine exposes an idle zero progress snapshot");
    expect(
        engine.request_seek(1, 0).result ==
            platform::windows::CddaSeekRequestResult::not_active,
        "an idle engine rejects seek commands without touching hardware");

    platform::windows::CddaPlaybackRequest negative_offset;
    negative_offset.offset_frames = -1;
    const auto negative_result = engine.play(negative_offset);
    expect(
        negative_result.error ==
                platform::windows::CddaPlaybackError::invalid_range &&
            negative_result.final_state == audio::PlaybackState::failed,
        "negative playback offset fails before device discovery");
    expect(
        engine.progress().state == audio::PlaybackState::failed,
        "engine publishes terminal failure state");

    platform::windows::CddaPlaybackRequest empty_duration;
    empty_duration.maximum_frames = 0;
    const auto empty_result = engine.play(empty_duration);
    expect(
        empty_result.error ==
                platform::windows::CddaPlaybackError::invalid_range &&
            empty_result.final_state == audio::PlaybackState::failed,
        "zero-length playback range is rejected");
    expect(
        empty_result.target_frames == 0 && empty_result.frames_produced == 0 &&
            empty_result.frames_submitted == 0 &&
            empty_result.frames_rendered == 0,
        "invalid request cannot publish stale progress from the prior session");
}

void test_playback_session_seek_planning()
{
    using namespace cd404;
    using namespace platform::windows;

    constexpr std::array entries{
        disc::RawTocEntry{1, 0, true},
        disc::RawTocEntry{2, 100, true},
        disc::RawTocEntry{3, 200, false},
        disc::RawTocEntry{4, 300, true},
    };
    disc::TocError error{};
    const auto toc = disc::Toc::create(entries, 400, error);
    expect(toc.has_value(), "seek planner test creates a mixed-mode TOC");
    if (!toc) {
        return;
    }

    const auto track_two = plan_cdda_session_seek(*toc, 1, 2, 2, 37);
    expect(
        track_two.result == CddaSeekRequestResult::queued &&
            track_two.stream_offset_frames ==
                100 * core::kCdSampleFramesPerSector + 37 &&
            track_two.remaining_frames ==
                100 * core::kCdSampleFramesPerSector - 37,
        "a forward track switch reuses the contiguous audio session exactly");

    const auto track_one = plan_cdda_session_seek(*toc, 1, 2, 1, 11);
    expect(
        track_one.result == CddaSeekRequestResult::queued &&
            track_one.stream_offset_frames == 11,
        "a backward track switch reuses the same audio session");
    expect(
        plan_cdda_session_seek(*toc, 1, 2, 4, 0).result ==
            CddaSeekRequestResult::outside_session,
        "a track beyond a data gap requests an explicit session fallback");
    expect(
        plan_cdda_session_seek(*toc, 1, 2, 3, 0).result ==
            CddaSeekRequestResult::invalid_track,
        "a data track can never enter the CDDA seek path");
    expect(
        plan_cdda_session_seek(
            *toc,
            1,
            2,
            2,
            100 * core::kCdSampleFramesPerSector).result ==
            CddaSeekRequestResult::invalid_range,
        "a seek at the track end is rejected instead of reading the next track");

    LatestCddaSeekCommand commands;
    const auto first = commands.queue(1, 10);
    const auto latest = commands.queue(2, 20);
    const auto taken = commands.take_latest();
    expect(
        first.sequence != 0 && latest.sequence > first.sequence && taken &&
            taken->sequence == latest.sequence && taken->track_number == 2 &&
            taken->offset_frames == 20 && !commands.has_pending(),
        "rapid seek commands coalesce to the newest monotonic sequence");
    commands.reset();
    expect(
        commands.queue(1, 0).sequence == 1,
        "a new playback session resets the seek command sequence");

    constexpr std::uint64_t kSeed = 0xCD4047A11ULL;
    std::mt19937_64 random(kSeed);
    LatestCddaSeekCommand random_commands;
    std::uint64_t previous_sequence{};
    bool all_exact = true;
    for (std::size_t iteration = 0; iteration < 4'096 && all_exact; ++iteration) {
        const auto track = static_cast<std::uint8_t>(1 + random() % 2);
        const auto track_frames =
            static_cast<core::SampleFrame>(100 * core::kCdSampleFramesPerSector);
        const auto offset = static_cast<core::SampleFrame>(
            random() % static_cast<std::uint64_t>(track_frames));
        const auto plan = plan_cdda_session_seek(*toc, 1, 2, track, offset);
        const auto command = random_commands.queue(track, offset);
        const auto latest_command = random_commands.take_latest();

        const auto expected_stream_offset =
            static_cast<core::SampleFrame>((track - 1) * track_frames) + offset;
        all_exact = plan.result == CddaSeekRequestResult::queued &&
            plan.stream_offset_frames == expected_stream_offset &&
            plan.remaining_frames == 2 * track_frames - expected_stream_offset &&
            command.sequence > previous_sequence && latest_command &&
            latest_command->sequence == command.sequence &&
            latest_command->track_number == track &&
            latest_command->offset_frames == offset;
        if (!all_exact) {
            std::cerr << "track-switch seed=0xCD4047A11 iteration="
                      << iteration << " track=" << static_cast<unsigned int>(track)
                      << " offset=" << offset << '\n';
        }
        previous_sequence = command.sequence;
    }
    expect(
        all_exact,
        "seed 0xCD4047A11 random track switches preserve session offsets and newest-command ordering");
}

void test_volume_control_boundaries()
{
    using cd404::platform::windows::CddaPlaybackEngine;

    CddaPlaybackEngine engine;
    engine.set_volume(0.42F);
    expect(
        engine.volume() > 0.419F && engine.volume() < 0.421F,
        "playback engine retains an in-range volume");
    engine.set_volume(-1.0F);
    expect(engine.volume() == 0.0F, "playback engine clamps volume to mute");
    engine.set_volume(2.0F);
    expect(engine.volume() == 1.0F, "playback engine clamps volume to unity");
}

void test_listenbrainz_payload_contract()
{
    using namespace cd404;
    using namespace winrt::Windows::Data::Json;

    expect(
        platform::windows::is_listenbrainz_token_format_valid(
            L"01234567-89ab-cdef-0123-456789abcdef") &&
            platform::windows::is_listenbrainz_token_format_valid(
                L"Abc123_~") &&
            !platform::windows::is_listenbrainz_token_format_valid(L"") &&
            !platform::windows::is_listenbrainz_token_format_valid(
                L"token with space") &&
            !platform::windows::is_listenbrainz_token_format_valid(
                L"token\r\nInjected: header"),
        "ListenBrainz token validation accepts punctuation but rejects whitespace and header injection");

    listenbrainz::Submission playing_now;
    playing_now.type = listenbrainz::SubmissionType::playing_now;
    playing_now.listened_at = 1'700'000'000;
    playing_now.track_name = L"Track";
    playing_now.artist_name = L"Artist";
    playing_now.release_name = L"Album";
    playing_now.duration_milliseconds = 123'000;
    playing_now.track_number = 2;
    playing_now.recording_mbid = L"recording-id";
    playing_now.release_mbid = L"release-id";
    playing_now.release_group_mbid = L"release-group-id";
    playing_now.track_mbid = L"track-id";
    playing_now.artist_mbids = {L"artist-id-1", L"artist-id-2"};

    const auto now_root = JsonObject::Parse(
        platform::windows::build_listenbrainz_payload(playing_now));
    const auto now_listen = now_root.GetNamedArray(L"payload")
                                .GetObjectAt(0);
    expect(
        now_root.GetNamedString(L"listen_type") == L"playing_now" &&
            !now_listen.HasKey(L"listened_at"),
        "playing_now payload omits listened_at as required by ListenBrainz");
    expect(
        now_listen.GetNamedObject(L"track_metadata")
                  .GetNamedString(L"track_name") == L"Track",
        "playing_now payload contains required track metadata");
    const auto now_additional = now_listen.GetNamedObject(L"track_metadata")
                                    .GetNamedObject(L"additional_info");
    expect(
        now_additional.GetNamedString(L"tracknumber") == L"2" &&
            now_additional.GetNamedString(L"recording_mbid") == L"recording-id" &&
            now_additional.GetNamedString(L"release_mbid") == L"release-id" &&
            now_additional.GetNamedString(L"release_group_mbid") ==
                L"release-group-id" &&
            now_additional.GetNamedString(L"track_mbid") == L"track-id" &&
            now_additional.GetNamedArray(L"artist_mbids").Size() == 2 &&
            now_additional.GetNamedString(L"submission_client_version") ==
                cd404::core::kVersionWide,
        "ListenBrainz payload carries exact identities and the central release version");

    auto single = playing_now;
    single.type = listenbrainz::SubmissionType::single;
    single.duration_played_seconds = 61;
    const auto single_root = JsonObject::Parse(
        platform::windows::build_listenbrainz_payload(single));
    const auto single_listen = single_root.GetNamedArray(L"payload")
                                   .GetObjectAt(0);
    const auto additional = single_listen.GetNamedObject(L"track_metadata")
                                .GetNamedObject(L"additional_info");
    expect(
        single_root.GetNamedString(L"listen_type") == L"single" &&
            single_listen.GetNamedNumber(L"listened_at") == 1'700'000'000.0,
        "single payload includes the playback-start Unix timestamp");
    expect(
        additional.GetNamedNumber(L"duration_ms") == 123'000.0 &&
            additional.GetNamedNumber(L"duration_played") == 61.0,
        "single payload includes duration diagnostics");
}

void test_listenbrainz_queue_restart_and_account_isolation()
{
    using namespace cd404::platform::windows;
    const auto path = std::filesystem::temp_directory_path() /
        std::format(L"cd404-listenbrainz-{}.db", GetCurrentProcessId());
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.wstring() + L"-wal", error);
    std::filesystem::remove(path.wstring() + L"-shm", error);
    {
        ListenBrainzQueue queue(path);
        expect(
            queue.available() && queue.schema_version() == 3,
            "ListenBrainz queue creates the current SQLite schema");
        expect(
            queue.enqueue("owner-a", "session-a", "payload-a") &&
                queue.enqueue("owner-b", "session-b", "payload-b") &&
                queue.pending_count("owner-a") == 1U &&
                queue.pending_count("owner-b") == 1U,
            "different token fingerprints have isolated pending queues");
    }
    {
        ListenBrainzQueue queue(path);
        expect(
            queue.pending_count("owner-a") == 1U &&
                queue.pending_count("owner-b") == 1U,
            "pending listens recover after process-style queue restart");
        expect(
            queue.clear_owner("owner-a") &&
                queue.pending_count("owner-a") == 0U &&
                queue.pending_count("owner-b") == 1U,
            "queue cleanup deletes only the current account");
    }
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.wstring() + L"-wal", error);
    std::filesystem::remove(path.wstring() + L"-shm", error);

    const auto legacy_path = std::filesystem::temp_directory_path() /
        std::format(L"cd404-listenbrainz-legacy-{}.db", GetCurrentProcessId());
    std::filesystem::remove(legacy_path, error);
    sqlite3* legacy{};
    const bool legacy_created = sqlite3_open16(legacy_path.c_str(), &legacy) == SQLITE_OK &&
        sqlite3_exec(
            legacy,
            "CREATE TABLE listen_queue ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL UNIQUE,"
            "payload TEXT NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,"
            "next_attempt INTEGER NOT NULL DEFAULT 0, failed INTEGER NOT NULL DEFAULT 0,"
            "last_status INTEGER NOT NULL DEFAULT 0, last_error INTEGER NOT NULL DEFAULT 0);"
            "INSERT INTO listen_queue(session_id,payload) VALUES('legacy','payload');"
            "PRAGMA user_version=1;",
            nullptr,
            nullptr,
            nullptr) == SQLITE_OK;
    if (legacy != nullptr) {
        sqlite3_close(legacy);
    }
    {
        ListenBrainzQueue migrated(legacy_path);
        expect(
            legacy_created && migrated.available() && migrated.schema_version() == 3 &&
                migrated.pending_count("") == 1U,
            "SQLite v1 queue migrates in place without losing legacy pending listens");
    }
    std::filesystem::remove(legacy_path, error);
    std::filesystem::remove(legacy_path.wstring() + L"-wal", error);
    std::filesystem::remove(legacy_path.wstring() + L"-shm", error);
}

void test_listenbrainz_fake_http_failures()
{
    using namespace cd404;
    using namespace platform::windows;

    const auto exercise = [](
                              HttpResponse submission_response,
                              const std::wstring_view suffix,
                              const ListenBrainzState expected_state) {
        const auto path = std::filesystem::temp_directory_path() /
            (L"cd404-listenbrainz-http-" + std::wstring(suffix) + L".db");
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.wstring() + L"-wal", error);
        std::filesystem::remove(path.wstring() + L"-shm", error);
        auto client = std::make_shared<ScriptedHttpClient>();
        client->validation.status = 200;
        constexpr std::string_view validation_json =
            R"({"valid":true,"user_name":"synthetic"})";
        client->validation.body.assign(
            validation_json.begin(),
            validation_json.end());
        client->submission = std::move(submission_response);

        ListenBrainzStatus final_status;
        {
            ListenBrainzReporter reporter(ListenBrainzReporterOptions{
                client,
                path,
                L"synthetic-token",
            });
            const auto validation_deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < validation_deadline &&
                   reporter.status().state != ListenBrainzState::ready) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            listenbrainz::Submission listen;
            listen.type = listenbrainz::SubmissionType::single;
            listen.listened_at = 1'700'000'000;
            listen.track_name = L"Track";
            listen.artist_name = L"Artist";
            listen.duration_milliseconds = 60'000;
            reporter.submit(listen);
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                final_status = reporter.status();
                if (client->post_calls.load(std::memory_order_relaxed) != 0U &&
                    final_status.state == expected_state) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.wstring() + L"-wal", error);
        std::filesystem::remove(path.wstring() + L"-shm", error);
        return final_status;
    };

    HttpResponse unauthorized;
    unauthorized.status = 401;
    const auto unauthorized_status = exercise(
        unauthorized,
        L"401",
        ListenBrainzState::unauthorized);
    expect(
        unauthorized_status.state == ListenBrainzState::unauthorized &&
            unauthorized_status.pending_listens == 1U,
        "fake HTTP 401 pauses the durable queue without deleting the listen");

    HttpResponse limited;
    limited.status = 429;
    limited.has_rate_limit_reset = true;
    limited.rate_limit_reset_seconds = 17;
    const auto limited_status = exercise(
        limited,
        L"429",
        ListenBrainzState::retry_wait);
    expect(
        limited_status.state == ListenBrainzState::retry_wait &&
            limited_status.retry_after_seconds == 17U &&
            limited_status.pending_listens == 1U,
        "fake HTTP 429 honors the server reset interval and retains the queue");

    HttpResponse offline;
    offline.system_error = ERROR_NETWORK_UNREACHABLE;
    const auto offline_status = exercise(
        offline,
        L"offline",
        ListenBrainzState::retry_wait);
    expect(
        offline_status.state == ListenBrainzState::retry_wait &&
            offline_status.retry_after_seconds > 0U &&
            offline_status.pending_listens == 1U,
        "fake network failure schedules retry without public internet access");

    HttpResponse server_error;
    server_error.status = 503;
    const auto server_status = exercise(
        server_error,
        L"503",
        ListenBrainzState::retry_wait);
    expect(
        server_status.state == ListenBrainzState::retry_wait &&
            server_status.pending_listens == 1U,
        "fake HTTP 5xx remains retryable and durable");
}

void test_cddb_configuration_contract()
{
    using namespace cd404;
    using namespace cd404::platform::windows;

    const auto default_server = parse_cddb_server(L"gnudb.gnudb.org");
    expect(
        default_server && default_server->secure &&
            default_server->host == L"gnudb.gnudb.org" &&
            default_server->port == 443U &&
            default_server->query_path == L"/~cddb/cddb.cgi" &&
            default_server->submit_path == L"/~cddb/submit.cgi",
        "CDDB default host expands to the secure GnuDB CGI endpoints");

    const auto custom_server = parse_cddb_server(
        L"http://metadata.example.test:8080/freedb/query.cgi");
    expect(
        custom_server && !custom_server->secure &&
            custom_server->host == L"metadata.example.test" &&
            custom_server->port == 8080U &&
            custom_server->query_path == L"/freedb/query.cgi" &&
            custom_server->submit_path == L"/freedb/submit.cgi",
        "CDDB custom origin preserves scheme, port and derives submit endpoint");
    expect(
        !parse_cddb_server(L"ftp://example.test") &&
            !parse_cddb_server(L"https://user:secret@example.test") &&
            !parse_cddb_server(L"https://example.test/path?query=1"),
        "CDDB server parser rejects unsupported or ambiguous endpoints");
    expect(
        is_valid_cddb_email(L"listener@example.test") &&
            !is_valid_cddb_email(L"listener") &&
            !is_valid_cddb_email(L"a@b") &&
            !is_valid_cddb_email(L"a@b.test\r\nInjected: value"),
        "CDDB submission email validation rejects missing domains and header injection");

    const auto direct_match = parse_gnudb_query_response(
        "200 data 6506c88d Artist / Album\r\n");
    const auto exact_matches = parse_gnudb_query_response(
        "210 Found exact matches, list follows\r\n"
        "data 6506c88d Artist / Album\r\n.\r\n");
    const auto inexact_matches = parse_gnudb_query_response(
        "211 Found inexact matches, list follows\r\n"
        "data 6506c88d Artist / Album\r\n.\r\n");
    const auto rejected = parse_gnudb_query_response(
        "500 Unknown developer email for CD404WindowsAudioCDPlayer v0.2.0\r\n");
    expect(
        direct_match.protocol_status == 200U && direct_match.match &&
            direct_match.match->gnucdid == "6506c88d" &&
            exact_matches.protocol_status == 210U && exact_matches.match &&
            inexact_matches.protocol_status == 211U && inexact_matches.match &&
            rejected.protocol_status == 500U && !rejected.match &&
            rejected.message.find("Unknown developer email") != std::string::npos,
        "GnuDB query parser accepts gnucdid exact and inexact matches and preserves errors");

    constexpr std::array entries{
        disc::RawTocEntry{1, 0, true},
        disc::RawTocEntry{2, 10'000, true},
    };
    disc::TocError toc_error{};
    const auto toc = disc::Toc::create(entries, 20'000, toc_error);
    const auto invalid_identity = toc
        ? lookup_gnudb(*toc, {L"gnudb.gnudb.org", L"not-an-email"})
        : GnudbLookupResult{};
    expect(
        !invalid_identity.metadata &&
            invalid_identity.system_error == ERROR_INVALID_PARAMETER,
        "GnuDB lookup rejects a malformed explicit user identity email");
    disc::GnudbSubmissionMetadataUtf8 metadata;
    metadata.album_title = "Album";
    metadata.album_artist = "Artist";
    metadata.category = "rock";
    metadata.year = "2026";
    metadata.track_titles = {"First Song", "Second Song"};
    metadata.track_artists = {"Artist", "Artist"};
    metadata.user_edited = true;
    const auto test_request = toc
        ? build_cddb_submission_request(
              {L"gnudb.gnudb.org", L"listener@example.test"},
              metadata,
              *toc,
              CddbSubmissionMode::test)
        : std::nullopt;
    expect(
        test_request &&
            test_request->headers.find(L"Category: rock\r\n") != std::wstring::npos &&
            test_request->headers.find(L"User-Email: listener@example.test\r\n") !=
                std::wstring::npos &&
            test_request->headers.find(L"Submit-Mode: test\r\n") !=
                std::wstring::npos &&
            test_request->headers.find(L"Charset: UTF-8\r\n") !=
                std::wstring::npos &&
            std::string(test_request->body.begin(), test_request->body.end())
                .find("DTITLE=Artist / Album\n") != std::string::npos,
        "CDDB test submission request carries protocol headers and raw xmcd body");
    const auto submit_request = toc
        ? build_cddb_submission_request(
              {L"https://metadata.example.test", L"listener@example.test"},
              metadata,
              *toc,
              CddbSubmissionMode::submit)
        : std::nullopt;
    expect(
        submit_request &&
            submit_request->headers.find(L"Submit-Mode: submit\r\n") !=
                std::wstring::npos,
        "CDDB formal submission request is distinct from server-side test mode");
}

void test_system_media_controls_safe_fallback()
{
    cd404::platform::windows::SystemMediaControls controls;
    expect(
        !controls.initialize(nullptr, {}),
        "SMTC rejects a null HWND without exposing a partial session");
    expect(!controls.available(), "SMTC remains unavailable after null initialization");
    controls.clear();
}

void test_system_media_controls_with_window()
{
    using namespace cd404::platform::windows;

    constexpr wchar_t class_name[] = L"CD404.SystemMediaControlsTest";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    const ATOM atom = RegisterClassW(&window_class);
    expect(
        atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
        "SMTC test window class registers");
    const HWND window = CreateWindowExW(
        0,
        class_name,
        L"CD.404 SMTC test",
        WS_OVERLAPPED,
        0,
        0,
        320,
        200,
        nullptr,
        nullptr,
        window_class.hInstance,
        nullptr);
    expect(window != nullptr, "SMTC test creates a real top-level HWND");
    if (window == nullptr) {
        return;
    }

    SystemMediaControls controls;
    const bool initialized = controls.initialize(window, [](const auto&) {});
    expect(initialized && controls.available(), "SMTC binds to a desktop HWND");
    if (initialized) {
        controls.update_metadata(L"Track", L"Artist", L"Album");
        controls.update_timeline(30'000, 180'000);
        controls.set_playback_state(SystemMediaPlaybackState::playing);
        controls.set_playback_state(SystemMediaPlaybackState::paused);
        controls.clear();
    }
    DestroyWindow(window);
    UnregisterClassW(class_name, window_class.hInstance);
}

void test_user_settings_round_trip()
{
    using namespace cd404;
    using namespace platform::windows;

    constexpr std::array entries{
        disc::RawTocEntry{1, 0, true},
        disc::RawTocEntry{2, 10'000, true},
    };
    disc::TocError toc_error{};
    const auto toc = disc::Toc::create(entries, 20'000, toc_error);
    expect(toc.has_value(), "settings test creates a valid TOC");
    if (!toc) {
        return;
    }
    const std::wstring disc_key = make_disc_settings_key(*toc);
    expect(
        !disc_key.empty() && disc_key == make_disc_settings_key(*toc),
        "TOC settings key is stable and non-empty");

    UserSettings source;
    source.volume = 0.37F;
    source.listenbrainz_reporting_enabled = false;
    source.audio_endpoint_id = L"endpoint-{stable-id}";
    source.audio_output_engine = AudioOutputEngine::wasapi;
    source.audio_exclusive_mode = true;
    source.audio_allow_shared_fallback = true;
    source.cddb_enabled = false;
    source.cddb_server = L"https://metadata.example.test";
    source.cddb_email = L"listener@example.test";
    source.playback_positions[disc_key] = {2, 123'456};
    source.lyric_offsets_ms[disc_key][2] = 650;
    source.metadata_overrides[disc_key] = SavedDiscMetadata{
        L"Edited Album",
        L"Edited Artist",
        L"classical",
        L"2025",
        {L"First", L"Second"},
        {L"Artist A", L"Artist B"},
        4,
    };
    const std::wstring json = encode_user_settings(source);
    const UserSettings decoded = decode_user_settings(json);
    const auto position = decoded.playback_positions.find(disc_key);
    const auto offsets = decoded.lyric_offsets_ms.find(disc_key);
    expect(
        decoded.volume > 0.369F && decoded.volume < 0.371F &&
            !decoded.listenbrainz_reporting_enabled &&
            decoded.audio_endpoint_id == source.audio_endpoint_id &&
            decoded.audio_output_engine == AudioOutputEngine::wasapi &&
            decoded.audio_exclusive_mode &&
            decoded.audio_allow_shared_fallback && !decoded.cddb_enabled &&
            decoded.cddb_server == L"https://metadata.example.test" &&
            decoded.cddb_email == L"listener@example.test",
        "user settings JSON round-trips persistent options");
    expect(
        position != decoded.playback_positions.end() &&
            position->second.track_number == 2 &&
            position->second.offset_frames == 123'456,
        "user settings JSON round-trips per-disc playback position");
    expect(
        offsets != decoded.lyric_offsets_ms.end() &&
            offsets->second.find(2) != offsets->second.end() &&
            offsets->second.at(2) == 650,
        "user settings JSON round-trips per-track lyric offsets");
    expect(
        json.find(L"Token") == std::wstring::npos &&
            json.find(L"token") == std::wstring::npos &&
            json.find(L"\"audio_output_engine\":\"wasapi\"") !=
                std::wstring::npos,
        "settings JSON never contains a ListenBrainz token");
    const auto metadata = decoded.metadata_overrides.find(disc_key);
    expect(
        metadata != decoded.metadata_overrides.end() &&
            metadata->second.album_title == L"Edited Album" &&
            metadata->second.track_titles ==
                std::vector<std::wstring>{L"First", L"Second"} &&
            metadata->second.category == L"classical" &&
            metadata->second.revision == 4,
        "user settings JSON round-trips edited disc metadata");

    const UserSettings defaults = decode_user_settings(L"not json");
    expect(
        defaults.volume == 1.0F && defaults.listenbrainz_reporting_enabled &&
            defaults.audio_endpoint_id.empty() &&
            defaults.audio_output_engine == AudioOutputEngine::wasapi &&
            !defaults.audio_exclusive_mode &&
            !defaults.audio_allow_shared_fallback && defaults.cddb_enabled &&
            defaults.cddb_server == kDefaultCddbServer,
        "malformed settings keep shared default output and safe volume");
}

void test_musicbrainz_exact_lookup_paths()
{
    using namespace cd404;
    constexpr std::array entries{
        disc::RawTocEntry{1, 0, true},
        disc::RawTocEntry{2, 15'213, true},
        disc::RawTocEntry{3, 32'164, true},
        disc::RawTocEntry{4, 46'442, true},
        disc::RawTocEntry{5, 63'264, true},
        disc::RawTocEntry{6, 80'339, true},
    };
    disc::TocError error{};
    const auto toc = disc::Toc::create(entries, 95'312, error);
    expect(toc.has_value(), "MusicBrainz lookup test TOC is valid");
    if (!toc) {
        return;
    }
    const auto paths = platform::windows::make_musicbrainz_lookup_paths(*toc);
    expect(
        paths && paths->exact.find(
            L"/ws/2/discid/49HHV7Eb8UKF3aQiNmu1GR8vKTY-?") == 0 &&
            paths->exact.find(L"toc=") == std::wstring::npos &&
            paths->fuzzy.find(L"/ws/2/discid/-?toc=1+6+95462+") == 0,
        "MusicBrainz performs exact Disc ID lookup before TOC fuzzy fallback");
}

void test_musicbrainz_multiple_release_parsing()
{
    using namespace cd404::platform::windows;
    const std::string xml =
        "<metadata><release-list>"
        "<release id='release-a'><title>Album A</title>"
        "<artist-credit><name-credit><artist id='artist-a'><name>Artist A</name>"
        "</artist></name-credit></artist-credit><release-group id='group-a'/>"
        "<medium-list><medium><track-list count='1'><track id='track-a'>"
        "<length>60000</length><recording id='recording-a'><title>Song A</title>"
        "<artist-credit><name-credit><artist id='artist-a'><name>Artist A</name>"
        "</artist></name-credit></artist-credit></recording></track></track-list>"
        "</medium></medium-list></release>"
        "<release id='release-b'><title>Album B</title>"
        "<artist-credit><name-credit><artist id='artist-b'><name>Artist B</name>"
        "</artist></name-credit></artist-credit><release-group id='group-b'/>"
        "<medium-list><medium><track-list count='1'><track id='track-b'>"
        "<length>60500</length><recording id='recording-b'><title>Song B</title>"
        "<artist-credit><name-credit><artist id='artist-b'><name>Artist B</name>"
        "</artist></name-credit></artist-credit></recording></track></track-list>"
        "</medium></medium-list></release>"
        "</release-list></metadata>";
    const std::vector<std::uint8_t> body(xml.begin(), xml.end());
    constexpr std::array<std::uint64_t, 1> expected_lengths{60'000};
    const auto candidates = parse_musicbrainz_candidates(
        body,
        expected_lengths,
        true);
    expect(
        candidates.size() == 2U &&
            candidates[0].release_id == L"release-a" &&
            candidates[1].release_id == L"release-b" &&
            candidates[0].track_titles == std::vector{std::wstring(L"Song A")} &&
            candidates[0].exact_disc_id_match &&
            candidates[1].exact_disc_id_match,
        "MusicBrainz parser retains all duration-compatible exact release candidates");
}

void test_musicbrainz_content_association_reorders_recordings()
{
    using namespace cd404::platform::windows;
    MusicBrainzContentQuery query;
    query.album_title = L"Re:BPM15Q by TeddyLoid";
    query.album_artist = L"TeddyLoid";
    query.track_titles = {
        L"Overture",
        L"はくちゅーむ",
        L"BPM15Q!",
        L"ANNARI",
        L"すれ違い…",
        L"コクハク…",
        L"カタオモイ…",
        L"ドキドキ…",
    };
    query.track_lengths_milliseconds = {
        111'000U, 283'000U, 239'000U, 202'000U,
        274'000U, 170'000U, 210'000U, 250'000U,
    };

    MusicBrainzMetadata official;
    official.release_id = L"reference-release";
    official.release_group_id = L"reference-group";
    official.album_title = L"Re:BPM15Q by TeddyLoid";
    official.album_artist = L"BPM15Q";
    official.track_titles = {
        L"Overture",
        L"BPM15Q! (TeddyLoid Remix)",
        L"HANNARI (TeddyLoid Remix)",
        L"すれ違い…",
        L"コクハク…",
        L"カタオモイ…",
        L"ドキドキ…",
        L"はくちゅーむ",
    };
    official.track_lengths_milliseconds = {
        111'000U, 239'000U, 202'000U, 274'000U,
        170'000U, 210'000U, 250'000U, 283'000U,
    };
    official.track_ids = {
        L"track-1", L"track-2", L"track-3", L"track-4",
        L"track-5", L"track-6", L"track-7", L"track-8",
    };
    official.recording_ids = {
        L"recording-1", L"recording-2", L"recording-3", L"recording-4",
        L"recording-5", L"recording-6", L"recording-7", L"recording-8",
    };
    official.track_artists.assign(8U, L"BPM15Q");
    official.track_artist_ids.assign(8U, {L"artist-bpm15q"});

    const std::array candidates{official};
    const auto matched = match_musicbrainz_content(query, candidates);
    expect(
        matched && matched->content_match &&
            matched->release_id == L"reference-release" &&
            matched->recording_ids == std::vector<std::wstring>{
                L"recording-1", L"recording-8", L"recording-2", L"recording-3",
                L"recording-4", L"recording-5", L"recording-6", L"recording-7",
            } &&
            matched->track_ids.empty(),
        "MusicBrainz content association maps reordered custom discs without "
        "claiming release-specific track identities");

    MusicBrainzMetadata conflicting = official;
    conflicting.release_id = L"different-release";
    conflicting.recording_ids[0] = L"different-recording";
    const std::array ambiguous{official, conflicting};
    expect(
        !match_musicbrainz_content(query, ambiguous),
        "MusicBrainz content association rejects equally strong releases with "
        "different recording identities");

    official.track_lengths_milliseconds[7] += 30'000U;
    const std::array wrong_duration{official};
    expect(
        !match_musicbrainz_content(query, wrong_duration),
        "MusicBrainz content association rejects a title-only match with incompatible audio");
}

void test_musicbrainz_content_release_json_parsing()
{
    using namespace cd404::platform::windows;
    const std::string json = R"json(
        {"id":"dfe4a55b-d323-41c8-9d31-4e3347efd21c",
         "title":"Re:BPM15Q by TeddyLoid",
         "release-group":{"id":"42505aeb-5d69-4fe2-8e10-c96434c54403"},
         "artist-credit":[{"name":"BPM15Q","artist":{"id":"artist-release","name":"BPM15Q"}}],
         "media":[{"track-count":2,"tracks":[
           {"id":"track-1","title":"Overture","length":110894,
            "artist-credit":[{"name":"BPM15Q","artist":{"id":"artist-track","name":"BPM15Q"}}],
            "recording":{"id":"recording-1","title":"Overture","length":110894}},
           {"id":"track-2","title":"BPM15Q! (TeddyLoid Remix)","length":238588,
            "recording":{"id":"recording-2","title":"BPM15Q! (TeddyLoid Remix)","length":238588,
                         "artist-credit":[{"name":"BPM15Q","artist":{"id":"artist-track","name":"BPM15Q"}}]}}
         ]}]}
    )json";
    const std::vector<std::uint8_t> body(json.begin(), json.end());
    const auto parsed = parse_musicbrainz_content_release(body, 2U);
    expect(
        parsed.size() == 1U &&
            parsed[0].release_id == L"dfe4a55b-d323-41c8-9d31-4e3347efd21c" &&
            parsed[0].release_group_id ==
                L"42505aeb-5d69-4fe2-8e10-c96434c54403" &&
            parsed[0].track_lengths_milliseconds ==
                std::vector<std::uint64_t>{110'894U, 238'588U} &&
            parsed[0].recording_ids == std::vector<std::wstring>{
                L"recording-1", L"recording-2"} &&
            parsed[0].track_artist_ids[1] ==
                std::vector<std::wstring>{L"artist-track"},
        "MusicBrainz content parser accepts the real release JSON field layout");
}

void test_metadata_revision_selection_and_cache()
{
    using namespace cd404::platform::windows;

    SourcedMetadataValue title{L"CD title", MetadataSource::cd_text};
    expect(
        !merge_metadata_value(title, L"Catalog title", MetadataSource::itunes) &&
            title.value == L"CD title" && title.source == MetadataSource::cd_text,
        "lower-priority online metadata cannot replace CD-TEXT");
    revise_metadata_value(title, L"My corrected title");
    expect(
        !merge_metadata_value(title, L"Remote refresh", MetadataSource::musicbrainz) &&
            title.value == L"My corrected title" &&
            title.source == MetadataSource::user,
        "refresh never overwrites a user revision");

    const std::vector<MetadataReleaseCandidate> candidates{
        {L"release-a", L"First", L"Artist"},
        {L"release-b", L"Second", L"Artist"},
    };
    expect(
        select_metadata_candidate(candidates, L"release-b") == 1U &&
            select_metadata_candidate(candidates, L"missing") == 0U,
        "remembered release selection wins and missing choices safely use first candidate");

    MetadataCacheEntry entry;
    entry.disc_key = L"deadbeef";
    entry.selected_release_id = L"release-b";
    entry.updated_unix_seconds = 1'700'000'000;
    entry.metadata.album_title = title;
    entry.metadata.album_artist = {L"Artist", MetadataSource::musicbrainz};
    entry.metadata.tracks.push_back({
        {L"Track", MetadataSource::user},
        {L"Performer", MetadataSource::cd_text},
    });
    const std::wstring encoded = encode_metadata_cache(entry);
    const auto decoded = decode_metadata_cache(encoded);
    expect(
        decoded && decoded->disc_key == entry.disc_key &&
            decoded->selected_release_id == L"release-b" &&
            decoded->metadata.album_title.source == MetadataSource::user &&
            decoded->metadata.tracks.size() == 1U &&
            decoded->metadata.tracks[0].artist.source == MetadataSource::cd_text,
        "versioned metadata cache round-trips field-level provenance and revisions");
    expect(
        metadata_cache_is_fresh(*decoded, 1'700'000'100, 3'600) &&
            !metadata_cache_is_fresh(*decoded, 1'700'004'000, 3'600) &&
            !decode_metadata_cache(L"{not-json"),
        "cache freshness and corrupt-cache fallback are deterministic");
    const auto migrated = decode_metadata_cache(
        L"{\"disc_key\":\"a1b2\",\"album_title\":\"Legacy\","
        L"\"album_artist\":\"Artist\",\"tracks\":[{\"title\":\"T\","
        L"\"artist\":\"A\"}]}");
    expect(
        migrated && migrated->metadata.album_title.value == L"Legacy" &&
            migrated->metadata.album_title.source == MetadataSource::unknown &&
            migrated->metadata.tracks.size() == 1U,
        "legacy unversioned metadata cache migrates to explicit unknown provenance");
}

void test_diagnostic_names()
{
    using namespace cd404;

    constexpr std::array states{
        audio::PlaybackState::idle,
        audio::PlaybackState::opening,
        audio::PlaybackState::buffering,
        audio::PlaybackState::playing,
        audio::PlaybackState::paused,
        audio::PlaybackState::draining,
        audio::PlaybackState::stopping,
        audio::PlaybackState::completed,
        audio::PlaybackState::cancelled,
        audio::PlaybackState::failed,
    };
    for (const auto state : states) {
        expect(
            std::string_view(audio::to_string(state)) != "unknown",
            "every playback state has a diagnostic name");
    }

    constexpr std::array errors{
        platform::windows::CddaPlaybackError::none,
        platform::windows::CddaPlaybackError::already_running,
        platform::windows::CddaPlaybackError::no_ready_audio_cd,
        platform::windows::CddaPlaybackError::source_open_failed,
        platform::windows::CddaPlaybackError::invalid_stream,
        platform::windows::CddaPlaybackError::invalid_range,
        platform::windows::CddaPlaybackError::output_open_failed,
        platform::windows::CddaPlaybackError::invalid_endpoint_buffer,
        platform::windows::CddaPlaybackError::read_failed,
        platform::windows::CddaPlaybackError::output_failed,
        platform::windows::CddaPlaybackError::endpoint_underrun,
        platform::windows::CddaPlaybackError::incomplete,
        platform::windows::CddaPlaybackError::cancelled,
    };
    for (const auto error : errors) {
        expect(
            std::string_view(platform::windows::to_string(error)) != "unknown",
            "every playback error has a diagnostic name");
    }

    constexpr std::array seek_results{
        platform::windows::CddaSeekRequestResult::queued,
        platform::windows::CddaSeekRequestResult::not_active,
        platform::windows::CddaSeekRequestResult::invalid_track,
        platform::windows::CddaSeekRequestResult::invalid_range,
        platform::windows::CddaSeekRequestResult::outside_session,
    };
    for (const auto result : seek_results) {
        expect(
            std::string_view(platform::windows::to_string(result)) != "unknown",
            "every seek request result has a diagnostic name");
    }

    constexpr std::array listenbrainz_states{
        platform::windows::ListenBrainzState::disabled,
        platform::windows::ListenBrainzState::token_missing,
        platform::windows::ListenBrainzState::validating,
        platform::windows::ListenBrainzState::ready,
        platform::windows::ListenBrainzState::submitting,
        platform::windows::ListenBrainzState::retry_wait,
        platform::windows::ListenBrainzState::unauthorized,
        platform::windows::ListenBrainzState::error,
    };
    for (const auto state : listenbrainz_states) {
        expect(
            std::wstring_view(platform::windows::to_string(state)) != L"未知",
            "every ListenBrainz state has a user-facing name");
    }
}

void test_hardware_cancellation()
{
    using namespace cd404;

    platform::windows::CddaPlaybackEngine engine;
    platform::windows::CddaPlaybackRequest request;
    request.maximum_frames = 30 * core::kCdSampleFramesPerSecond;
    std::optional<platform::windows::CddaPlaybackResult> result;
    std::thread playback([&] { result = engine.play(request); });

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = engine.progress().state;
        if (state == audio::PlaybackState::playing) {
            break;
        }
        if (state == audio::PlaybackState::completed ||
            state == audio::PlaybackState::cancelled ||
            state == audio::PlaybackState::failed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    engine.request_stop();
    playback.join();
    expect(
        result && result->error ==
                platform::windows::CddaPlaybackError::cancelled &&
            result->final_state == audio::PlaybackState::cancelled,
        "real playback request stops through the cooperative cancellation path");
}

void test_hardware_pause_resume()
{
    using namespace cd404;

    platform::windows::CddaPlaybackEngine engine;
    platform::windows::CddaPlaybackRequest request;
    request.maximum_frames = 30 * core::kCdSampleFramesPerSecond;
    std::optional<platform::windows::CddaPlaybackResult> result;
    std::thread playback([&] { result = engine.play(request); });

    const auto playing_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < playing_deadline &&
           engine.progress().state != audio::PlaybackState::playing) {
        const auto state = engine.progress().state;
        if (state == audio::PlaybackState::completed ||
            state == audio::PlaybackState::cancelled ||
            state == audio::PlaybackState::failed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    engine.request_pause();
    const auto pause_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < pause_deadline &&
           engine.progress().state != audio::PlaybackState::paused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto paused_progress = engine.progress();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    const auto still_paused = engine.progress();
    expect(
        paused_progress.state == audio::PlaybackState::paused &&
            still_paused.state == audio::PlaybackState::paused &&
            still_paused.frames_rendered == paused_progress.frames_rendered,
        "real WASAPI pause keeps the session alive without advancing frames");

    engine.request_resume();
    const auto resume_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < resume_deadline &&
           (engine.progress().state != audio::PlaybackState::playing ||
            engine.progress().frames_rendered <= paused_progress.frames_rendered)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(
        engine.progress().state == audio::PlaybackState::playing &&
            engine.progress().frames_rendered > paused_progress.frames_rendered,
        "real WASAPI session resumes and advances after pause");

    engine.request_stop();
    playback.join();
    expect(
        result && result->error ==
                platform::windows::CddaPlaybackError::cancelled,
        "paused and resumed playback remains cooperatively cancellable");
}

} // namespace

int main(const int argument_count, char** arguments)
{
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    test_result_semantics();
    test_autoplay_launch_arguments();
    test_autoplay_policy_masks();
    test_theme_palettes();
    test_animation_timing();
    test_settings_model();
    test_playback_error_presentation();
    test_metadata_source_capsules();
    test_diagnostic_redaction_and_export();
    test_device_failure_classification();
    test_wasapi_negotiation_and_fallback();
    test_device_lifecycle_message_classification();
    test_invalid_requests_without_device_access();
    test_playback_session_seek_planning();
    test_volume_control_boundaries();
    test_listenbrainz_payload_contract();
    test_listenbrainz_queue_restart_and_account_isolation();
    test_listenbrainz_fake_http_failures();
    test_online_lyrics_matching_and_parsing();
    test_cloud_lyrics_codecs();
    test_cddb_configuration_contract();
    test_system_media_controls_safe_fallback();
    test_system_media_controls_with_window();
    test_user_settings_round_trip();
    test_musicbrainz_exact_lookup_paths();
    test_musicbrainz_multiple_release_parsing();
    test_musicbrainz_content_association_reorders_recordings();
    test_musicbrainz_content_release_json_parsing();
    test_metadata_revision_selection_and_cache();
    test_diagnostic_names();
    if (argument_count == 2 &&
        std::string_view(arguments[1]) == "--hardware-cancel") {
        test_hardware_cancellation();
    } else if (argument_count == 2 &&
               std::string_view(arguments[1]) == "--autoplay-policy-repair") {
        test_autoplay_policy_repair_live();
    } else if (argument_count == 2 &&
               std::string_view(arguments[1]) == "--hardware-pause") {
        test_hardware_pause_resume();
    } else if (argument_count == 2 &&
               std::string_view(arguments[1]) == "--online-lyrics") {
        test_online_lyrics_live();
    } else if (argument_count == 2 &&
               std::string_view(arguments[1]) == "--online-krc") {
        test_krc_translation_alignment_live();
    } else if (argument_count == 2 &&
               std::string_view(arguments[1]) == "--online-marchen-track2") {
        test_marchen_track_two_live();
    }

    if (failures != 0) {
        if (SUCCEEDED(com_result)) {
            CoUninitialize();
        }
        std::cerr << failures << " Windows playback test(s) failed.\n";
        return 1;
    }

    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }
    std::cout << "All CD.404 Windows playback tests passed.\n";
    return 0;
}
