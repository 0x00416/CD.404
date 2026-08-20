#pragma once

#include <cd404/audio/cdda_sector_source.hpp>
#include <cd404/audio/playback_state_machine.hpp>
#include <cd404/audio/reliable_cdda_sector_source.hpp>
#include <cd404/core/cd_time.hpp>
#include <cd404/platform/windows/optical_drive.hpp>
#include <cd404/platform/windows/wasapi_output.hpp>

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

namespace cd404::platform::windows {

struct CddaPlaybackRequest final {
    // When omitted, the first ready drive containing the requested audio track
    // is selected. Supplying a drive keeps UI selection and engine playback in
    // the same device context.
    std::optional<OpticalDrive> drive;
    unsigned int track_number{};
    core::SampleFrame offset_frames{};
    std::optional<core::SampleFrame> maximum_frames{
        15 * core::kCdSampleFramesPerSecond};
    WasapiOpenOptions output;
};

enum class CddaPlaybackError {
    none,
    already_running,
    no_ready_audio_cd,
    source_open_failed,
    invalid_stream,
    invalid_range,
    output_open_failed,
    invalid_endpoint_buffer,
    read_failed,
    output_failed,
    endpoint_underrun,
    incomplete,
    cancelled,
};

struct PlaybackBufferStatistics final {
    std::uint64_t producer_starvation_events{};
    std::uint64_t endpoint_underruns{};
    std::size_t minimum_queued_frames{};
};

struct CddaPlaybackProgress final {
    audio::PlaybackState state{audio::PlaybackState::idle};
    std::uint64_t stream_generation{};
    std::uint64_t applied_seek_sequence{};
    unsigned int base_track_number{};
    core::SampleFrame base_track_offset_frames{};
    core::SampleFrame target_frames{};
    core::SampleFrame frames_produced{};
    core::SampleFrame frames_submitted{};
    core::SampleFrame frames_rendered{};
};

enum class DigitalClipKind : std::uint8_t {
    none,
    true_peak_over,
    hard_sample_clip,
};

struct StereoTruePeakAnalysis final {
    float left_dbtp{-120.0F};
    float right_dbtp{-120.0F};
    DigitalClipKind left_clip{DigitalClipKind::none};
    DigitalClipKind right_clip{DigitalClipKind::none};
};

class StereoPcm16TruePeakMeter final {
public:
    void reset() noexcept;
    [[nodiscard]] StereoTruePeakAnalysis process(
        std::span<const std::int16_t> samples) noexcept;

private:
    struct ChannelState final {
        std::array<double, 12> history{};
        int rail_sign{};
        unsigned int rail_run{};
    };

    ChannelState left_;
    ChannelState right_;
};

struct StereoMeterLevels final {
    float left_dbfs{-120.0F};
    float right_dbfs{-120.0F};
    float left_true_peak_dbtp{-120.0F};
    float right_true_peak_dbtp{-120.0F};
    DigitalClipKind left_clip{DigitalClipKind::none};
    DigitalClipKind right_clip{DigitalClipKind::none};
};

// Converts a linear RMS amplitude to sine-referenced dBFS, where a full-scale
// sine reads 0 dBFS rather than -3.0103 dBFS.
[[nodiscard]] float sine_referenced_dbfs_from_rms(double rms_amplitude) noexcept;

struct CddaPlaybackResult final {
    audio::PlaybackState final_state{audio::PlaybackState::idle};
    CddaPlaybackError error{CddaPlaybackError::none};
    audio::ReadStatus read_status{audio::ReadStatus::ok};
    unsigned long system_error{};
    std::int32_t audio_status{};
    WasapiOpenResult output_open_result;
    bool used_default_output_endpoint{true};
    unsigned int first_track_number{};
    unsigned int final_track_number{};
    core::SampleFrame target_frames{};
    core::SampleFrame frames_produced{};
    core::SampleFrame frames_submitted{};
    core::SampleFrame frames_rendered{};
    audio::ReliableReadStatistics read_statistics;
    PlaybackBufferStatistics buffer_statistics;
    std::uint64_t session_seek_count{};

    [[nodiscard]] bool succeeded() const noexcept;
};

enum class CddaSeekRequestResult {
    queued,
    not_active,
    invalid_track,
    invalid_range,
    outside_session,
};

struct CddaSeekRequestReceipt final {
    CddaSeekRequestResult result{CddaSeekRequestResult::not_active};
    std::uint64_t sequence{};
};

struct CddaSeekCommand final {
    unsigned int track_number{};
    core::SampleFrame offset_frames{};
    std::uint64_t sequence{};
};

// Latest-wins mailbox. Callers provide synchronization so the same behavior
// can be exercised deterministically without a CD drive or WASAPI endpoint.
class LatestCddaSeekCommand final {
public:
    [[nodiscard]] CddaSeekRequestReceipt queue(
        unsigned int track_number,
        core::SampleFrame offset_frames) noexcept;
    [[nodiscard]] std::optional<CddaSeekCommand> take_latest() noexcept;
    [[nodiscard]] bool has_pending() const noexcept;
    void reset() noexcept;

private:
    std::optional<CddaSeekCommand> pending_;
    std::uint64_t next_sequence_{};
};

struct CddaSessionSeekPlan final {
    CddaSeekRequestResult result{CddaSeekRequestResult::invalid_track};
    core::SampleFrame stream_offset_frames{};
    core::SampleFrame remaining_frames{};
};

[[nodiscard]] CddaSessionSeekPlan plan_cdda_session_seek(
    const disc::Toc& toc,
    unsigned int session_first_track,
    unsigned int session_final_track,
    unsigned int target_track,
    core::SampleFrame target_offset_frames) noexcept;

// These classifiers keep Win32/WASAPI error details at the platform boundary
// and expose deterministic recovery decisions to the UI and synthetic tests.
[[nodiscard]] bool is_recoverable_default_endpoint_failure(
    const CddaPlaybackResult& result) noexcept;
[[nodiscard]] bool is_media_unavailable_failure(
    const CddaPlaybackResult& result) noexcept;

// A reusable blocking playback session. Call play() on a worker thread when it
// is used by a UI. request_stop() is thread-safe and interrupts pending raw-CD
// and WASAPI operations through the session's cancellation watcher.
class CddaPlaybackEngine final {
public:
    CddaPlaybackEngine() noexcept = default;

    CddaPlaybackEngine(const CddaPlaybackEngine&) = delete;
    CddaPlaybackEngine& operator=(const CddaPlaybackEngine&) = delete;
    CddaPlaybackEngine(CddaPlaybackEngine&&) = delete;
    CddaPlaybackEngine& operator=(CddaPlaybackEngine&&) = delete;

    [[nodiscard]] CddaPlaybackResult play(const CddaPlaybackRequest& request);
    void request_stop() noexcept;
    void request_pause() noexcept;
    void request_resume() noexcept;
    [[nodiscard]] CddaSeekRequestReceipt request_seek(
        unsigned int track_number,
        core::SampleFrame offset_frames) noexcept;
    void set_volume(float volume) noexcept;
    [[nodiscard]] float volume() const noexcept;
    [[nodiscard]] CddaPlaybackProgress progress() const noexcept;
    [[nodiscard]] StereoMeterLevels meter_levels() noexcept;

private:
    std::atomic_bool active_{};
    std::atomic_bool stop_requested_{};
    std::atomic_bool pause_requested_{};
    std::mutex control_mutex_;
    std::condition_variable control_changed_;
    LatestCddaSeekCommand seek_commands_;
    std::atomic<float> volume_{1.0F};
    std::atomic<audio::PlaybackState> state_{audio::PlaybackState::idle};
    std::atomic<std::uint64_t> stream_generation_{};
    std::atomic<std::uint64_t> applied_seek_sequence_{};
    std::atomic<unsigned int> base_track_number_{};
    std::atomic<core::SampleFrame> base_track_offset_frames_{};
    std::atomic<unsigned int> session_first_track_number_{};
    std::atomic<unsigned int> session_final_track_number_{};
    std::atomic<core::SampleFrame> target_frames_{};
    std::atomic<core::SampleFrame> frames_produced_{};
    std::atomic<core::SampleFrame> frames_submitted_{};
    std::atomic<core::SampleFrame> frames_rendered_{};
    std::atomic<float> left_meter_dbfs_{-120.0F};
    std::atomic<float> right_meter_dbfs_{-120.0F};
    std::atomic<float> left_true_peak_dbtp_{-120.0F};
    std::atomic<float> right_true_peak_dbtp_{-120.0F};
    std::atomic<DigitalClipKind> left_clip_{DigitalClipKind::none};
    std::atomic<DigitalClipKind> right_clip_{DigitalClipKind::none};
};

[[nodiscard]] const char* to_string(CddaPlaybackError error) noexcept;
[[nodiscard]] const char* to_string(CddaSeekRequestResult result) noexcept;

} // namespace cd404::platform::windows
