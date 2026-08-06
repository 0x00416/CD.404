#include <cd404/audio/cdda_pcm.hpp>

#include <cstring>

namespace cd404::audio {

CddaPcmConversionResult copy_cdda_to_pcm16le(
    const std::span<const std::byte> source,
    const std::span<std::byte> destination) noexcept
{
    if (!is_cdda_pcm_frame_aligned(source.size())) {
        return {CddaPcmConversionStatus::source_not_frame_aligned, 0};
    }

    if (destination.size() < source.size()) {
        return {CddaPcmConversionStatus::destination_too_small, 0};
    }

    if (!source.empty()) {
        std::memmove(destination.data(), source.data(), source.size());
    }

    const auto bytes_per_frame =
        static_cast<std::size_t>(core::kCdBytesPerSampleFrame);
    return {CddaPcmConversionStatus::ok, source.size() / bytes_per_frame};
}

CddaPcmConversionStatus convert_cdda_to_pcm16le_in_place(
    const std::span<std::byte> samples) noexcept
{
    if (!is_cdda_pcm_frame_aligned(samples.size())) {
        return CddaPcmConversionStatus::source_not_frame_aligned;
    }

    return CddaPcmConversionStatus::ok;
}

} // namespace cd404::audio
