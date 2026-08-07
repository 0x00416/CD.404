#include <windows.h>

#include <cd404/core/cd_time.hpp>
#include <cd404/platform/windows/cdda_playback_engine.hpp>
#include <cd404/platform/windows/optical_drive.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>
#include <thread>

namespace {

std::atomic_bool g_cancel_requested{};

BOOL WINAPI handle_console_control(const DWORD control_type)
{
    if (control_type != CTRL_C_EVENT && control_type != CTRL_BREAK_EVENT &&
        control_type != CTRL_CLOSE_EVENT) {
        return FALSE;
    }

    g_cancel_requested.store(true, std::memory_order_release);
    return TRUE;
}

class ConsoleControlRegistration final {
public:
    ConsoleControlRegistration() noexcept
        : registered_(SetConsoleCtrlHandler(handle_console_control, TRUE) != FALSE)
    {
    }

    ~ConsoleControlRegistration()
    {
        if (registered_) {
            static_cast<void>(SetConsoleCtrlHandler(handle_console_control, FALSE));
        }
    }

    [[nodiscard]] bool registered() const noexcept { return registered_; }

private:
    bool registered_{};
};

struct Options final {
    unsigned int track_number{};
    unsigned int offset_seconds{};
    std::optional<unsigned int> seconds{15};
    bool show_help{};
    bool valid{true};
};

[[nodiscard]] bool parse_unsigned(
    const wchar_t* text,
    const unsigned long maximum,
    unsigned int& value) noexcept
{
    wchar_t* end{};
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (text == end || *end != L'\0' || parsed == 0 || parsed > maximum) {
        return false;
    }
    value = static_cast<unsigned int>(parsed);
    return true;
}

[[nodiscard]] Options parse_options(const int argument_count, wchar_t** arguments)
{
    Options options;
    bool duration_was_set{};
    for (int index = 1; index < argument_count; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--help" || argument == L"-h") {
            options.show_help = true;
            continue;
        }
        if (argument == L"--all") {
            if (duration_was_set) {
                options.valid = false;
                return options;
            }
            options.seconds.reset();
            duration_was_set = true;
            continue;
        }
        if ((argument == L"--track" || argument == L"--offset-seconds" ||
             argument == L"--seconds") &&
            index + 1 < argument_count) {
            unsigned int parsed{};
            const unsigned long maximum = argument == L"--track" ? 99UL : 86'400UL;
            if (!parse_unsigned(arguments[++index], maximum, parsed) ||
                (argument == L"--seconds" && duration_was_set)) {
                options.valid = false;
                return options;
            }
            if (argument == L"--track") {
                options.track_number = parsed;
            } else if (argument == L"--offset-seconds") {
                options.offset_seconds = parsed;
            } else {
                options.seconds = parsed;
                duration_was_set = true;
            }
            continue;
        }

        options.valid = false;
        return options;
    }
    return options;
}

void print_usage()
{
    std::wcout << L"Usage: CD.404.Playback [--track N] [--offset-seconds N] "
                  L"[--seconds N | --all]\n"
                  L"  --track N    Start at this audio track; defaults to the first.\n"
                  L"  --offset-seconds N  Seek N seconds from the selected track start.\n"
                  L"  --seconds N  Play at most N seconds; default is 15.\n"
                  L"  --all        Play to the end of the contiguous audio-track run.\n"
                  L"Press Ctrl+C to stop without draining queued audio.\n";
}

void print_hresult(const wchar_t* operation, const std::int32_t status)
{
    std::wcout << operation << L" failed with HRESULT 0x" << std::hex
               << std::setw(8) << std::setfill(L'0')
               << static_cast<std::uint32_t>(status) << std::dec << std::setfill(L' ')
               << L".\n";
}

void print_statistics(
    const cd404::platform::windows::CddaPlaybackResult& result)
{
    const auto& read = result.read_statistics;
    const auto& buffer = result.buffer_statistics;
    std::wcout << L"Read statistics: " << read.logical_reads << L" logical, "
               << read.device_reads << L" device, " << read.retries
               << L" retries, " << read.overlap_checks << L" overlap checks, "
               << read.overlap_mismatches << L" mismatches, "
               << read.sectors_delivered << L" sectors delivered.\n"
               << L"Buffer statistics: " << buffer.producer_starvation_events
               << L" producer starvation event(s), "
               << buffer.endpoint_underruns << L" endpoint underrun(s), "
               << buffer.minimum_queued_frames
               << L" minimum queued frame(s) after start.\n";
}

int print_failure(const cd404::platform::windows::CddaPlaybackResult& result)
{
    using cd404::platform::windows::CddaPlaybackError;
    using cd404::platform::windows::format_system_error;

    switch (result.error) {
    case CddaPlaybackError::none:
        return 0;
    case CddaPlaybackError::cancelled:
        std::wcout << L"Playback cancelled after " << result.frames_submitted
                   << L" frame(s).\n";
        return 130;
    case CddaPlaybackError::already_running:
        std::wcout << L"The playback engine is already running.\n";
        break;
    case CddaPlaybackError::no_ready_audio_cd:
        std::wcout << L"No ready audio CD with the requested track was found.\n";
        break;
    case CddaPlaybackError::source_open_failed:
        std::wcout << L"Unable to open the CDDA source: "
                   << format_system_error(result.system_error) << L"\n";
        break;
    case CddaPlaybackError::invalid_stream:
        std::wcout << L"Unable to create a continuous stream for the audio range.\n";
        break;
    case CddaPlaybackError::invalid_range:
        std::wcout << L"The requested playback range is empty or outside the disc.\n";
        break;
    case CddaPlaybackError::output_open_failed:
        print_hresult(L"Opening the default audio endpoint", result.audio_status);
        break;
    case CddaPlaybackError::invalid_endpoint_buffer:
        std::wcout << L"The default audio endpoint reported an empty buffer.\n";
        break;
    case CddaPlaybackError::read_failed:
        std::wcout << L"CDDA streaming stopped after " << result.frames_produced
                   << L" frame(s): ";
        if (result.read_status == cd404::audio::ReadStatus::verification_error) {
            std::wcout << L"sequential sector overlap verification failed.\n";
        } else {
            std::wcout << format_system_error(result.system_error) << L"\n";
        }
        break;
    case CddaPlaybackError::output_failed:
        print_hresult(L"Streaming PCM frames", result.audio_status);
        break;
    case CddaPlaybackError::endpoint_underrun:
        std::wcout << L"Playback stopped because the audio endpoint ran out of "
                      L"buffered frames before the CD reader produced more data.\n";
        break;
    case CddaPlaybackError::incomplete:
        std::wcout << L"Playback ended before every requested frame was submitted.\n";
        break;
    }
    return 1;
}

} // namespace

int wmain(const int argument_count, wchar_t** arguments)
{
    using namespace cd404;

    const Options options = parse_options(argument_count, arguments);
    if (!options.valid || options.show_help) {
        print_usage();
        return options.valid ? 0 : 2;
    }

    g_cancel_requested.store(false, std::memory_order_release);
    ConsoleControlRegistration console_control;
    if (!console_control.registered()) {
        std::wcout << L"Unable to install the Ctrl+C handler.\n";
        return 1;
    }

    platform::windows::CddaPlaybackRequest request;
    request.track_number = options.track_number;
    request.offset_frames =
        static_cast<core::SampleFrame>(options.offset_seconds) *
        core::kCdSampleFramesPerSecond;
    request.maximum_frames = options.seconds
        ? std::optional<core::SampleFrame>(
              static_cast<core::SampleFrame>(*options.seconds) *
              core::kCdSampleFramesPerSecond)
        : std::nullopt;

    platform::windows::CddaPlaybackEngine engine;
    std::jthread cancel_watcher([&](const std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            if (g_cancel_requested.load(std::memory_order_acquire)) {
                engine.request_stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    std::wcout << L"Starting the reusable CDDA playback engine...\n";
    const auto result = engine.play(request);
    cancel_watcher.request_stop();
    cancel_watcher.join();
    print_statistics(result);

    if (!result.succeeded()) {
        return print_failure(result);
    }

    std::wcout << L"Playback completed from track " << result.first_track_number;
    if (result.final_track_number != result.first_track_number) {
        std::wcout << L" through " << result.final_track_number;
    }
    std::wcout << L": " << result.frames_submitted
               << L" frame(s) submitted without rebuilding the CDDA stream.\n";
    return 0;
}
