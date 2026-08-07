#include <cd404/audio/playback_state_machine.hpp>
#include <cd404/platform/windows/cdda_playback_engine.hpp>

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
    test_result_semantics();
    test_invalid_requests_without_device_access();
    test_diagnostic_names();
    if (argument_count == 2 &&
        std::string_view(arguments[1]) == "--hardware-cancel") {
        test_hardware_cancellation();
    }

    if (failures != 0) {
        std::cerr << failures << " Windows playback test(s) failed.\n";
        return 1;
    }

    std::cout << "All CD.404 Windows playback tests passed.\n";
    return 0;
}
