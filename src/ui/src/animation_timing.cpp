#include <cd404/ui/animation_timing.hpp>

#include <algorithm>
#include <cmath>

namespace cd404::ui {

std::int64_t display_refresh_interval_100ns(
    const std::uint32_t numerator,
    const std::uint32_t denominator) noexcept
{
    if (numerator == 0U || denominator == 0U) {
        return kDefaultFrameInterval100ns;
    }
    const double refresh_rate =
        static_cast<double>(numerator) / static_cast<double>(denominator);
    if (!std::isfinite(refresh_rate) || refresh_rate < 30.0 ||
        refresh_rate > 360.0) {
        return kDefaultFrameInterval100ns;
    }
    return std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(std::llround(
            10'000'000.0 / refresh_rate)));
}

LyricTransitionFrame lyric_transition_frame(
    const double elapsed_seconds,
    const double duration_seconds) noexcept
{
    if (!(duration_seconds > 0.0)) {
        return {1.0F, 0.0F, 1.0F, false};
    }
    const double linear = std::clamp(
        elapsed_seconds / duration_seconds,
        0.0,
        1.0);
    const double remaining = 1.0 - linear;
    const float eased = static_cast<float>(1.0 - remaining * remaining * remaining);
    return {
        eased,
        1.0F - eased,
        0.55F + 0.45F * eased,
        linear < 1.0,
    };
}

} // namespace cd404::ui
