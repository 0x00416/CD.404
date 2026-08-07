#pragma once

#include <cd404/audio/cdda_sector_source.hpp>
#include <cd404/platform/windows/optical_drive.hpp>

#include <atomic>
#include <memory>

namespace cd404::platform::windows {

class RawCddaSectorSource final : public audio::CddaSectorSource {
public:
    RawCddaSectorSource(
        void* native_handle,
        void* native_io_event,
        core::Sector first_lba,
        core::Sector end_lba) noexcept;
    ~RawCddaSectorSource() override;

    [[nodiscard]] core::Sector first_lba() const noexcept override;
    [[nodiscard]] core::Sector end_lba() const noexcept override;
    [[nodiscard]] audio::SectorReadResult read_sectors(
        core::Sector start_lba,
        std::span<std::byte> destination) override;

    // Cancels a pending overlapped device read. Safe to call from another
    // thread while the source remains alive.
    void request_cancel() noexcept;

private:
    void* native_handle_{};
    void* native_io_event_{};
    core::Sector first_lba_{};
    core::Sector end_lba_{};
    std::atomic_bool cancel_requested_{};
};

struct RawCddaOpenResult final {
    std::unique_ptr<RawCddaSectorSource> source;
    unsigned long system_error{};
};

[[nodiscard]] RawCddaOpenResult open_raw_cdda_source(
    const OpticalDrive& drive,
    core::Sector first_lba,
    core::Sector end_lba);

} // namespace cd404::platform::windows
