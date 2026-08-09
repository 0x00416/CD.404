#include <windows.h>

#include <cd404/audio/cdda_pcm.hpp>
#include <cd404/audio/continuous_cdda_stream.hpp>
#include <cd404/audio/pcm16_spsc_ring_buffer.hpp>
#include <cd404/audio/pcm16_volume.hpp>
#include <cd404/platform/windows/cdda_playback_engine.hpp>
#include <cd404/platform/windows/raw_cdda_sector_source.hpp>
#include <cd404/platform/windows/wasapi_output.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace cd404::platform::windows {
namespace {

using core::SampleFrame;

constexpr std::size_t kReadBlockSectors = 16;
constexpr std::size_t kReadBlockFrames =
    kReadBlockSectors * static_cast<std::size_t>(core::kCdSampleFramesPerSector);
constexpr std::size_t kRingCapacityFrames =
    6 * static_cast<std::size_t>(core::kCdSampleFramesPerSecond);
constexpr std::size_t kPrebufferFrames =
    3 * static_cast<std::size_t>(core::kCdSampleFramesPerSecond);
constexpr auto kQueueWait = std::chrono::milliseconds(50);

struct StreamState final {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic_bool stop_requested{};
    audio::ReadStatus terminal_status{audio::ReadStatus::ok};
    unsigned long native_error{};
    SampleFrame frames_produced{};
};

struct ProducerContext final {
    audio::ContinuousCddaStream& stream;
    audio::Pcm16SpscRingBuffer& ring;
    StreamState& state;
    std::atomic<SampleFrame>& published_frames_produced;
    SampleFrame target_frames{};
};

struct SubmitResult final {
    std::int32_t status{};
    std::uint32_t frames_submitted{};
};

struct ActiveReset final {
    std::atomic_bool& active;
    ~ActiveReset() { active.store(false, std::memory_order_release); }
};

void request_stream_stop(StreamState& state, RawCddaSectorSource& source) noexcept
{
    state.stop_requested.store(true, std::memory_order_release);
    source.request_cancel();
    state.changed.notify_all();
}

void produce_pcm(const ProducerContext context)
{
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
            context.published_frames_produced.store(
                context.state.frames_produced,
                std::memory_order_release);
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
                read_result.status == audio::ReadStatus::ok
                    ? audio::ReadStatus::io_error
                    : read_result.status;
            context.state.native_error = read_result.native_error;
            break;
        }
    }

    context.ring.close();
    context.state.changed.notify_all();
}

[[nodiscard]] SubmitResult submit_frames(
    WasapiOutput& output,
    const std::span<const std::int16_t> samples,
    const std::uint32_t frame_count,
    bool& started)
{
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
    const disc::Toc& toc,
    const unsigned int requested_track) noexcept
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

bool CddaPlaybackResult::succeeded() const noexcept
{
    return error == CddaPlaybackError::none &&
           final_state == audio::PlaybackState::completed;
}

CddaPlaybackResult CddaPlaybackEngine::play(const CddaPlaybackRequest& request)
{
    CddaPlaybackResult result;
    bool expected_inactive{};
    if (!active_.compare_exchange_strong(
            expected_inactive,
            true,
            std::memory_order_acq_rel)) {
        result.final_state = state_.load(std::memory_order_acquire);
        result.error = CddaPlaybackError::already_running;
        return result;
    }
    ActiveReset active_reset{active_};

    stop_requested_.store(false, std::memory_order_release);
    target_frames_.store(0, std::memory_order_release);
    frames_produced_.store(0, std::memory_order_release);
    frames_submitted_.store(0, std::memory_order_release);
    frames_rendered_.store(0, std::memory_order_release);

    audio::PlaybackStateMachine state_machine;
    const auto advance = [this, &state_machine](const audio::PlaybackEvent event) {
        const bool accepted = state_machine.apply(event);
        if (accepted) {
            state_.store(state_machine.state(), std::memory_order_release);
        }
        return accepted;
    };
    static_cast<void>(advance(audio::PlaybackEvent::open));

    const auto finish_failed = [&](const CddaPlaybackError error) {
        static_cast<void>(advance(audio::PlaybackEvent::failure));
        result.final_state = state_machine.state();
        result.error = error;
        return result;
    };
    const auto finish_cancelled = [&] {
        static_cast<void>(advance(audio::PlaybackEvent::stop_requested));
        static_cast<void>(advance(audio::PlaybackEvent::cancellation_completed));
        result.final_state = state_machine.state();
        result.error = CddaPlaybackError::cancelled;
        return result;
    };

    if (request.offset_frames < 0 ||
        (request.maximum_frames && *request.maximum_frames <= 0)) {
        return finish_failed(CddaPlaybackError::invalid_range);
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
        return finish_cancelled();
    }

    std::vector<OpticalDrive> drives;
    if (request.drive) {
        drives.push_back(*request.drive);
    } else {
        drives = enumerate_optical_drives();
    }

    const OpticalDrive* selected_drive{};
    std::optional<disc::Toc> selected_toc;
    std::size_t selected_track_index{};
    for (const auto& drive : drives) {
        auto toc_result = read_toc(drive);
        if (!toc_result.toc) {
            continue;
        }
        const std::size_t track_index =
            find_track_index(*toc_result.toc, request.track_number);
        if (track_index != toc_result.toc->tracks().size()) {
            selected_drive = &drive;
            selected_track_index = track_index;
            selected_toc = std::move(toc_result.toc);
            break;
        }
    }
    if (selected_drive == nullptr || !selected_toc) {
        return stop_requested_.load(std::memory_order_acquire)
            ? finish_cancelled()
            : finish_failed(CddaPlaybackError::no_ready_audio_cd);
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
    result.first_track_number = selected_track.number;
    result.final_track_number = final_track.number;

    auto source_result = open_raw_cdda_source(
        *selected_drive,
        selected_track.start_lba,
        final_track.end_lba);
    if (!source_result.source) {
        result.system_error = source_result.system_error;
        return stop_requested_.load(std::memory_order_acquire)
            ? finish_cancelled()
            : finish_failed(CddaPlaybackError::source_open_failed);
    }

    audio::ReliableCddaSectorSource reliable_source(*source_result.source);
    audio::ContinuousCddaStream stream(
        reliable_source,
        selected_track.start_lba,
        final_track.end_lba);
    if (!stream.valid()) {
        return finish_failed(CddaPlaybackError::invalid_stream);
    }
    if (request.offset_frames >= stream.total_frames() ||
        !stream.seek(request.offset_frames)) {
        return finish_failed(CddaPlaybackError::invalid_range);
    }

    SampleFrame target_frames = stream.total_frames() - request.offset_frames;
    if (request.maximum_frames) {
        target_frames = std::min(target_frames, *request.maximum_frames);
    }
    if (target_frames <= 0) {
        return finish_failed(CddaPlaybackError::invalid_range);
    }
    result.target_frames = target_frames;
    target_frames_.store(target_frames, std::memory_order_release);

    WasapiOutput output;
    const std::int32_t open_status = output.open_default_shared();
    if (open_status < 0) {
        result.audio_status = open_status;
        return finish_failed(CddaPlaybackError::output_open_failed);
    }
    const std::size_t endpoint_buffer_frames = output.buffer_frame_count();
    if (endpoint_buffer_frames == 0) {
        return finish_failed(CddaPlaybackError::invalid_endpoint_buffer);
    }

    const std::size_t consumer_capacity =
        std::max(kReadBlockFrames, endpoint_buffer_frames);
    std::vector<std::int16_t> consumer_samples(
        consumer_capacity * audio::kPcm16StereoChannelCount);
    audio::Pcm16SpscRingBuffer ring(kRingCapacityFrames);
    StreamState stream_state;
    static_cast<void>(advance(audio::PlaybackEvent::source_ready));

    std::jthread cancel_watcher([&](const std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                request_stream_stop(stream_state, *source_result.source);
                output.request_cancel();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    const ProducerContext producer_context{
        stream,
        ring,
        stream_state,
        frames_produced_,
        target_frames,
    };
    std::thread producer(produce_pcm, producer_context);

    {
        std::unique_lock lock(stream_state.mutex);
        const std::size_t prebuffer_target =
            std::min(kPrebufferFrames, static_cast<std::size_t>(target_frames));
        while (ring.readable_frames() < prebuffer_target && !ring.closed() &&
               !stop_requested_.load(std::memory_order_acquire)) {
            stream_state.changed.wait_for(lock, kQueueWait);
        }
    }

    SampleFrame submitted_frames{};
    bool output_started{};
    bool underrun_detected{};
    bool waiting_for_producer{};
    PlaybackBufferStatistics buffer_statistics;
    std::size_t minimum_queued_frames{std::numeric_limits<std::size_t>::max()};
    std::int32_t playback_status{S_OK};
    static_cast<void>(advance(audio::PlaybackEvent::prebuffer_ready));

    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (pause_requested_.load(std::memory_order_acquire)) {
            std::uint32_t padding{};
            if (output.get_current_padding(padding) >= 0) {
                const SampleFrame rendered_frames = std::max<SampleFrame>(
                    submitted_frames - static_cast<SampleFrame>(padding),
                    0);
                frames_rendered_.store(rendered_frames, std::memory_order_release);
            }
            if (output_started) {
                playback_status = output.pause();
                if (playback_status < 0) {
                    request_stream_stop(stream_state, *source_result.source);
                    break;
                }
            }
            static_cast<void>(advance(audio::PlaybackEvent::pause_requested));
            {
                std::unique_lock lock(control_mutex_);
                control_changed_.wait(lock, [this] {
                    return stop_requested_.load(std::memory_order_acquire) ||
                           !pause_requested_.load(std::memory_order_acquire);
                });
            }
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            if (output_started) {
                playback_status = output.start();
                if (playback_status < 0) {
                    request_stream_stop(stream_state, *source_result.source);
                    break;
                }
            }
            static_cast<void>(advance(audio::PlaybackEvent::resume_requested));
            continue;
        }

        const std::size_t readable = ring.readable_frames();
        if (output_started && (readable != 0 || !ring.drained())) {
            minimum_queued_frames = std::min(minimum_queued_frames, readable);
        }
        if (readable == 0) {
            if (ring.drained()) {
                break;
            }
            if (output_started && !waiting_for_producer) {
                ++buffer_statistics.producer_starvation_events;
                waiting_for_producer = true;
            }
            if (output_started && submitted_frames < target_frames) {
                std::uint32_t padding{};
                const std::int32_t padding_status = output.get_current_padding(padding);
                if (padding_status < 0) {
                    playback_status = padding_status;
                    request_stream_stop(stream_state, *source_result.source);
                    break;
                }
                if (padding == 0) {
                    underrun_detected = true;
                    ++buffer_statistics.endpoint_underruns;
                    request_stream_stop(stream_state, *source_result.source);
                    break;
                }
            }
            std::unique_lock lock(stream_state.mutex);
            stream_state.changed.wait_for(lock, kQueueWait, [&] {
                return ring.readable_frames() != 0 || ring.closed() ||
                       stop_requested_.load(std::memory_order_acquire);
            });
            continue;
        }
        waiting_for_producer = false;

        std::size_t frames_to_pop = std::min(readable, consumer_capacity);
        if (!output_started) {
            frames_to_pop = std::min(frames_to_pop, endpoint_buffer_frames);
        }
        auto destination = std::span(consumer_samples).first(
            frames_to_pop * audio::kPcm16StereoChannelCount);
        const auto pop_result = ring.pop(destination);
        if (pop_result.frames_transferred == 0) {
            continue;
        }
        stream_state.changed.notify_one();
        audio::apply_pcm16_volume(
            destination.first(
                pop_result.frames_transferred * audio::kPcm16StereoChannelCount),
            volume_.load(std::memory_order_acquire));

        const auto submit_result = submit_frames(
            output,
            destination,
            static_cast<std::uint32_t>(pop_result.frames_transferred),
            output_started);
        submitted_frames += static_cast<SampleFrame>(submit_result.frames_submitted);
        frames_submitted_.store(submitted_frames, std::memory_order_release);
        std::uint32_t padding{};
        if (output.get_current_padding(padding) >= 0) {
            const SampleFrame rendered_frames = std::max<SampleFrame>(
                submitted_frames - static_cast<SampleFrame>(padding),
                0);
            frames_rendered_.store(rendered_frames, std::memory_order_release);
        }
        if (submit_result.status < 0 ||
            submit_result.frames_submitted != pop_result.frames_transferred) {
            playback_status = submit_result.status;
            request_stream_stop(stream_state, *source_result.source);
            break;
        }
    }

    const bool cancelled = stop_requested_.load(std::memory_order_acquire);
    if (cancelled) {
        request_stream_stop(stream_state, *source_result.source);
    }
    producer.join();

    result.frames_produced = stream_state.frames_produced;
    result.frames_submitted = submitted_frames;
    result.frames_rendered = frames_rendered_.load(std::memory_order_acquire);
    result.read_status = stream_state.terminal_status;
    result.system_error = stream_state.native_error;
    result.audio_status = playback_status;
    result.read_statistics = reliable_source.statistics();
    buffer_statistics.minimum_queued_frames =
        minimum_queued_frames == std::numeric_limits<std::size_t>::max()
            ? 0
            : minimum_queued_frames;
    result.buffer_statistics = buffer_statistics;

    const bool read_failed =
        stream_state.terminal_status != audio::ReadStatus::ok ||
        stream_state.frames_produced != target_frames;
    const bool every_frame_submitted = submitted_frames == target_frames;

    if (!cancelled && playback_status >= 0 && !read_failed &&
        every_frame_submitted && !underrun_detected) {
        static_cast<void>(advance(audio::PlaybackEvent::stream_ended));
        const std::int32_t drain_status = output.drain();
        if (drain_status < 0) {
            playback_status = drain_status;
            result.audio_status = drain_status;
        } else {
            frames_rendered_.store(target_frames, std::memory_order_release);
            result.frames_rendered = target_frames;
        }
    }
    static_cast<void>(output.stop());
    cancel_watcher.request_stop();
    cancel_watcher.join();

    if (cancelled || stop_requested_.load(std::memory_order_acquire)) {
        return finish_cancelled();
    }
    if (playback_status < 0) {
        return finish_failed(CddaPlaybackError::output_failed);
    }
    if (underrun_detected) {
        return finish_failed(CddaPlaybackError::endpoint_underrun);
    }
    if (read_failed) {
        return finish_failed(CddaPlaybackError::read_failed);
    }
    if (!every_frame_submitted || state_machine.state() != audio::PlaybackState::draining) {
        return finish_failed(CddaPlaybackError::incomplete);
    }

    static_cast<void>(advance(audio::PlaybackEvent::drain_completed));
    result.final_state = state_machine.state();
    result.error = CddaPlaybackError::none;
    return result;
}

void CddaPlaybackEngine::request_stop() noexcept
{
    stop_requested_.store(true, std::memory_order_release);
    pause_requested_.store(false, std::memory_order_release);
    control_changed_.notify_all();
}

void CddaPlaybackEngine::request_pause() noexcept
{
    pause_requested_.store(true, std::memory_order_release);
    control_changed_.notify_all();
}

void CddaPlaybackEngine::request_resume() noexcept
{
    pause_requested_.store(false, std::memory_order_release);
    control_changed_.notify_all();
}

void CddaPlaybackEngine::set_volume(const float volume) noexcept
{
    const float finite_volume = std::isfinite(volume) ? volume : 1.0F;
    volume_.store(std::clamp(finite_volume, 0.0F, 1.0F), std::memory_order_release);
}

float CddaPlaybackEngine::volume() const noexcept
{
    return volume_.load(std::memory_order_acquire);
}

CddaPlaybackProgress CddaPlaybackEngine::progress() const noexcept
{
    return {
        state_.load(std::memory_order_acquire),
        target_frames_.load(std::memory_order_acquire),
        frames_produced_.load(std::memory_order_acquire),
        frames_submitted_.load(std::memory_order_acquire),
        frames_rendered_.load(std::memory_order_acquire),
    };
}

const char* to_string(const CddaPlaybackError error) noexcept
{
    switch (error) {
    case CddaPlaybackError::none:
        return "none";
    case CddaPlaybackError::already_running:
        return "already_running";
    case CddaPlaybackError::no_ready_audio_cd:
        return "no_ready_audio_cd";
    case CddaPlaybackError::source_open_failed:
        return "source_open_failed";
    case CddaPlaybackError::invalid_stream:
        return "invalid_stream";
    case CddaPlaybackError::invalid_range:
        return "invalid_range";
    case CddaPlaybackError::output_open_failed:
        return "output_open_failed";
    case CddaPlaybackError::invalid_endpoint_buffer:
        return "invalid_endpoint_buffer";
    case CddaPlaybackError::read_failed:
        return "read_failed";
    case CddaPlaybackError::output_failed:
        return "output_failed";
    case CddaPlaybackError::endpoint_underrun:
        return "endpoint_underrun";
    case CddaPlaybackError::incomplete:
        return "incomplete";
    case CddaPlaybackError::cancelled:
        return "cancelled";
    }
    return "unknown";
}

} // namespace cd404::platform::windows
