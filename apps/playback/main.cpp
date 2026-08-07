#include <windows.h>

#include <cd404/audio/cdda_pcm.hpp>
#include <cd404/audio/continuous_cdda_stream.hpp>
#include <cd404/audio/pcm16_spsc_ring_buffer.hpp>
#include <cd404/core/cd_time.hpp>
#include <cd404/platform/windows/optical_drive.hpp>
#include <cd404/platform/windows/raw_cdda_sector_source.hpp>
#include <cd404/platform/windows/wasapi_output.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using cd404::core::SampleFrame;

constexpr std::size_t kReadBlockSectors = 16;
constexpr std::size_t kReadBlockFrames =
    kReadBlockSectors *
    static_cast<std::size_t>(cd404::core::kCdSampleFramesPerSector);
constexpr std::size_t kRingCapacityFrames =
    6 * static_cast<std::size_t>(cd404::core::kCdSampleFramesPerSecond);
constexpr std::size_t kPrebufferFrames =
    static_cast<std::size_t>(cd404::core::kCdSampleFramesPerSecond);
constexpr auto kQueueWait = std::chrono::milliseconds(50);

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

    [[nodiscard]] bool registered() const noexcept
    {
        return registered_;
    }

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

struct StreamState final {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic_bool stop_requested{};
    cd404::audio::ReadStatus terminal_status{cd404::audio::ReadStatus::ok};
    unsigned long native_error{};
    SampleFrame frames_produced{};
};

struct ProducerContext final {
    cd404::audio::ContinuousCddaStream& stream;
    cd404::audio::Pcm16SpscRingBuffer& ring;
    StreamState& state;
    SampleFrame target_frames{};
};

struct SubmitResult final {
    std::int32_t status{};
    std::uint32_t frames_submitted{};
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

void request_stop(
    StreamState& state,
    cd404::platform::windows::RawCddaSectorSource& source)
{
    state.stop_requested.store(true, std::memory_order_release);
    source.request_cancel();
    state.changed.notify_all();
}

void produce_pcm(ProducerContext context)
{
    using namespace cd404;

    std::array<std::int16_t, kReadBlockFrames * audio::kPcm16StereoChannelCount>
        samples{};

    while (!context.state.stop_requested.load(std::memory_order_acquire) &&
           context.state.frames_produced < context.target_frames) {
        const auto remaining = context.target_frames - context.state.frames_produced;
        const std::size_t wanted_frames = static_cast<std::size_t>(
            std::min<SampleFrame>(remaining, static_cast<SampleFrame>(kReadBlockFrames)));
        auto sample_span = std::span(samples).first(
            wanted_frames * audio::kPcm16StereoChannelCount);
        auto byte_span = std::as_writable_bytes(sample_span);
        const auto read_result = context.stream.read_frames(byte_span);

        if (read_result.frames_read > wanted_frames ||
            audio::convert_cdda_to_pcm16le_in_place(
                byte_span.first(
                    read_result.frames_read *
                    static_cast<std::size_t>(core::kCdBytesPerSampleFrame))) !=
                audio::CddaPcmConversionStatus::ok) {
            context.state.terminal_status = audio::ReadStatus::io_error;
            context.state.native_error = ERROR_INVALID_DATA;
            break;
        }

        std::size_t pushed_frames{};
        while (pushed_frames < read_result.frames_read &&
               !context.state.stop_requested.load(std::memory_order_acquire)) {
            const auto remaining_samples = sample_span.subspan(
                pushed_frames * audio::kPcm16StereoChannelCount,
                (read_result.frames_read - pushed_frames) *
                    audio::kPcm16StereoChannelCount);
            const auto push_result = context.ring.push(remaining_samples);
            pushed_frames += push_result.frames_transferred;
            context.state.frames_produced +=
                static_cast<SampleFrame>(push_result.frames_transferred);
            if (push_result.frames_transferred != 0) {
                context.state.changed.notify_one();
                continue;
            }
            if (push_result.status != audio::Pcm16BufferStatus::full) {
                context.state.terminal_status = audio::ReadStatus::io_error;
                context.state.native_error = ERROR_WRITE_FAULT;
                break;
            }

            std::unique_lock lock(context.state.mutex);
            context.state.changed.wait_for(lock, kQueueWait, [&context] {
                return context.state.stop_requested.load(std::memory_order_acquire) ||
                       context.ring.readable_frames() <
                           context.ring.capacity_frames();
            });
        }

        if (context.state.terminal_status != audio::ReadStatus::ok) {
            break;
        }
        if (read_result.status != audio::ReadStatus::ok ||
            read_result.frames_read != wanted_frames) {
            context.state.terminal_status =
                read_result.status == audio::ReadStatus::end_of_stream
                    ? audio::ReadStatus::end_of_stream
                    : audio::ReadStatus::io_error;
            context.state.native_error = read_result.native_error;
            break;
        }
    }

    context.ring.close();
    context.state.changed.notify_all();
}

[[nodiscard]] SubmitResult submit_frames(
    cd404::platform::windows::WasapiOutput& output,
    const std::span<const std::int16_t> samples,
    const std::uint32_t frame_count,
    bool& started)
{
    using cd404::platform::windows::WasapiOutput;

    std::uint32_t submitted{};
    while (submitted < frame_count) {
        const auto remaining_samples = samples.subspan(
            static_cast<std::size_t>(submitted) * WasapiOutput::channel_count);
        const auto write_result = output.write_interleaved(
            remaining_samples,
            frame_count - submitted);
        submitted += write_result.frames_written;
        if (write_result.status < 0) {
            return {write_result.status, submitted};
        }

        if (!started) {
            const std::int32_t start_status = output.start();
            if (start_status < 0) {
                return {start_status, submitted};
            }
            started = true;
        }

        if (write_result.frames_written == 0) {
            return {static_cast<std::int32_t>(E_UNEXPECTED), submitted};
        }
    }
    return {static_cast<std::int32_t>(S_OK), submitted};
}

[[nodiscard]] std::size_t find_track_index(
    const cd404::disc::Toc& toc,
    const unsigned int requested_track)
{
    const auto& tracks = toc.tracks();
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (tracks[index].is_audio &&
            (requested_track == 0 || tracks[index].number == requested_track)) {
            return index;
        }
    }
    return tracks.size();
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

    const auto drives = platform::windows::enumerate_optical_drives();
    const platform::windows::OpticalDrive* selected_drive{};
    std::optional<disc::Toc> selected_toc;
    std::size_t selected_track_index{};
    for (const auto& drive : drives) {
        auto toc_result = platform::windows::read_toc(drive);
        if (!toc_result.toc) {
            continue;
        }
        const std::size_t track_index =
            find_track_index(*toc_result.toc, options.track_number);
        if (track_index != toc_result.toc->tracks().size()) {
            selected_drive = &drive;
            selected_track_index = track_index;
            selected_toc = std::move(toc_result.toc);
            break;
        }
    }

    if (selected_drive == nullptr || !selected_toc) {
        std::wcout << L"No ready audio CD with the requested track was found.\n";
        return 1;
    }

    const auto& tracks = selected_toc->tracks();
    std::size_t run_end_index = selected_track_index;
    while (run_end_index + 1 < tracks.size() &&
           tracks[run_end_index + 1].is_audio &&
           tracks[run_end_index].end_lba == tracks[run_end_index + 1].start_lba) {
        ++run_end_index;
    }
    const auto& selected_track = tracks[selected_track_index];
    const auto& final_track = tracks[run_end_index];

    auto source_result = platform::windows::open_raw_cdda_source(
        *selected_drive,
        selected_track.start_lba,
        final_track.end_lba);
    if (!source_result.source) {
        std::wcout << L"Unable to open the CDDA source: "
                   << platform::windows::format_system_error(
                          source_result.system_error)
                   << L"\n";
        return 1;
    }

    audio::ContinuousCddaStream stream(
        *source_result.source,
        selected_track.start_lba,
        final_track.end_lba);
    if (!stream.valid()) {
        std::wcout << L"Unable to create a continuous stream for the audio range.\n";
        return 1;
    }

    const SampleFrame offset_frames =
        static_cast<SampleFrame>(options.offset_seconds) *
        core::kCdSampleFramesPerSecond;
    if (offset_frames >= stream.total_frames() || !stream.seek(offset_frames)) {
        std::wcout << L"The requested start offset is outside the audio range.\n";
        return 1;
    }

    SampleFrame target_frames = stream.total_frames() - offset_frames;
    if (options.seconds) {
        const SampleFrame requested_frames =
            static_cast<SampleFrame>(*options.seconds) *
            core::kCdSampleFramesPerSecond;
        target_frames = std::min(target_frames, requested_frames);
    }
    if (target_frames <= 0) {
        std::wcout << L"The requested playback range is empty.\n";
        return 1;
    }

    platform::windows::WasapiOutput output;
    const std::int32_t open_status = output.open_default_shared();
    if (open_status < 0) {
        print_hresult(L"Opening the default audio endpoint", open_status);
        return 1;
    }
    const std::size_t endpoint_buffer_frames = output.buffer_frame_count();
    if (endpoint_buffer_frames == 0) {
        std::wcout << L"The default audio endpoint reported an empty buffer.\n";
        return 1;
    }

    const std::size_t consumer_capacity =
        std::max(kReadBlockFrames, endpoint_buffer_frames);
    std::vector<std::int16_t> consumer_samples(
        consumer_capacity * audio::kPcm16StereoChannelCount);

    g_cancel_requested.store(false, std::memory_order_release);
    ConsoleControlRegistration console_control;
    if (!console_control.registered()) {
        std::wcout << L"Unable to install the Ctrl+C handler.\n";
        return 1;
    }
    audio::Pcm16SpscRingBuffer ring(kRingCapacityFrames);
    StreamState state;
    std::jthread cancel_watcher([&](const std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            if (g_cancel_requested.load(std::memory_order_acquire)) {
                request_stop(state, *source_result.source);
                output.request_cancel();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    ProducerContext producer_context{stream, ring, state, target_frames};
    std::thread producer(produce_pcm, producer_context);

    std::wcout << L"Streaming track "
               << static_cast<unsigned int>(selected_track.number);
    if (final_track.number != selected_track.number) {
        std::wcout << L" through "
                   << static_cast<unsigned int>(final_track.number);
    }
    std::wcout << L" using a 6-second bounded buffer";
    if (options.seconds) {
        std::wcout << L" for at most " << *options.seconds << L" second(s)";
    }
    if (options.offset_seconds != 0) {
        std::wcout << L", starting " << options.offset_seconds
                   << L" second(s) into the selected track";
    }
    std::wcout << L"...\n";

    {
        std::unique_lock lock(state.mutex);
        const std::size_t prebuffer_target =
            std::min(kPrebufferFrames, static_cast<std::size_t>(target_frames));
        while (ring.readable_frames() < prebuffer_target && !ring.closed() &&
               !g_cancel_requested.load(std::memory_order_acquire)) {
            state.changed.wait_for(lock, kQueueWait);
        }
    }

    SampleFrame submitted_frames{};
    bool output_started{};
    bool underrun_detected{};
    std::int32_t playback_status{S_OK};

    while (!g_cancel_requested.load(std::memory_order_acquire)) {
        const std::size_t readable = ring.readable_frames();
        if (readable == 0) {
            if (ring.drained()) {
                break;
            }
            if (output_started && submitted_frames < target_frames) {
                std::uint32_t padding{};
                const std::int32_t padding_status =
                    output.get_current_padding(padding);
                if (padding_status < 0) {
                    playback_status = padding_status;
                    request_stop(state, *source_result.source);
                    break;
                }
                if (padding == 0) {
                    underrun_detected = true;
                    request_stop(state, *source_result.source);
                    break;
                }
            }
            std::unique_lock lock(state.mutex);
            state.changed.wait_for(lock, kQueueWait, [&] {
                return ring.readable_frames() != 0 || ring.closed() ||
                       g_cancel_requested.load(std::memory_order_acquire);
            });
            continue;
        }

        std::size_t frames_to_pop = std::min(readable, consumer_capacity);
        if (!output_started) {
            frames_to_pop = std::min(
                frames_to_pop,
                endpoint_buffer_frames);
        }
        auto destination = std::span(consumer_samples).first(
            frames_to_pop * audio::kPcm16StereoChannelCount);
        const auto pop_result = ring.pop(destination);
        if (pop_result.frames_transferred == 0) {
            continue;
        }
        state.changed.notify_one();

        const auto submit_result = submit_frames(
            output,
            destination,
            static_cast<std::uint32_t>(pop_result.frames_transferred),
            output_started);
        submitted_frames += static_cast<SampleFrame>(submit_result.frames_submitted);
        if (submit_result.status < 0 ||
            submit_result.frames_submitted != pop_result.frames_transferred) {
            playback_status = submit_result.status;
            request_stop(state, *source_result.source);
            break;
        }
    }

    const bool cancelled = g_cancel_requested.load(std::memory_order_acquire);
    if (cancelled) {
        request_stop(state, *source_result.source);
    }
    producer.join();

    const bool read_failed =
        state.terminal_status != audio::ReadStatus::ok ||
        state.frames_produced != target_frames;
    bool completed = !cancelled && playback_status >= 0 && !read_failed &&
                     submitted_frames == target_frames;
    if (completed) {
        const std::int32_t drain_status = output.drain();
        if (drain_status < 0) {
            playback_status = drain_status;
            completed = false;
        }
    }
    static_cast<void>(output.stop());
    cancel_watcher.request_stop();
    cancel_watcher.join();

    if (cancelled || g_cancel_requested.load(std::memory_order_acquire)) {
        std::wcout << L"Playback cancelled after " << submitted_frames
                   << L" frame(s).\n";
        return 130;
    }
    if (playback_status < 0) {
        print_hresult(L"Streaming PCM frames", playback_status);
        return 1;
    }
    if (underrun_detected) {
        std::wcout << L"Playback stopped because the audio endpoint ran out of "
                      L"buffered frames before the CD reader produced more data.\n";
        return 1;
    }
    if (read_failed) {
        std::wcout << L"CDDA streaming stopped after " << state.frames_produced
                   << L" frame(s): "
                   << platform::windows::format_system_error(state.native_error)
                   << L"\n";
        return 1;
    }
    if (!completed) {
        std::wcout << L"Playback ended before every requested frame was submitted.\n";
        return 1;
    }

    std::wcout << L"Playback completed: " << submitted_frames
               << L" frame(s) submitted without rebuilding the CDDA stream.\n";
    return 0;
}
