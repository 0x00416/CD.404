#pragma once

namespace cd404::audio {

enum class PlaybackState {
    idle,
    opening,
    buffering,
    playing,
    draining,
    stopping,
    completed,
    cancelled,
    failed,
};

enum class PlaybackEvent {
    open,
    source_ready,
    prebuffer_ready,
    stream_ended,
    stop_requested,
    drain_completed,
    cancellation_completed,
    failure,
    reset,
};

// Small deterministic state machine shared by the playback engine and tests.
// Invalid events are rejected without changing the current state.
class PlaybackStateMachine final {
public:
    [[nodiscard]] PlaybackState state() const noexcept;
    [[nodiscard]] bool apply(PlaybackEvent event) noexcept;

private:
    PlaybackState state_{PlaybackState::idle};
};

[[nodiscard]] const char* to_string(PlaybackState state) noexcept;

} // namespace cd404::audio
