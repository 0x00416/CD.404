#pragma once

#include <d2d1.h>
#include <dwrite.h>

#include <string_view>

namespace cd404::ui::detail {

enum class ControlIcon {
    refresh,
    eject,
    previous,
    play,
    pause,
    stop,
    next,
    settings,
};

void draw_control_icon(
    ID2D1Factory* factory,
    ID2D1RenderTarget* target,
    ControlIcon icon,
    D2D1_POINT_2F center,
    ID2D1Brush* brush);

void draw_text(
    ID2D1RenderTarget* target,
    std::wstring_view text,
    IDWriteTextFormat* format,
    D2D1_RECT_F rectangle,
    ID2D1Brush* brush,
    DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);

void draw_toggle(
    ID2D1RenderTarget* target,
    D2D1_RECT_F rectangle,
    bool enabled,
    ID2D1Brush* accent,
    ID2D1Brush* elevated,
    ID2D1Brush* border,
    ID2D1Brush* accent_text,
    ID2D1Brush* secondary);

} // namespace cd404::ui::detail
