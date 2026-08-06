#pragma once

#include <cstdint>
#include <optional>

namespace cd404::core {

inline constexpr std::int64_t kCdSampleFramesPerSecond = 44'100;
inline constexpr std::int64_t kCdSectorsPerSecond = 75;
inline constexpr std::int64_t kCdSampleFramesPerSector = 588;
inline constexpr std::int64_t kCdBytesPerSector = 2'352;
inline constexpr std::int64_t kCdBytesPerSampleFrame = 4;
inline constexpr std::int64_t kCdProgramAreaOffsetSectors = 150;

using Sector = std::int64_t;
using SampleFrame = std::int64_t;

struct Msf final {
    int minute{};
    int second{};
    int frame{};

    [[nodiscard]] bool operator==(const Msf&) const = default;
};

[[nodiscard]] constexpr bool is_valid_msf(const Msf value) noexcept
{
    return value.minute >= 0 && value.minute <= 99 && value.second >= 0 &&
           value.second < 60 && value.frame >= 0 && value.frame < 75;
}

[[nodiscard]] std::optional<Sector> msf_to_absolute_sector(Msf value) noexcept;
[[nodiscard]] std::optional<Msf> absolute_sector_to_msf(Sector sector) noexcept;
[[nodiscard]] std::optional<Sector> lba_to_absolute_sector(Sector lba) noexcept;
[[nodiscard]] std::optional<Sector> absolute_sector_to_lba(Sector sector) noexcept;
[[nodiscard]] std::optional<SampleFrame> sector_count_to_sample_frames(
    Sector sector_count) noexcept;
[[nodiscard]] std::optional<SampleFrame> lba_to_disc_frame(
    Sector lba,
    Sector origin_lba) noexcept;

} // namespace cd404::core
