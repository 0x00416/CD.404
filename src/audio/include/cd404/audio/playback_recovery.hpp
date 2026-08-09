#pragma once

#include <string>
#include <string_view>

namespace cd404::audio {

struct PlaybackRecoveryActions final {
    bool stop_playback{};
    bool refresh_disc{};
    bool discard_disc_snapshot{};
    bool restart_playback{};
};

// Deterministic policy for hardware lifecycle events. The Win32 UI translates
// device and power notifications into these calls, while tests can inject the
// same semantic events without an optical drive or audio endpoint.
class PlaybackRecoveryCoordinator final {
public:
    [[nodiscard]] PlaybackRecoveryActions request_disc_refresh() noexcept;
    [[nodiscard]] PlaybackRecoveryActions complete_disc_refresh(
        std::wstring_view disc_key,
        bool disc_ready) noexcept;
    [[nodiscard]] PlaybackRecoveryActions media_changed(
        bool playback_active) noexcept;
    [[nodiscard]] PlaybackRecoveryActions suspend(
        bool playback_active,
        bool playback_paused,
        std::wstring_view disc_key);
    [[nodiscard]] PlaybackRecoveryActions resume() noexcept;

    void begin_playback_intent() noexcept;
    void playback_became_stable() noexcept;
    [[nodiscard]] PlaybackRecoveryActions endpoint_failed(
        bool playback_was_active) noexcept;

private:
    bool disc_refresh_active_{};
    bool disc_refresh_pending_{};
    bool suspended_{};
    bool endpoint_retry_in_progress_{};
    std::wstring resume_disc_key_;
};

} // namespace cd404::audio
