#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <dbt.h>
#include <objbase.h>
#include <winsqlite/winsqlite3.h>

#include <cd404/audio/playback_state_machine.hpp>
#include <cd404/platform/windows/cdda_playback_engine.hpp>
#include <cd404/platform/windows/device_lifecycle.hpp>
#include <cd404/platform/windows/diagnostics.hpp>
#include <cd404/platform/windows/listenbrainz_reporter.hpp>
#include <cd404/platform/windows/listenbrainz_queue.hpp>
#include <cd404/platform/windows/musicbrainz_client.hpp>
#include <cd404/platform/windows/metadata_store.hpp>
#include <cd404/platform/windows/system_media_controls.hpp>
#include <cd404/platform/windows/user_settings.hpp>
#include <cd404/platform/windows/wasapi_output.hpp>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string_view>
#include <thread>

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
            now_additional.GetNamedArray(L"artist_mbids").Size() == 2,
        "ListenBrainz payload carries exact MusicBrainz identities when available");

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
    source.audio_exclusive_mode = true;
    source.audio_allow_shared_fallback = true;
    source.playback_positions[disc_key] = {2, 123'456};
    const std::wstring json = encode_user_settings(source);
    const UserSettings decoded = decode_user_settings(json);
    const auto position = decoded.playback_positions.find(disc_key);
    expect(
        decoded.volume > 0.369F && decoded.volume < 0.371F &&
            !decoded.listenbrainz_reporting_enabled &&
            decoded.audio_endpoint_id == source.audio_endpoint_id &&
            decoded.audio_exclusive_mode &&
            decoded.audio_allow_shared_fallback,
        "user settings JSON round-trips persistent options");
    expect(
        position != decoded.playback_positions.end() &&
            position->second.track_number == 2 &&
            position->second.offset_frames == 123'456,
        "user settings JSON round-trips per-disc playback position");
    expect(
        json.find(L"Token") == std::wstring::npos &&
            json.find(L"token") == std::wstring::npos,
        "settings JSON never contains a ListenBrainz token");

    const UserSettings defaults = decode_user_settings(L"not json");
    expect(
        defaults.volume == 1.0F && defaults.listenbrainz_reporting_enabled &&
            defaults.audio_endpoint_id.empty() &&
            !defaults.audio_exclusive_mode &&
            !defaults.audio_allow_shared_fallback,
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
    test_system_media_controls_safe_fallback();
    test_system_media_controls_with_window();
    test_user_settings_round_trip();
    test_musicbrainz_exact_lookup_paths();
    test_musicbrainz_multiple_release_parsing();
    test_metadata_revision_selection_and_cache();
    test_diagnostic_names();
    if (argument_count == 2 &&
        std::string_view(arguments[1]) == "--hardware-cancel") {
        test_hardware_cancellation();
    } else if (argument_count == 2 &&
               std::string_view(arguments[1]) == "--hardware-pause") {
        test_hardware_pause_resume();
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
