#pragma once

#include <d2d1.h>
#include <dwrite.h>

namespace cd404::ui {

inline constexpr D2D1_TEXT_ANTIALIAS_MODE kInterfaceTextAntialiasMode =
    D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
inline constexpr D2D1_TEXT_ANTIALIAS_MODE kAnimatedTextAntialiasMode =
    D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
inline constexpr DWRITE_RENDERING_MODE kTextRenderingMode =
    DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;

[[nodiscard]] HRESULT configure_text_rendering(
    ID2D1RenderTarget& render_target,
    IDWriteFactory& write_factory);

} // namespace cd404::ui
