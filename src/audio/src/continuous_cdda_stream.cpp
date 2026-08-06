#include <cd404/audio/continuous_cdda_stream.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace cd404::audio {

ContinuousCddaStream::ContinuousCddaStream(
    CddaSectorSource& source,
    const core::Sector start_lba,
    const core::Sector end_lba) noexcept
    : source_(source),
      start_lba_(start_lba),
      end_lba_(end_lba),
      next_lba_(start_lba)
{
    const bool range_is_valid = start_lba >= source.first_lba() &&
                                end_lba <= source.end_lba() && start_lba < end_lba;
    if (!range_is_valid) {
        return;
    }

    if (start_lba < 0 &&
        end_lba > std::numeric_limits<core::Sector>::max() + start_lba) {
        return;
    }

    const auto total_frames =
        core::sector_count_to_sample_frames(end_lba - start_lba);
    if (!total_frames) {
        return;
    }

    total_frames_ = *total_frames;
    valid_ = true;
}

bool ContinuousCddaStream::valid() const noexcept
{
    return valid_;
}

core::SampleFrame ContinuousCddaStream::position() const noexcept
{
    return position_;
}

core::SampleFrame ContinuousCddaStream::total_frames() const noexcept
{
    return total_frames_;
}

FrameReadResult ContinuousCddaStream::read_frames(
    const std::span<std::byte> destination)
{
    const auto bytes_per_frame =
        static_cast<std::size_t>(core::kCdBytesPerSampleFrame);
    if (!valid_ || destination.size() % bytes_per_frame != 0) {
        return FrameReadResult{ReadStatus::invalid_request, 0, 0};
    }

    std::size_t bytes_written{};
    while (bytes_written < destination.size()) {
        if (cache_offset_ == cache_size_) {
            const auto fill_result = fill_cache();
            if (fill_result.status != ReadStatus::ok) {
                return FrameReadResult{
                    fill_result.status,
                    bytes_written / bytes_per_frame,
                    fill_result.native_error,
                };
            }
        }

        const std::size_t available = cache_size_ - cache_offset_;
        const std::size_t requested = destination.size() - bytes_written;
        const std::size_t bytes_to_copy = std::min(available, requested);
        std::memcpy(
            destination.data() + bytes_written,
            cache_.data() + cache_offset_,
            bytes_to_copy);
        cache_offset_ += bytes_to_copy;
        bytes_written += bytes_to_copy;
        position_ += static_cast<core::SampleFrame>(bytes_to_copy / bytes_per_frame);
    }

    return FrameReadResult{
        ReadStatus::ok,
        bytes_written / bytes_per_frame,
        0,
    };
}

bool ContinuousCddaStream::seek(const core::SampleFrame frame) noexcept
{
    if (!valid_ || frame < 0 || frame > total_frames_) {
        return false;
    }

    const core::Sector sector_offset = frame / core::kCdSampleFramesPerSector;
    const core::SampleFrame frame_in_sector =
        frame % core::kCdSampleFramesPerSector;

    next_lba_ = start_lba_ + sector_offset;
    pending_skip_bytes_ = static_cast<std::size_t>(
        frame_in_sector * core::kCdBytesPerSampleFrame);
    cache_offset_ = 0;
    cache_size_ = 0;
    position_ = frame;
    return true;
}

SectorReadResult ContinuousCddaStream::fill_cache()
{
    cache_offset_ = 0;
    cache_size_ = 0;

    if (next_lba_ >= end_lba_) {
        return SectorReadResult{ReadStatus::end_of_stream, 0, 0};
    }

    const auto remaining_sectors = static_cast<std::size_t>(end_lba_ - next_lba_);
    const std::size_t requested_sectors =
        std::min(remaining_sectors, kCacheSectorCount);
    const std::size_t requested_bytes = requested_sectors *
                                        static_cast<std::size_t>(
                                            core::kCdBytesPerSector);
    auto result = source_.read_sectors(
        next_lba_,
        std::span<std::byte>(cache_.data(), requested_bytes));

    if (result.sectors_read > requested_sectors ||
        (result.status == ReadStatus::ok && result.sectors_read == 0)) {
        return SectorReadResult{ReadStatus::io_error, 0, result.native_error};
    }

    cache_size_ = result.sectors_read *
                  static_cast<std::size_t>(core::kCdBytesPerSector);
    next_lba_ += static_cast<core::Sector>(result.sectors_read);

    if (pending_skip_bytes_ > cache_size_) {
        pending_skip_bytes_ = 0;
        cache_size_ = 0;
        return SectorReadResult{ReadStatus::io_error, 0, result.native_error};
    }

    cache_offset_ = pending_skip_bytes_;
    pending_skip_bytes_ = 0;

    if (cache_offset_ == cache_size_) {
        return result.status == ReadStatus::ok
                   ? SectorReadResult{ReadStatus::io_error, 0, result.native_error}
                   : result;
    }

    return SectorReadResult{ReadStatus::ok, result.sectors_read, 0};
}

} // namespace cd404::audio
