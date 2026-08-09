#include "ui_drawing.hpp"

#include <d2d1helper.h>
#include <wrl/client.h>

#include <cmath>

namespace cd404::ui::detail {
namespace {

using Microsoft::WRL::ComPtr;

void fill_triangle(
    ID2D1Factory* factory,
    ID2D1RenderTarget* target,
    const D2D1_POINT_2F first,
    const D2D1_POINT_2F second,
    const D2D1_POINT_2F third,
    ID2D1Brush* brush)
{
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(factory->CreatePathGeometry(geometry.ReleaseAndGetAddressOf()))) {
        return;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.ReleaseAndGetAddressOf()))) {
        return;
    }
    sink->BeginFigure(first, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(second);
    sink->AddLine(third);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (SUCCEEDED(sink->Close())) {
        target->FillGeometry(geometry.Get(), brush);
    }
}

} // namespace

void draw_control_icon(
    ID2D1Factory* factory,
    ID2D1RenderTarget* target,
    const ControlIcon icon,
    const D2D1_POINT_2F center,
    ID2D1Brush* brush)
{
    constexpr float half = 7.0F;
    switch (icon) {
    case ControlIcon::play:
        fill_triangle(factory, target,
            D2D1::Point2F(center.x - 4.5F, center.y - half),
            D2D1::Point2F(center.x + 7.0F, center.y),
            D2D1::Point2F(center.x - 4.5F, center.y + half), brush);
        break;
    case ControlIcon::stop:
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(
            center.x - 6.0F, center.y - 6.0F,
            center.x + 6.0F, center.y + 6.0F), 1.5F, 1.5F), brush);
        break;
    case ControlIcon::pause:
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(
            center.x - 6.5F, center.y - 7.0F,
            center.x - 2.0F, center.y + 7.0F), 1.0F, 1.0F), brush);
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(
            center.x + 2.0F, center.y - 7.0F,
            center.x + 6.5F, center.y + 7.0F), 1.0F, 1.0F), brush);
        break;
    case ControlIcon::previous:
        target->DrawLine(D2D1::Point2F(center.x - 6.5F, center.y - half),
            D2D1::Point2F(center.x - 6.5F, center.y + half), brush, 2.0F);
        fill_triangle(factory, target,
            D2D1::Point2F(center.x + 6.0F, center.y - half),
            D2D1::Point2F(center.x - 4.0F, center.y),
            D2D1::Point2F(center.x + 6.0F, center.y + half), brush);
        break;
    case ControlIcon::next:
        target->DrawLine(D2D1::Point2F(center.x + 6.5F, center.y - half),
            D2D1::Point2F(center.x + 6.5F, center.y + half), brush, 2.0F);
        fill_triangle(factory, target,
            D2D1::Point2F(center.x - 6.0F, center.y - half),
            D2D1::Point2F(center.x + 4.0F, center.y),
            D2D1::Point2F(center.x - 6.0F, center.y + half), brush);
        break;
    case ControlIcon::eject:
        fill_triangle(factory, target,
            D2D1::Point2F(center.x, center.y - 7.0F),
            D2D1::Point2F(center.x + 8.0F, center.y + 4.0F),
            D2D1::Point2F(center.x - 8.0F, center.y + 4.0F), brush);
        target->DrawLine(D2D1::Point2F(center.x - 8.0F, center.y + 8.0F),
            D2D1::Point2F(center.x + 8.0F, center.y + 8.0F), brush, 2.0F);
        break;
    case ControlIcon::refresh: {
        ComPtr<ID2D1PathGeometry> geometry;
        if (FAILED(factory->CreatePathGeometry(geometry.ReleaseAndGetAddressOf()))) {
            break;
        }
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geometry->Open(sink.ReleaseAndGetAddressOf()))) {
            break;
        }
        sink->BeginFigure(D2D1::Point2F(center.x + 6.5F, center.y - 3.5F),
            D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(center.x + 3.5F, center.y - 8.5F),
            D2D1::Point2F(center.x - 3.5F, center.y - 8.5F),
            D2D1::Point2F(center.x - 7.0F, center.y - 3.0F)));
        sink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(center.x - 10.0F, center.y + 2.0F),
            D2D1::Point2F(center.x - 6.0F, center.y + 8.0F),
            D2D1::Point2F(center.x, center.y + 8.0F)));
        sink->AddBezier(D2D1::BezierSegment(
            D2D1::Point2F(center.x + 3.5F, center.y + 8.0F),
            D2D1::Point2F(center.x + 6.5F, center.y + 6.0F),
            D2D1::Point2F(center.x + 7.5F, center.y + 3.0F)));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (SUCCEEDED(sink->Close())) {
            target->DrawGeometry(geometry.Get(), brush, 2.0F);
        }
        fill_triangle(factory, target,
            D2D1::Point2F(center.x + 7.0F, center.y - 7.5F),
            D2D1::Point2F(center.x + 9.0F, center.y - 1.0F),
            D2D1::Point2F(center.x + 2.5F, center.y - 2.5F), brush);
        break;
    }
    case ControlIcon::settings:
        target->DrawEllipse(D2D1::Ellipse(center, 6.0F, 6.0F), brush, 2.0F);
        target->DrawEllipse(D2D1::Ellipse(center, 2.0F, 2.0F), brush, 1.5F);
        for (int tooth = 0; tooth < 8; ++tooth) {
            const float angle = static_cast<float>(tooth) * 3.14159265F / 4.0F;
            const float cosine = std::cos(angle);
            const float sine = std::sin(angle);
            target->DrawLine(
                D2D1::Point2F(center.x + cosine * 7.5F, center.y + sine * 7.5F),
                D2D1::Point2F(center.x + cosine * 10.0F, center.y + sine * 10.0F),
                brush, 2.0F);
        }
        break;
    }
}

void draw_text(
    ID2D1RenderTarget* target,
    const std::wstring_view text,
    IDWriteTextFormat* format,
    const D2D1_RECT_F rectangle,
    ID2D1Brush* brush,
    const DWRITE_TEXT_ALIGNMENT alignment)
{
    const DWRITE_TEXT_ALIGNMENT previous = format->GetTextAlignment();
    format->SetTextAlignment(alignment);
    target->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format,
        rectangle, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    format->SetTextAlignment(previous);
}

void draw_toggle(
    ID2D1RenderTarget* target,
    const D2D1_RECT_F rectangle,
    const bool enabled,
    ID2D1Brush* accent,
    ID2D1Brush* elevated,
    ID2D1Brush* border,
    ID2D1Brush* accent_text,
    ID2D1Brush* secondary)
{
    const auto pill = D2D1::RoundedRect(rectangle, 14.0F, 14.0F);
    target->FillRoundedRectangle(pill, enabled ? accent : elevated);
    target->DrawRoundedRectangle(pill, border, 1.0F);
    constexpr float radius = 10.0F;
    const float center_x = enabled
        ? rectangle.right - radius - 4.0F
        : rectangle.left + radius + 4.0F;
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(
        center_x, (rectangle.top + rectangle.bottom) * 0.5F), radius, radius),
        enabled ? accent_text : secondary);
}

} // namespace cd404::ui::detail
