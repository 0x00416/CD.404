#include <cd404/audio/playback_recovery.hpp>

namespace cd404::audio {

PlaybackRecoveryActions PlaybackRecoveryCoordinator::request_disc_refresh() noexcept
{
    if (disc_refresh_active_) {
        disc_refresh_pending_ = true;
        return {};
    }
    disc_refresh_active_ = true;
    PlaybackRecoveryActions actions;
    actions.refresh_disc = true;
    return actions;
}

PlaybackRecoveryActions PlaybackRecoveryCoordinator::complete_disc_refresh(
    const std::wstring_view disc_key,
    const bool disc_ready) noexcept
{
    if (!disc_refresh_active_) {
        return {};
    }

    disc_refresh_active_ = false;
    if (disc_refresh_pending_) {
        disc_refresh_pending_ = false;
        PlaybackRecoveryActions actions;
        actions.refresh_disc = true;
        actions.discard_disc_snapshot = true;
        return actions;
    }

    PlaybackRecoveryActions actions;
    actions.restart_playback =
        !resume_disc_key_.empty() && disc_ready && disc_key == resume_disc_key_;
    resume_disc_key_.clear();
    return actions;
}

PlaybackRecoveryActions PlaybackRecoveryCoordinator::media_changed(
    const bool playback_active) noexcept
{
    resume_disc_key_.clear();
    PlaybackRecoveryActions actions = request_disc_refresh();
    actions.stop_playback = playback_active;
    return actions;
}

PlaybackRecoveryActions PlaybackRecoveryCoordinator::suspend(
    const bool playback_active,
    const bool playback_paused,
    const std::wstring_view disc_key)
{
    suspended_ = true;
    resume_disc_key_ = playback_active && !playback_paused && !disc_key.empty()
        ? std::wstring(disc_key)
        : std::wstring{};
    PlaybackRecoveryActions actions;
    actions.stop_playback = playback_active;
    return actions;
}

PlaybackRecoveryActions PlaybackRecoveryCoordinator::resume() noexcept
{
    if (!suspended_) {
        return {};
    }
    suspended_ = false;
    return request_disc_refresh();
}

void PlaybackRecoveryCoordinator::begin_playback_intent() noexcept
{
    endpoint_retry_in_progress_ = false;
}

void PlaybackRecoveryCoordinator::playback_became_stable() noexcept
{
    endpoint_retry_in_progress_ = false;
}

PlaybackRecoveryActions PlaybackRecoveryCoordinator::endpoint_failed(
    const bool playback_was_active) noexcept
{
    PlaybackRecoveryActions actions;
    if (playback_was_active && !suspended_ && !endpoint_retry_in_progress_) {
        endpoint_retry_in_progress_ = true;
        actions.restart_playback = true;
    }
    return actions;
}

} // namespace cd404::audio
