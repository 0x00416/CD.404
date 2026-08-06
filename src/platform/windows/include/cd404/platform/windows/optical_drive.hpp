#pragma once

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

[[nodiscard]] std::vector<OpticalDrive> enumerate_optical_drives();
[[nodiscard]] TocReadResult read_toc(const OpticalDrive& drive);
[[nodiscard]] std::wstring format_system_error(unsigned long error_code);

} // namespace cd404::platform::windows
