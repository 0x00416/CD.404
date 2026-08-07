#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include <cd404/audio/playback_state_machine.hpp>
#include <cd404/platform/windows/cdda_playback_engine.hpp>
#include <cd404/platform/windows/listenbrainz_reporter.hpp>
#include <cd404/platform/windows/system_media_controls.hpp>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <chrono>
#include <iostream>
#include <optional>
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

    listenbrainz::Submission playing_now;
    playing_now.type = listenbrainz::SubmissionType::playing_now;
    playing_now.listened_at = 1'700'000'000;
    playing_now.track_name = L"Track";
    playing_now.artist_name = L"Artist";
    playing_now.release_name = L"Album";
    playing_now.duration_milliseconds = 123'000;

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

void test_diagnostic_names()
{
    using namespace cd404;

    constexpr std::array states{
        audio::PlaybackState::idle,
        audio::PlaybackState::opening,
        audio::PlaybackState::buffering,
        audio::PlaybackState::playing,
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

} // namespace

int main(const int argument_count, char** arguments)
{
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    test_result_semantics();
    test_invalid_requests_without_device_access();
    test_volume_control_boundaries();
    test_listenbrainz_payload_contract();
    test_system_media_controls_safe_fallback();
    test_system_media_controls_with_window();
    test_diagnostic_names();
    if (argument_count == 2 &&
        std::string_view(arguments[1]) == "--hardware-cancel") {
        test_hardware_cancellation();
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
