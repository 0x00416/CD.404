#include <windows.h>
#include <audioclient.h>

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

float sine_referenced_dbfs_from_rms(const double rms_amplitude) noexcept
{
    if (!std::isfinite(rms_amplitude) || rms_amplitude <= 0.0) {
        return -120.0F;
    }
    constexpr double rms_to_full_scale_sine = 1.4142135623730950488;
    const double sine_referenced_amplitude =
        std::max(rms_amplitude * rms_to_full_scale_sine, 1.0e-6);
    return static_cast<float>(std::clamp(
        20.0 * std::log10(sine_referenced_amplitude), -120.0, 0.0));
}

void StereoPcm16TruePeakMeter::reset() noexcept
{
    left_ = {};
    right_ = {};
}

StereoTruePeakAnalysis StereoPcm16TruePeakMeter::process(
    const std::span<const std::int16_t> samples) noexcept
{
    // ITU-R BS.1770 Annex 2: order-48, four-phase FIR interpolator.
    static constexpr std::array<std::array<double, 4>, 12> coefficients{{
        {{ 0.0017089843750, -0.0291748046875, -0.0189208984375, -0.0083007812500}},
        {{ 0.0109863281250,  0.0292968750000,  0.0330810546875,  0.0148925781250}},
        {{-0.0196533203125, -0.0517578125000, -0.0582275390625, -0.0266113281250}},
        {{ 0.0332031250000,  0.0891113281250,  0.1015625000000,  0.0476074218750}},
        {{-0.0594482421875, -0.1665039062500, -0.2003173828125, -0.1022949218750}},
        {{ 0.1373291015625,  0.4650878906250,  0.7797851562500,  0.9721679687500}},
        {{ 0.9721679687500,  0.7797851562500,  0.4650878906250,  0.1373291015625}},
        {{-0.1022949218750, -0.2003173828125, -0.1665039062500, -0.0594482421875}},
        {{ 0.0476074218750,  0.1015625000000,  0.0891113281250,  0.0332031250000}},
        {{-0.0266113281250, -0.0582275390625, -0.0517578125000, -0.0196533203125}},
        {{ 0.0148925781250,  0.0330810546875,  0.0292968750000,  0.0109863281250}},
        {{-0.0083007812500, -0.0189208984375, -0.0291748046875,  0.0017089843750}},
    }};

    double left_peak{};
    double right_peak{};
    bool left_hard_clip{};
    bool right_hard_clip{};
    const auto process_channel = [&](
        ChannelState& state,
        const std::int16_t sample,
        double& peak,
        bool& hard_clip) {
        std::move_backward(
            state.history.begin(), state.history.end() - 1, state.history.end());
        const double normalized = static_cast<double>(sample) / 32768.0;
        state.history.front() = normalized;
        peak = std::max(peak, std::abs(normalized));
        for (std::size_t phase = 0; phase < 4; ++phase) {
            double interpolated{};
            for (std::size_t tap = 0; tap < state.history.size(); ++tap) {
                interpolated += state.history[tap] * coefficients[tap][phase];
            }
            peak = std::max(peak, std::abs(interpolated));
        }

        const int rail_sign = sample == std::numeric_limits<std::int16_t>::max()
            ? 1
            : sample == std::numeric_limits<std::int16_t>::min() ? -1 : 0;
        if (rail_sign == 0) {
            state.rail_sign = 0;
            state.rail_run = 0;
        } else if (rail_sign == state.rail_sign) {
            ++state.rail_run;
        } else {
            state.rail_sign = rail_sign;
            state.rail_run = 1;
        }
        hard_clip = hard_clip || state.rail_run >= 3;
    };

    const std::size_t frames = samples.size() / audio::kPcm16StereoChannelCount;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        process_channel(
            left_, samples[frame * 2], left_peak, left_hard_clip);
        process_channel(
            right_, samples[frame * 2 + 1], right_peak, right_hard_clip);
    }
    const auto to_dbtp = [](const double peak) {
        return static_cast<float>(20.0 * std::log10(std::max(peak, 1.0e-6)));
    };
    const auto clip_kind = [](const bool hard_clip, const double peak) {
        if (hard_clip) {
            return DigitalClipKind::hard_sample_clip;
        }
        return peak > 1.0
            ? DigitalClipKind::true_peak_over
            : DigitalClipKind::none;
    };
    return {
        to_dbtp(left_peak),
        to_dbtp(right_peak),
        clip_kind(left_hard_clip, left_peak),
        clip_kind(right_hard_clip, right_peak),
    };
}

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

[[nodiscard]] StereoMeterLevels measure_stereo_pcm16(
    const std::span<const std::int16_t> samples) noexcept
{
    if (samples.size() < audio::kPcm16StereoChannelCount) {
        return {};
    }
    double left_sum{};
    double right_sum{};
    const std::size_t frames = samples.size() / audio::kPcm16StereoChannelCount;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto left = static_cast<std::int32_t>(samples[frame * 2]);
        const auto right = static_cast<std::int32_t>(samples[frame * 2 + 1]);
        const double normalized_left = static_cast<double>(left) / 32768.0;
        const double normalized_right = static_cast<double>(right) / 32768.0;
        left_sum += normalized_left * normalized_left;
        right_sum += normalized_right * normalized_right;
    }
    const auto to_dbfs = [frames](const double sum) {
        const double rms = std::sqrt(sum / static_cast<double>(frames));
        return sine_referenced_dbfs_from_rms(rms);
    };
    return {to_dbfs(left_sum), to_dbfs(right_sum)};
}

void latch_clip_kind(
    std::atomic<DigitalClipKind>& destination,
    const DigitalClipKind value) noexcept
{
    auto current = destination.load(std::memory_order_acquire);
    while (static_cast<unsigned int>(current) < static_cast<unsigned int>(value) &&
           !destination.compare_exchange_weak(
               current, value, std::memory_order_acq_rel)) {
    }
}

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

CddaSessionSeekPlan plan_cdda_session_seek(
    const disc::Toc& toc,
    const unsigned int session_first_track,
    const unsigned int session_final_track,
    const unsigned int target_track,
    const core::SampleFrame target_offset_frames) noexcept
{
    const auto& tracks = toc.tracks();
    const std::size_t first_index = find_track_index(toc, session_first_track);
    const std::size_t final_index = find_track_index(toc, session_final_track);
    const auto* const target = toc.find_track(
        static_cast<std::uint8_t>(target_track));
    if (target == nullptr || !target->is_audio) {
        return {CddaSeekRequestResult::invalid_track, 0, 0};
    }
    if (first_index >= tracks.size() || final_index >= tracks.size() ||
        first_index > final_index) {
        return {CddaSeekRequestResult::outside_session, 0, 0};
    }
    for (std::size_t index = first_index; index <= final_index; ++index) {
        if (!tracks[index].is_audio ||
            (index != first_index &&
             tracks[index - 1].end_lba != tracks[index].start_lba)) {
            return {CddaSeekRequestResult::outside_session, 0, 0};
        }
    }
    const std::size_t target_index = static_cast<std::size_t>(
        target - tracks.data());
    if (target_index < first_index || target_index > final_index) {
        return {CddaSeekRequestResult::outside_session, 0, 0};
    }
    if (target_offset_frames < 0 ||
        target_offset_frames >= target->frame_count) {
        return {CddaSeekRequestResult::invalid_range, 0, 0};
    }

    const auto& first = tracks[first_index];
    const auto& final = tracks[final_index];
    const core::SampleFrame stream_offset =
        target->start_disc_frame - first.start_disc_frame + target_offset_frames;
    const core::SampleFrame stream_end =
        final.start_disc_frame - first.start_disc_frame + final.frame_count;
    return {
        CddaSeekRequestResult::queued,
        stream_offset,
        stream_end - stream_offset,
    };
}

CddaSeekRequestReceipt LatestCddaSeekCommand::queue(
    const unsigned int track_number,
    const core::SampleFrame offset_frames) noexcept
{
    const std::uint64_t sequence = ++next_sequence_;
    pending_ = CddaSeekCommand{track_number, offset_frames, sequence};
    return {CddaSeekRequestResult::queued, sequence};
}

std::optional<CddaSeekCommand> LatestCddaSeekCommand::take_latest() noexcept
{
    auto command = pending_;
    pending_.reset();
    return command;
}

bool LatestCddaSeekCommand::has_pending() const noexcept
{
    return pending_.has_value();
}

void LatestCddaSeekCommand::reset() noexcept
{
    pending_.reset();
    next_sequence_ = 0;
}

bool is_recoverable_default_endpoint_failure(
    const CddaPlaybackResult& result) noexcept
{
    if (!result.used_default_output_endpoint ||
        (result.error != CddaPlaybackError::output_open_failed &&
         result.error != CddaPlaybackError::output_failed)) {
        return false;
    }

    switch (static_cast<HRESULT>(result.audio_status)) {
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_RESOURCES_INVALIDATED:
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return true;
    default:
        return false;
    }
}

bool is_media_unavailable_failure(const CddaPlaybackResult& result) noexcept
{
    if (result.error == CddaPlaybackError::no_ready_audio_cd) {
        return true;
    }
    if (result.error != CddaPlaybackError::source_open_failed &&
        result.error != CddaPlaybackError::read_failed) {
        return false;
    }

    switch (result.system_error) {
    case ERROR_NOT_READY:
    case ERROR_MEDIA_CHANGED:
    case ERROR_NO_MEDIA_IN_DRIVE:
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_INVALID_HANDLE:
        return true;
    default:
        return false;
    }
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
    {
        std::scoped_lock lock(control_mutex_);
        seek_commands_.reset();
    }
    stream_generation_.store(0, std::memory_order_release);
    applied_seek_sequence_.store(0, std::memory_order_release);
    base_track_number_.store(0, std::memory_order_release);
    base_track_offset_frames_.store(0, std::memory_order_release);
    session_first_track_number_.store(0, std::memory_order_release);
    session_final_track_number_.store(0, std::memory_order_release);
    target_frames_.store(0, std::memory_order_release);
    frames_produced_.store(0, std::memory_order_release);
    frames_submitted_.store(0, std::memory_order_release);
    frames_rendered_.store(0, std::memory_order_release);
    left_meter_dbfs_.store(-120.0F, std::memory_order_release);
    right_meter_dbfs_.store(-120.0F, std::memory_order_release);
    left_true_peak_dbtp_.store(-120.0F, std::memory_order_release);
    right_true_peak_dbtp_.store(-120.0F, std::memory_order_release);
    left_clip_.store(DigitalClipKind::none, std::memory_order_release);
    right_clip_.store(DigitalClipKind::none, std::memory_order_release);

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
    std::size_t run_start_index = selected_track_index;
    while (run_start_index > 0 && tracks[run_start_index - 1].is_audio &&
           tracks[run_start_index - 1].end_lba ==
               tracks[run_start_index].start_lba) {
        --run_start_index;
    }
    std::size_t run_end_index = selected_track_index;
    while (run_end_index + 1 < tracks.size() &&
           tracks[run_end_index + 1].is_audio &&
           tracks[run_end_index].end_lba == tracks[run_end_index + 1].start_lba) {
        ++run_end_index;
    }
    const auto& selected_track = tracks[selected_track_index];
    const auto& first_track = tracks[run_start_index];
    const auto& final_track = tracks[run_end_index];
    result.first_track_number = selected_track.number;
    result.final_track_number = final_track.number;

    auto source_result = open_raw_cdda_source(
        *selected_drive,
        first_track.start_lba,
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
        first_track.start_lba,
        final_track.end_lba);
    if (!stream.valid()) {
        return finish_failed(CddaPlaybackError::invalid_stream);
    }
    const auto initial_seek = plan_cdda_session_seek(
        *selected_toc,
        first_track.number,
        final_track.number,
        selected_track.number,
        request.offset_frames);
    if (initial_seek.result != CddaSeekRequestResult::queued ||
        !stream.seek(initial_seek.stream_offset_frames)) {
        return finish_failed(CddaPlaybackError::invalid_range);
    }

    SampleFrame target_frames = initial_seek.remaining_frames;
    if (request.maximum_frames) {
        target_frames = std::min(target_frames, *request.maximum_frames);
    }
    if (target_frames <= 0) {
        return finish_failed(CddaPlaybackError::invalid_range);
    }
    result.target_frames = target_frames;
    result.used_default_output_endpoint = request.output.endpoint_id.empty();
    target_frames_.store(target_frames, std::memory_order_release);
    stream_generation_.store(1, std::memory_order_release);
    base_track_number_.store(selected_track.number, std::memory_order_release);
    base_track_offset_frames_.store(request.offset_frames, std::memory_order_release);
    session_first_track_number_.store(first_track.number, std::memory_order_release);
    session_final_track_number_.store(final_track.number, std::memory_order_release);

    WasapiOutput output;
    result.output_open_result = output.open(request.output);
    if (!result.output_open_result.succeeded()) {
        result.audio_status = result.output_open_result.status;
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
    StereoPcm16TruePeakMeter true_peak_meter;
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
    std::thread producer;
    const auto start_producer = [&] {
        producer = std::thread(
            produce_pcm,
            ProducerContext{
                stream,
                ring,
                stream_state,
                frames_produced_,
                target_frames,
            });
        std::unique_lock lock(stream_state.mutex);
        const std::size_t prebuffer_target =
            std::min(kPrebufferFrames, static_cast<std::size_t>(target_frames));
        while (ring.readable_frames() < prebuffer_target && !ring.closed() &&
               !stop_requested_.load(std::memory_order_acquire)) {
            stream_state.changed.wait_for(lock, kQueueWait);
        }
    };
    start_producer();

    SampleFrame submitted_frames{};
    bool output_started{};
    bool underrun_detected{};
    bool waiting_for_producer{};
    PlaybackBufferStatistics buffer_statistics;
    std::size_t minimum_queued_frames{std::numeric_limits<std::size_t>::max()};
    std::int32_t playback_status{S_OK};
    static_cast<void>(advance(audio::PlaybackEvent::prebuffer_ready));

    const auto take_pending_seek = [this]() -> std::optional<CddaSeekCommand> {
        std::scoped_lock lock(control_mutex_);
        return seek_commands_.take_latest();
    };

    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (const auto pending_seek = take_pending_seek()) {
            const auto plan = plan_cdda_session_seek(
                *selected_toc,
                first_track.number,
                final_track.number,
                pending_seek->track_number,
                pending_seek->offset_frames);
            if (plan.result == CddaSeekRequestResult::queued) {
                stream_state.stop_requested.store(true, std::memory_order_release);
                stream_state.changed.notify_all();
                if (producer.joinable()) {
                    producer.join();
                }
                playback_status = output.stop();
                if (playback_status < 0 ||
                    !audio::reposition_cdda_stream(
                        stream,
                        ring,
                        plan.stream_offset_frames)) {
                    break;
                }

                output_started = false;
                stream_state.stop_requested.store(false, std::memory_order_release);
                stream_state.terminal_status = audio::ReadStatus::ok;
                stream_state.native_error = 0;
                stream_state.frames_produced = 0;
                target_frames = plan.remaining_frames;
                if (request.maximum_frames) {
                    target_frames = std::min(target_frames, *request.maximum_frames);
                }
                submitted_frames = 0;
                true_peak_meter.reset();
                frames_produced_.store(0, std::memory_order_release);
                frames_submitted_.store(0, std::memory_order_release);
                frames_rendered_.store(0, std::memory_order_release);
                target_frames_.store(target_frames, std::memory_order_release);
                base_track_number_.store(
                    pending_seek->track_number,
                    std::memory_order_release);
                base_track_offset_frames_.store(
                    pending_seek->offset_frames,
                    std::memory_order_release);
                stream_generation_.fetch_add(1, std::memory_order_acq_rel);
                applied_seek_sequence_.store(
                    pending_seek->sequence,
                    std::memory_order_release);
                result.first_track_number = pending_seek->track_number;
                result.target_frames = target_frames;
                ++result.session_seek_count;
                waiting_for_producer = false;
                start_producer();
                continue;
            }
        }
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
                           !pause_requested_.load(std::memory_order_acquire) ||
                           seek_commands_.has_pending();
                });
            }
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }
            if (pause_requested_.load(std::memory_order_acquire)) {
                continue;
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
        const auto meter_samples = destination.first(
            pop_result.frames_transferred * audio::kPcm16StereoChannelCount);
        const StereoTruePeakAnalysis source_peak = true_peak_meter.process(meter_samples);
        const float gain = std::clamp(
            volume_.load(std::memory_order_acquire), 0.0F, 1.0F);
        audio::apply_pcm16_volume(meter_samples, gain);
        const StereoMeterLevels levels = measure_stereo_pcm16(
            meter_samples);
        left_meter_dbfs_.store(levels.left_dbfs, std::memory_order_release);
        right_meter_dbfs_.store(levels.right_dbfs, std::memory_order_release);
        const float gain_db = gain > 0.0F
            ? 20.0F * std::log10(gain)
            : -120.0F;
        const float left_output_dbtp = source_peak.left_dbtp + gain_db;
        const float right_output_dbtp = source_peak.right_dbtp + gain_db;
        left_true_peak_dbtp_.store(left_output_dbtp, std::memory_order_release);
        right_true_peak_dbtp_.store(right_output_dbtp, std::memory_order_release);
        const auto output_clip = [](
            const DigitalClipKind source_clip,
            const float output_dbtp) {
            if (source_clip == DigitalClipKind::hard_sample_clip) {
                return DigitalClipKind::hard_sample_clip;
            }
            return output_dbtp > 0.0F
                ? DigitalClipKind::true_peak_over
                : DigitalClipKind::none;
        };
        latch_clip_kind(left_clip_, output_clip(source_peak.left_clip, left_output_dbtp));
        latch_clip_kind(right_clip_, output_clip(source_peak.right_clip, right_output_dbtp));

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
    if (producer.joinable()) {
        producer.join();
    }

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
    output.close();

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

CddaSeekRequestReceipt CddaPlaybackEngine::request_seek(
    const unsigned int track_number,
    const core::SampleFrame offset_frames) noexcept
{
    if (track_number == 0) {
        return {CddaSeekRequestResult::invalid_track, 0};
    }
    if (offset_frames < 0) {
        return {CddaSeekRequestResult::invalid_range, 0};
    }
    if (!active_.load(std::memory_order_acquire)) {
        return {CddaSeekRequestResult::not_active, 0};
    }

    const unsigned int first =
        session_first_track_number_.load(std::memory_order_acquire);
    const unsigned int final =
        session_final_track_number_.load(std::memory_order_acquire);
    if (first == 0 || track_number < first || track_number > final) {
        return {CddaSeekRequestResult::outside_session, 0};
    }

    CddaSeekRequestReceipt receipt;
    {
        std::scoped_lock lock(control_mutex_);
        if (!active_.load(std::memory_order_acquire)) {
            return {CddaSeekRequestResult::not_active, 0};
        }
        receipt = seek_commands_.queue(track_number, offset_frames);
    }
    control_changed_.notify_all();
    return receipt;
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
        stream_generation_.load(std::memory_order_acquire),
        applied_seek_sequence_.load(std::memory_order_acquire),
        base_track_number_.load(std::memory_order_acquire),
        base_track_offset_frames_.load(std::memory_order_acquire),
        target_frames_.load(std::memory_order_acquire),
        frames_produced_.load(std::memory_order_acquire),
        frames_submitted_.load(std::memory_order_acquire),
        frames_rendered_.load(std::memory_order_acquire),
    };
}

StereoMeterLevels CddaPlaybackEngine::meter_levels() noexcept
{
    return {
        left_meter_dbfs_.load(std::memory_order_acquire),
        right_meter_dbfs_.load(std::memory_order_acquire),
        left_true_peak_dbtp_.load(std::memory_order_acquire),
        right_true_peak_dbtp_.load(std::memory_order_acquire),
        left_clip_.exchange(DigitalClipKind::none, std::memory_order_acq_rel),
        right_clip_.exchange(DigitalClipKind::none, std::memory_order_acq_rel),
    };
}

const char* to_string(const CddaSeekRequestResult result) noexcept
{
    switch (result) {
    case CddaSeekRequestResult::queued:
        return "queued";
    case CddaSeekRequestResult::not_active:
        return "not_active";
    case CddaSeekRequestResult::invalid_track:
        return "invalid_track";
    case CddaSeekRequestResult::invalid_range:
        return "invalid_range";
    case CddaSeekRequestResult::outside_session:
        return "outside_session";
    }
    return "unknown";
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
