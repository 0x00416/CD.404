#pragma once

#include <cstdint>

namespace cd404::ui {

constexpr std::int64_t kDefaultFrameInterval100ns = 166'667;

struct LyricTransitionFrame final {
    float progress{};
    float offset_factor{1.0F};
    float incoming_opacity{0.55F};
    bool active{};
};

[[nodiscard]] std::int64_t display_refresh_interval_100ns(
    std::uint32_t numerator,
    std::uint32_t denominator) noexcept;

[[nodiscard]] LyricTransitionFrame lyric_transition_frame(
    double elapsed_seconds,
    double duration_seconds = 0.28) noexcept;

} // namespace cd404::ui
