#pragma once

#include <cd404/audio/cdda_sector_source.hpp>
#include <cd404/audio/playback_state_machine.hpp>
#include <cd404/audio/reliable_cdda_sector_source.hpp>
#include <cd404/core/cd_time.hpp>
#include <cd404/platform/windows/optical_drive.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

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
    core::SampleFrame target_frames{};
    core::SampleFrame frames_produced{};
    core::SampleFrame frames_submitted{};
    core::SampleFrame frames_rendered{};
};

struct CddaPlaybackResult final {
    audio::PlaybackState final_state{audio::PlaybackState::idle};
    CddaPlaybackError error{CddaPlaybackError::none};
    audio::ReadStatus read_status{audio::ReadStatus::ok};
    unsigned long system_error{};
    std::int32_t audio_status{};
    unsigned int first_track_number{};
    unsigned int final_track_number{};
    core::SampleFrame target_frames{};
    core::SampleFrame frames_produced{};
    core::SampleFrame frames_submitted{};
    core::SampleFrame frames_rendered{};
    audio::ReliableReadStatistics read_statistics;
    PlaybackBufferStatistics buffer_statistics;

    [[nodiscard]] bool succeeded() const noexcept;
};

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
    [[nodiscard]] CddaPlaybackProgress progress() const noexcept;

private:
    std::atomic_bool active_{};
    std::atomic_bool stop_requested_{};
    std::atomic<audio::PlaybackState> state_{audio::PlaybackState::idle};
    std::atomic<core::SampleFrame> target_frames_{};
    std::atomic<core::SampleFrame> frames_produced_{};
    std::atomic<core::SampleFrame> frames_submitted_{};
    std::atomic<core::SampleFrame> frames_rendered_{};
};

[[nodiscard]] const char* to_string(CddaPlaybackError error) noexcept;

} // namespace cd404::platform::windows
