#pragma once

namespace cd404::ui {

inline constexpr float kVuMinimumDb = -22.0F;
inline constexpr float kVuMaximumDb = 5.0F;
inline constexpr float kVuReferenceDbfs = -8.0F;
inline constexpr float kVuMinimumAngleDegrees = -50.037483F;
inline constexpr float kVuZeroAngleDegrees = 25.664888F;
inline constexpr float kVuMaximumAngleDegrees = 50.037483F;

struct VuNeedleState final {
    float angle_degrees{kVuMinimumAngleDegrees};
    float angular_velocity_degrees_per_second{};
};

// Convert the playback engine's RMS dBFS value to the printed VU scale.
// The selected studio calibration is -8 dBFS = 0 VU.
[[nodiscard]] float vu_level_from_dbfs(float level_dbfs) noexcept;

// The printed radial scale is generated from this same amplitude-linear mapping.
[[nodiscard]] float vu_meter_angle_degrees(float level_db) noexcept;

// The scale is elliptical; this returns the outer tick radius for an angle.
[[nodiscard]] float vu_meter_needle_length(float angle_degrees) noexcept;

// Frame-rate-independent second-order electromechanical movement. The damping
// targets the standard VU response: about 99% in 300 ms, 1%-1.5% overshoot,
// with symmetrical rise and return.
[[nodiscard]] VuNeedleState advance_vu_needle(
    VuNeedleState state,
    float target_db,
    float elapsed_seconds) noexcept;

[[nodiscard]] bool vu_needle_is_at_rest(
    VuNeedleState state,
    float target_db) noexcept;

[[nodiscard]] float vu_backlight_opacity(bool playing, bool paused) noexcept;

} // namespace cd404::ui
