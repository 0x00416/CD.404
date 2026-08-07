#include <cd404/audio/pcm16_volume.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cd404::audio {

void apply_pcm16_volume(
    const std::span<std::int16_t> samples,
    const float gain) noexcept
{
    const float finite_gain = std::isfinite(gain) ? gain : 1.0F;
    const float clamped_gain = std::clamp(finite_gain, 0.0F, 1.0F);
    if (clamped_gain == 1.0F) {
        return;
    }

    constexpr std::int32_t kScale = 65'536;
    const auto multiplier = static_cast<std::int32_t>(
        std::lround(clamped_gain * static_cast<float>(kScale)));
    for (auto& sample : samples) {
        const std::int32_t scaled =
            static_cast<std::int32_t>(sample) * multiplier / kScale;
        sample = static_cast<std::int16_t>(scaled);
    }
}

} // namespace cd404::audio
