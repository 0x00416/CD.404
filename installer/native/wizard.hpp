#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace cd404::installer {

using ProgressCallback = std::function<void(int, std::wstring_view)>;

struct InstallWizardConfig {
    std::wstring version;
    std::wstring destination;
    std::uint64_t required_bytes{};
    bool updating{};
    std::function<bool(
        const std::wstring&,
        bool,
        const ProgressCallback&,
        DWORD&)> install;
    std::function<void(const std::wstring&)> launch;
};

struct UninstallWizardConfig {
    std::function<bool(bool, const ProgressCallback&, DWORD&)> uninstall;
};

[[nodiscard]] int run_install_wizard(
    HINSTANCE instance,
    InstallWizardConfig config);

[[nodiscard]] int run_uninstall_wizard(
    HINSTANCE instance,
    UninstallWizardConfig config);

} // namespace cd404::installer
