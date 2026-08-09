#pragma once

#include <cstdint>

namespace cd404::ui {

struct ThemePalette final {
    bool dark{};
    bool high_contrast{};
    std::uint32_t background{};
    std::uint32_t surface{};
    std::uint32_t elevated{};
    std::uint32_t border{};
    std::uint32_t text{};
    std::uint32_t secondary{};
    std::uint32_t muted{};
    std::uint32_t accent{};
    std::uint32_t accent_text{};
    std::uint32_t success{};
    std::uint32_t warning{};
    std::uint32_t error{};
    std::uint32_t disc{};
};

[[nodiscard]] ThemePalette make_theme_palette(
    bool system_dark,
    bool high_contrast) noexcept;
[[nodiscard]] ThemePalette query_system_theme() noexcept;

} // namespace cd404::ui
