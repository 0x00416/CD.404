#include <cd404/audio/reliable_cdda_sector_source.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace cd404::audio {

ReliableCddaSectorSource::ReliableCddaSectorSource(
    CddaSectorSource& source,
    ReliableReadPolicy policy) noexcept
    : source_(source), policy_(policy)
{
    policy_.maximum_attempts = std::max<std::size_t>(policy_.maximum_attempts, 1);
}

core::Sector ReliableCddaSectorSource::first_lba() const noexcept
{
    return source_.first_lba();
}

core::Sector ReliableCddaSectorSource::end_lba() const noexcept
{
    return source_.end_lba();
}

SectorReadResult ReliableCddaSectorSource::read_sectors(
    const core::Sector start_lba,
    const std::span<std::byte> destination)
{
    ++statistics_.logical_reads;
    if (destination.empty() || destination.size() % kSectorBytes != 0 ||
        start_lba < first_lba() || start_lba >= end_lba()) {
        return {ReadStatus::invalid_request, 0, 0};
    }

    const std::size_t requested_sectors = destination.size() / kSectorBytes;
    if (requested_sectors > static_cast<std::size_t>(end_lba() - start_lba)) {
        return {ReadStatus::invalid_request, 0, 0};
    }

    const bool verify_overlap = policy_.verify_sequential_overlap &&
                                previous_sector_valid_ &&
                                start_lba == expected_next_lba_ &&
                                start_lba > first_lba();
    const std::size_t device_sectors = requested_sectors +
                                       static_cast<std::size_t>(verify_overlap);
    if (device_sectors > std::numeric_limits<std::size_t>::max() / kSectorBytes) {
        return {ReadStatus::invalid_request, 0, 0};
    }

    std::span<std::byte> device_destination = destination;
    core::Sector device_start_lba = start_lba;
    if (verify_overlap) {
        overlap_buffer_.resize(device_sectors * kSectorBytes);
        device_destination = overlap_buffer_;
        device_start_lba = start_lba - 1;
    }

    SectorReadResult last_result{ReadStatus::io_error, 0, 0};
    for (std::size_t attempt = 0; attempt < policy_.maximum_attempts; ++attempt) {
        if (attempt != 0) {
            ++statistics_.retries;
        }
        ++statistics_.device_reads;
        last_result = source_.read_sectors(device_start_lba, device_destination);

        if (last_result.status == ReadStatus::ok &&
            last_result.sectors_read == device_sectors) {
            if (verify_overlap) {
                ++statistics_.overlap_checks;
                if (!std::equal(
                        previous_sector_.begin(),
                        previous_sector_.end(),
                        overlap_buffer_.begin())) {
                    ++statistics_.overlap_mismatches;
                    last_result = {
                        ReadStatus::verification_error,
                        0,
                        last_result.native_error,
                    };
                    continue;
                }
                std::memcpy(
                    destination.data(),
                    overlap_buffer_.data() + kSectorBytes,
                    destination.size());
            }

            remember_last_sector(start_lba, destination);
            statistics_.sectors_delivered += requested_sectors;
            return {ReadStatus::ok, requested_sectors, 0};
        }

        if (last_result.status == ReadStatus::ok) {
            last_result = {
                ReadStatus::io_error,
                0,
                last_result.native_error,
            };
        }

        if (last_result.status != ReadStatus::io_error) {
            break;
        }
        last_result.sectors_read = 0;
    }

    return last_result;
}

const ReliableReadStatistics& ReliableCddaSectorSource::statistics() const noexcept
{
    return statistics_;
}

void ReliableCddaSectorSource::remember_last_sector(
    const core::Sector start_lba,
    const std::span<const std::byte> bytes) noexcept
{
    std::memcpy(
        previous_sector_.data(),
        bytes.data() + bytes.size() - kSectorBytes,
        kSectorBytes);
    const auto delivered_sectors = static_cast<core::Sector>(bytes.size() / kSectorBytes);
    expected_next_lba_ = start_lba + delivered_sectors;
    previous_sector_valid_ = true;
}

} // namespace cd404::audio
