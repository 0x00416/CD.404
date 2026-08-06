#pragma once

#include <cd404/disc/cd_text.hpp>
#include <cd404/disc/toc.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cd404::platform::windows {

struct OpticalDrive final {
    std::wstring root_path;
    std::wstring device_path;
};

struct TocReadResult final {
    std::optional<disc::Toc> toc;
    unsigned long system_error{};
    disc::TocError validation_error{disc::TocError::none};
};

struct CdTextReadResult final {
    std::optional<disc::CdTextMetadata> metadata;
    unsigned long system_error{};
};

[[nodiscard]] std::vector<OpticalDrive> enumerate_optical_drives();
[[nodiscard]] TocReadResult read_toc(const OpticalDrive& drive);
[[nodiscard]] CdTextReadResult read_cd_text(const OpticalDrive& drive);
// Requests a hardware eject for the selected drive. Returns ERROR_SUCCESS on
// success or the Win32 error reported while opening/controlling the device.
[[nodiscard]] unsigned long eject_media(const OpticalDrive& drive) noexcept;
[[nodiscard]] std::wstring format_system_error(unsigned long error_code);

} // namespace cd404::platform::windows
