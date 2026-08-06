#pragma once

#include <cd404/core/cd_time.hpp>

#include <cstddef>
#include <span>

namespace cd404::audio {

// A CD-DA data block returned by an MMC READ CD operation contains signed
// 16-bit stereo samples in this byte order:
//   left low, left high, right low, right high.
// This is already the PCM16LE layout consumed by Windows audio APIs.
inline constexpr int kCddaPcmSampleRate = 44'100;
inline constexpr int kCddaPcmChannelCount = 2;
inline constexpr int kCddaPcmBitsPerSample = 16;

enum class CddaPcmConversionStatus {
    ok,
    source_not_frame_aligned,
    destination_too_small,
};

struct CddaPcmConversionResult final {
    CddaPcmConversionStatus status{CddaPcmConversionStatus::ok};
    std::size_t frames_written{};
};

[[nodiscard]] constexpr bool is_cdda_pcm_frame_aligned(
    const std::size_t byte_count) noexcept
{
    return byte_count %
               static_cast<std::size_t>(core::kCdBytesPerSampleFrame) ==
           0;
}

// Copies raw CD-DA frames into a Windows PCM16LE buffer. The spans may
// overlap. Bytes beyond source.size() in destination are left unchanged.
[[nodiscard]] CddaPcmConversionResult copy_cdda_to_pcm16le(
    std::span<const std::byte> source,
    std::span<std::byte> destination) noexcept;

// Validates a raw CD-DA buffer for direct submission as Windows PCM16LE.
// No sample bytes are changed because MMC CD-DA and PCM16LE have the same
// channel and byte order.
[[nodiscard]] CddaPcmConversionStatus convert_cdda_to_pcm16le_in_place(
    std::span<std::byte> samples) noexcept;

} // namespace cd404::audio
