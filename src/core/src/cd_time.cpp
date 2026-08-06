#include <cd404/core/cd_time.hpp>

#include <limits>

namespace cd404::core {
namespace {

[[nodiscard]] constexpr bool multiplication_would_overflow(
    const std::int64_t lhs,
    const std::int64_t rhs) noexcept
{
    if (lhs == 0 || rhs == 0) {
        return false;
    }

    return lhs > std::numeric_limits<std::int64_t>::max() / rhs;
}

} // namespace

std::optional<Sector> msf_to_absolute_sector(const Msf value) noexcept
{
    if (!is_valid_msf(value)) {
        return std::nullopt;
    }

    return (static_cast<Sector>(value.minute) * 60 + value.second) *
               kCdSectorsPerSecond +
           value.frame;
}

std::optional<Msf> absolute_sector_to_msf(const Sector sector) noexcept
{
    constexpr Sector kMaximumAddressableSector =
        (99 * 60 * kCdSectorsPerSecond) + (59 * kCdSectorsPerSecond) + 74;

    if (sector < 0 || sector > kMaximumAddressableSector) {
        return std::nullopt;
    }

    const auto minute = static_cast<int>(sector / (60 * kCdSectorsPerSecond));
    const auto remainder = sector % (60 * kCdSectorsPerSecond);
    const auto second = static_cast<int>(remainder / kCdSectorsPerSecond);
    const auto frame = static_cast<int>(remainder % kCdSectorsPerSecond);
    return Msf{minute, second, frame};
}

std::optional<Sector> lba_to_absolute_sector(const Sector lba) noexcept
{
    if (lba < -kCdProgramAreaOffsetSectors ||
        lba > std::numeric_limits<Sector>::max() - kCdProgramAreaOffsetSectors) {
        return std::nullopt;
    }

    return lba + kCdProgramAreaOffsetSectors;
}

std::optional<Sector> absolute_sector_to_lba(const Sector sector) noexcept
{
    if (sector < 0) {
        return std::nullopt;
    }

    return sector - kCdProgramAreaOffsetSectors;
}

std::optional<SampleFrame> sector_count_to_sample_frames(
    const Sector sector_count) noexcept
{
    if (sector_count < 0 ||
        multiplication_would_overflow(sector_count, kCdSampleFramesPerSector)) {
        return std::nullopt;
    }

    return sector_count * kCdSampleFramesPerSector;
}

std::optional<SampleFrame> lba_to_disc_frame(
    const Sector lba,
    const Sector origin_lba) noexcept
{
    if (lba < origin_lba) {
        return std::nullopt;
    }

    if (origin_lba < 0 &&
        lba > std::numeric_limits<Sector>::max() + origin_lba) {
        return std::nullopt;
    }

    return sector_count_to_sample_frames(lba - origin_lba);
}

} // namespace cd404::core
