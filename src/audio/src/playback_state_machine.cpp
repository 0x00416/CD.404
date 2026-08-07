#include <cd404/audio/playback_state_machine.hpp>

namespace cd404::audio {

PlaybackState PlaybackStateMachine::state() const noexcept
{
    return state_;
}

bool PlaybackStateMachine::apply(const PlaybackEvent event) noexcept
{
    PlaybackState next = state_;
    switch (event) {
    case PlaybackEvent::open:
        if (state_ != PlaybackState::idle) {
            return false;
        }
        next = PlaybackState::opening;
        break;
    case PlaybackEvent::source_ready:
        if (state_ != PlaybackState::opening) {
            return false;
        }
        next = PlaybackState::buffering;
        break;
    case PlaybackEvent::prebuffer_ready:
        if (state_ != PlaybackState::buffering) {
            return false;
        }
        next = PlaybackState::playing;
        break;
    case PlaybackEvent::stream_ended:
        if (state_ != PlaybackState::playing) {
            return false;
        }
        next = PlaybackState::draining;
        break;
    case PlaybackEvent::stop_requested:
        if (state_ != PlaybackState::opening &&
            state_ != PlaybackState::buffering &&
            state_ != PlaybackState::playing &&
            state_ != PlaybackState::draining) {
            return false;
        }
        next = PlaybackState::stopping;
        break;
    case PlaybackEvent::drain_completed:
        if (state_ != PlaybackState::draining) {
            return false;
        }
        next = PlaybackState::completed;
        break;
    case PlaybackEvent::cancellation_completed:
        if (state_ != PlaybackState::stopping) {
            return false;
        }
        next = PlaybackState::cancelled;
        break;
    case PlaybackEvent::failure:
        if (state_ != PlaybackState::opening &&
            state_ != PlaybackState::buffering &&
            state_ != PlaybackState::playing &&
            state_ != PlaybackState::draining &&
            state_ != PlaybackState::stopping) {
            return false;
        }
        next = PlaybackState::failed;
        break;
    case PlaybackEvent::reset:
        if (state_ != PlaybackState::completed &&
            state_ != PlaybackState::cancelled &&
            state_ != PlaybackState::failed) {
            return false;
        }
        next = PlaybackState::idle;
        break;
    }

    state_ = next;
    return true;
}

const char* to_string(const PlaybackState state) noexcept
{
    switch (state) {
    case PlaybackState::idle:
        return "idle";
    case PlaybackState::opening:
        return "opening";
    case PlaybackState::buffering:
        return "buffering";
    case PlaybackState::playing:
        return "playing";
    case PlaybackState::draining:
        return "draining";
    case PlaybackState::stopping:
        return "stopping";
    case PlaybackState::completed:
        return "completed";
    case PlaybackState::cancelled:
        return "cancelled";
    case PlaybackState::failed:
        return "failed";
    }
    return "unknown";
}

} // namespace cd404::audio
