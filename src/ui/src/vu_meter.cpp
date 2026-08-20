#include <cd404/ui/vu_meter.hpp>

#include <algorithm>
#include <cmath>

namespace cd404::ui {

float vu_level_from_dbfs(const float level_dbfs) noexcept
{
    if (!std::isfinite(level_dbfs)) {
        return kVuMinimumDb;
    }
    return std::clamp(
        level_dbfs - kVuReferenceDbfs,
        kVuMinimumDb,
        kVuMaximumDb);
}

float vu_meter_angle_degrees(const float level_db) noexcept
{
    // Sony 3 angles after the reference card's 660x328 layout is fitted to
    // the application's 448.5x167 meter card.
    const float bounded = std::clamp(level_db, kVuMinimumDb, kVuMaximumDb);
    const float amplitude = std::pow(10.0F, bounded / 20.0F);
    if (bounded <= 0.0F) {
        const float minimum_amplitude = std::pow(10.0F, kVuMinimumDb / 20.0F);
        const float position =
            (amplitude - minimum_amplitude) / (1.0F - minimum_amplitude);
        return kVuMinimumAngleDegrees +
            position * (kVuZeroAngleDegrees - kVuMinimumAngleDegrees);
    }
    const float maximum_amplitude = std::pow(10.0F, kVuMaximumDb / 20.0F);
    const float position = (amplitude - 1.0F) / (maximum_amplitude - 1.0F);
    return kVuZeroAngleDegrees +
        position * (kVuMaximumAngleDegrees - kVuZeroAngleDegrees);
}

float vu_meter_needle_length(const float angle_degrees) noexcept
{
    constexpr float ellipse_radius_x = 320.0F;
    constexpr float ellipse_radius_y = 150.0F;
    constexpr float major_tick_length = 24.0F;
    const float radians = angle_degrees * 3.14159265358979323846F / 180.0F;
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    const float denominator = std::sqrt(
        (sine * sine) / (ellipse_radius_x * ellipse_radius_x) +
        (cosine * cosine) / (ellipse_radius_y * ellipse_radius_y));
    return 1.0F / denominator + major_tick_length;
}

VuNeedleState advance_vu_needle(
    VuNeedleState state,
    const float target_db,
    const float elapsed_seconds) noexcept
{
    if (!std::isfinite(state.angle_degrees) ||
        !std::isfinite(state.angular_velocity_degrees_per_second)) {
        state = {};
    }

    const float elapsed = std::clamp(elapsed_seconds, 0.0F, 0.1F);
    if (elapsed == 0.0F) {
        return state;
    }

    // Exact solution of x'' + 2*zeta*omega*x' + omega^2*x = 0. Using the
    // closed form avoids frame-rate-dependent motion at 60/120/144 Hz.
    constexpr float natural_frequency = 14.0F;
    constexpr float damping_ratio = 0.82F;
    const float damped_frequency = natural_frequency *
        std::sqrt(1.0F - damping_ratio * damping_ratio);
    constexpr float damping = damping_ratio * natural_frequency;

    const float target_angle = vu_meter_angle_degrees(target_db);
    const float displacement = state.angle_degrees - target_angle;
    const float velocity = state.angular_velocity_degrees_per_second;
    const float phase = damped_frequency * elapsed;
    const float sine = std::sin(phase);
    const float cosine = std::cos(phase);
    const float decay = std::exp(-damping * elapsed);
    const float next_displacement = decay * (
        displacement * cosine +
        (velocity + damping * displacement) / damped_frequency * sine);
    const float next_velocity = decay * (
        velocity * cosine -
        (damping * velocity + natural_frequency * natural_frequency *
         displacement) / damped_frequency * sine);

    state.angle_degrees = target_angle + next_displacement;
    state.angular_velocity_degrees_per_second = next_velocity;
    if (state.angle_degrees <= kVuMinimumAngleDegrees &&
        state.angular_velocity_degrees_per_second < 0.0F) {
        state.angle_degrees = kVuMinimumAngleDegrees;
        state.angular_velocity_degrees_per_second = 0.0F;
    } else if (state.angle_degrees >= kVuMaximumAngleDegrees &&
               state.angular_velocity_degrees_per_second > 0.0F) {
        state.angle_degrees = kVuMaximumAngleDegrees;
        state.angular_velocity_degrees_per_second = 0.0F;
    }
    if (std::abs(state.angle_degrees - target_angle) < 0.005F &&
        std::abs(state.angular_velocity_degrees_per_second) < 0.05F) {
        state.angle_degrees = target_angle;
        state.angular_velocity_degrees_per_second = 0.0F;
    }
    return state;
}

bool vu_needle_is_at_rest(
    const VuNeedleState state,
    const float target_db) noexcept
{
    return std::abs(state.angle_degrees - vu_meter_angle_degrees(target_db)) < 0.01F &&
        std::abs(state.angular_velocity_degrees_per_second) < 0.1F;
}

float vu_backlight_opacity(const bool playing, const bool paused) noexcept
{
    if (playing && !paused) {
        return 0.34F;
    }
    if (paused) {
        return 0.20F;
    }
    return 0.10F;
}

} // namespace cd404::ui
