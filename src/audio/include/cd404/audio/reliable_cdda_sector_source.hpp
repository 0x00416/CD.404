#pragma once

#include <cd404/audio/cdda_sector_source.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cd404::audio {

struct ReliableReadPolicy final {
    std::size_t maximum_attempts{3};
    bool verify_sequential_overlap{true};
};

struct ReliableReadStatistics final {
    std::uint64_t logical_reads{};
    std::uint64_t device_reads{};
    std::uint64_t retries{};
    std::uint64_t overlap_checks{};
    std::uint64_t overlap_mismatches{};
    std::uint64_t sectors_delivered{};
};

// Adds bounded retry and sequential-block overlap verification to an existing
// sector source. It is intended for the non-real-time CDDA producer thread.
class ReliableCddaSectorSource final : public CddaSectorSource {
public:
    explicit ReliableCddaSectorSource(
        CddaSectorSource& source,
        ReliableReadPolicy policy = {}) noexcept;

    [[nodiscard]] core::Sector first_lba() const noexcept override;
    [[nodiscard]] core::Sector end_lba() const noexcept override;
    [[nodiscard]] SectorReadResult read_sectors(
        core::Sector start_lba,
        std::span<std::byte> destination) override;

    [[nodiscard]] const ReliableReadStatistics& statistics() const noexcept;

private:
    void remember_last_sector(
        core::Sector start_lba,
        std::span<const std::byte> bytes) noexcept;

    static constexpr std::size_t kSectorBytes =
        static_cast<std::size_t>(core::kCdBytesPerSector);

    CddaSectorSource& source_;
    ReliableReadPolicy policy_;
    ReliableReadStatistics statistics_;
    std::vector<std::byte> overlap_buffer_;
    std::array<std::byte, kSectorBytes> previous_sector_{};
    core::Sector expected_next_lba_{};
    bool previous_sector_valid_{};
};

} // namespace cd404::audio
