#pragma once

#include <cd404/core/cd_time.hpp>

#include <cstddef>
#include <span>

namespace cd404::audio {

enum class ReadStatus {
    ok,
    end_of_stream,
    invalid_request,
    io_error,
};

struct SectorReadResult final {
    ReadStatus status{ReadStatus::ok};
    std::size_t sectors_read{};
    unsigned long native_error{};
};

class CddaSectorSource {
public:
    virtual ~CddaSectorSource() = default;

    CddaSectorSource(const CddaSectorSource&) = delete;
    CddaSectorSource& operator=(const CddaSectorSource&) = delete;
    CddaSectorSource(CddaSectorSource&&) = delete;
    CddaSectorSource& operator=(CddaSectorSource&&) = delete;

    [[nodiscard]] virtual core::Sector first_lba() const noexcept = 0;
    [[nodiscard]] virtual core::Sector end_lba() const noexcept = 0;

    // The destination size must be a multiple of kCdBytesPerSector.
    [[nodiscard]] virtual SectorReadResult read_sectors(
        core::Sector start_lba,
        std::span<std::byte> destination) = 0;

protected:
    CddaSectorSource() = default;
};

} // namespace cd404::audio
