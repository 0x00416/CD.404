#pragma once

#include <cstdint>
#include <span>

namespace cd404::audio {

// Applies a linear gain to interleaved PCM16 samples. Gain is clamped to
// [0, 1]; unity gain is an exact no-op so the default path preserves samples.
void apply_pcm16_volume(
    std::span<std::int16_t> samples,
    float gain) noexcept;

} // namespace cd404::audio
