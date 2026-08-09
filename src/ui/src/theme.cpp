#include <windows.h>

#include <cd404/ui/theme.hpp>

namespace cd404::ui {
namespace {

[[nodiscard]] std::uint32_t rgb_from_colorref(const COLORREF value) noexcept
{
    return (static_cast<std::uint32_t>(GetRValue(value)) << 16U) |
        (static_cast<std::uint32_t>(GetGValue(value)) << 8U) |
        static_cast<std::uint32_t>(GetBValue(value));
}

} // namespace

ThemePalette make_theme_palette(
    const bool system_dark,
    const bool high_contrast) noexcept
{
    if (high_contrast) {
        return {
            true, true,
            0x000000, 0x000000, 0x000000, 0xFFFFFF,
            0xFFFFFF, 0xFFFFFF, 0xC0C0C0, 0xFFFF00,
            0x000000, 0x00FF00, 0xFFFF00, 0xFF8080, 0x000000,
        };
    }
    if (system_dark) {
        return {
            true, false,
            0x0C0E12, 0x14171C, 0x1B2027, 0x2A3038,
            0xF2F4F7, 0xA8B0BB, 0x747D89, 0x91A0FF,
            0x101218, 0x62D3A4, 0xF0BD6A, 0xEF737A, 0x050608,
        };
    }
    return {
        false, false,
        0xF5F7FA, 0xFFFFFF, 0xE9EDF3, 0xC8CFDA,
        0x171A21, 0x46505F, 0x687486, 0x4056D8,
        0xFFFFFF, 0x147A59, 0x8A5A00, 0xB4232B, 0xD8DCE4,
    };
}

ThemePalette query_system_theme() noexcept
{
    HIGHCONTRASTW high_contrast{};
    high_contrast.cbSize = sizeof(high_contrast);
    const bool high = SystemParametersInfoW(
        SPI_GETHIGHCONTRAST,
        sizeof(high_contrast),
        &high_contrast,
        0) != FALSE &&
        (high_contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;

    DWORD light_apps{1};
    DWORD size = sizeof(light_apps);
    static_cast<void>(RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &light_apps,
        &size));
    ThemePalette palette = make_theme_palette(light_apps == 0, high);
    if (high) {
        palette.background = rgb_from_colorref(GetSysColor(COLOR_WINDOW));
        palette.surface = rgb_from_colorref(GetSysColor(COLOR_WINDOW));
        palette.elevated = rgb_from_colorref(GetSysColor(COLOR_BTNFACE));
        palette.border = rgb_from_colorref(GetSysColor(COLOR_WINDOWTEXT));
        palette.text = rgb_from_colorref(GetSysColor(COLOR_WINDOWTEXT));
        palette.secondary = palette.text;
        palette.muted = rgb_from_colorref(GetSysColor(COLOR_GRAYTEXT));
        palette.accent = rgb_from_colorref(GetSysColor(COLOR_HIGHLIGHT));
        palette.accent_text = rgb_from_colorref(GetSysColor(COLOR_HIGHLIGHTTEXT));
        palette.disc = palette.background;
    }
    return palette;
}

} // namespace cd404::ui
