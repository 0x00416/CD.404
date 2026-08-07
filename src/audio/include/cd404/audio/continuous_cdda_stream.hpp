#pragma once

#include <cd404/audio/cdda_sector_source.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace cd404::audio {

struct FrameReadResult final {
    ReadStatus status{ReadStatus::ok};
    std::size_t frames_read{};
    unsigned long native_error{};
};

class ContinuousCddaStream final {
public:
    ContinuousCddaStream(
        CddaSectorSource& source,
        core::Sector start_lba,
        core::Sector end_lba) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] core::SampleFrame position() const noexcept;
    [[nodiscard]] core::SampleFrame total_frames() const noexcept;

    // Output remains in raw CDDA byte order. The destination size must be a
    // multiple of kCdBytesPerSampleFrame.
    [[nodiscard]] FrameReadResult read_frames(std::span<std::byte> destination);
    [[nodiscard]] bool seek(core::SampleFrame frame) noexcept;

private:
    [[nodiscard]] SectorReadResult fill_cache();

    static constexpr std::size_t kCacheSectorCount = 32;
    static constexpr std::size_t kCacheByteCount =
        kCacheSectorCount * static_cast<std::size_t>(core::kCdBytesPerSector);

    CddaSectorSource& source_;
    core::Sector start_lba_{};
    core::Sector end_lba_{};
    core::Sector next_lba_{};
    core::SampleFrame position_{};
    core::SampleFrame total_frames_{};
    std::size_t pending_skip_bytes_{};
    std::array<std::byte, kCacheByteCount> cache_{};
    std::size_t cache_offset_{};
    std::size_t cache_size_{};
    bool valid_{};
};

} // namespace cd404::audio
