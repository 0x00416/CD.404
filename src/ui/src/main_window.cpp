#include <windows.h>
#include <windowsx.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <cd404/core/cd_time.hpp>
#include <cd404/platform/windows/optical_drive.hpp>
#include <cd404/ui/main_window.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cd404::ui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClassName[] = L"CD404.MainWindow";
constexpr wchar_t kWindowTitle[] = L"CD.404";
constexpr UINT kDiscReadyMessage = WM_APP + 1;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kAnimationIntervalMs = 50;
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmSystemBackdropType = 38;

constexpr float kDesignWidth = 1'100.0F;
constexpr float kDesignHeight = 760.0F;
constexpr float kMinimumWidth = 840.0F;
constexpr float kMinimumHeight = 600.0F;

[[nodiscard]] D2D1_COLOR_F color(
    const std::uint32_t rgb,
    const float alpha = 1.0F) noexcept
{
    return D2D1::ColorF(
        static_cast<float>((rgb >> 16U) & 0xffU) / 255.0F,
        static_cast<float>((rgb >> 8U) & 0xffU) / 255.0F,
        static_cast<float>(rgb & 0xffU) / 255.0F,
        alpha);
}

[[nodiscard]] bool contains(const D2D1_RECT_F& rectangle, const D2D1_POINT_2F point)
{
    return point.x >= rectangle.left && point.x <= rectangle.right &&
           point.y >= rectangle.top && point.y <= rectangle.bottom;
}

[[nodiscard]] float clamp01(const float value) noexcept
{
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] std::wstring format_duration(const core::SampleFrame frame_count)
{
    const auto seconds = frame_count / core::kCdSampleFramesPerSecond;
    return std::format(L"{}:{:02}", seconds / 60, seconds % 60);
}

struct UiTrack final {
    std::uint8_t number{};
    core::SampleFrame frame_count{};
    bool is_audio{};
    std::wstring title;
};

struct DiscSnapshot final {
    std::optional<platform::windows::OpticalDrive> drive;
    std::vector<UiTrack> tracks;
    core::SampleFrame total_audio_frames{};
    std::wstring status;
    bool has_optical_drive{};
};

[[nodiscard]] DiscSnapshot load_disc_snapshot()
{
    DiscSnapshot snapshot;
    const auto drives = platform::windows::enumerate_optical_drives();
    snapshot.has_optical_drive = !drives.empty();
    if (drives.empty()) {
        snapshot.status = L"未检测到光驱";
        return snapshot;
    }

    unsigned long last_error{};
    for (const auto& drive : drives) {
        auto toc_result = platform::windows::read_toc(drive);
        if (!toc_result.toc) {
            last_error = toc_result.system_error;
            continue;
        }

        snapshot.drive = drive;
        for (const auto& track : toc_result.toc->tracks()) {
            UiTrack view;
            view.number = track.number;
            view.frame_count = track.frame_count;
            view.is_audio = track.is_audio;
            view.title = track.is_audio
                ? std::format(L"音轨 {:02}", track.number)
                : std::format(L"数据轨 {:02}", track.number);
            if (track.is_audio) {
                snapshot.total_audio_frames += track.frame_count;
            }
            snapshot.tracks.push_back(std::move(view));
        }
        snapshot.status = std::format(
            L"已就绪 · {} 首音频轨",
            std::count_if(
                snapshot.tracks.begin(),
                snapshot.tracks.end(),
                [](const UiTrack& track) { return track.is_audio; }));
        return snapshot;
    }

    snapshot.drive = drives.front();
    snapshot.status = last_error == 0
        ? L"请插入一张音频 CD"
        : L"光驱已连接 · 等待音频 CD";
    return snapshot;
}

struct TrackHit final {
    D2D1_RECT_F rectangle{};
    std::size_t track_index{};
};

struct Layout final {
    float width{};
    float height{};
    D2D1_RECT_F refresh_button{};
    D2D1_RECT_F eject_button{};
    D2D1_RECT_F cover{};
    D2D1_RECT_F track_list{};
    D2D1_RECT_F progress_hit{};
    D2D1_RECT_F previous_button{};
    D2D1_RECT_F play_button{};
    D2D1_RECT_F next_button{};
};

class MainWindow final {
public:
    explicit MainWindow(HINSTANCE instance) noexcept : instance_(instance) {}

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    ~MainWindow()
    {
        stop_playback();
    }

    [[nodiscard]] bool create(const int show_command)
    {
        if (FAILED(create_independent_resources())) {
            return false;
        }

        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        window_class.hIconSm = window_class.hIcon;
        window_class.lpszClassName = kWindowClassName;
        if (RegisterClassExW(&window_class) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        const UINT dpi = 96;
        RECT window_rectangle{
            0,
            0,
            static_cast<LONG>(kDesignWidth),
            static_cast<LONG>(kDesignHeight),
        };
        static_cast<void>(AdjustWindowRectExForDpi(
            &window_rectangle,
            WS_OVERLAPPEDWINDOW,
            FALSE,
            0,
            dpi));

        window_ = CreateWindowExW(
            0,
            kWindowClassName,
            kWindowTitle,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            window_rectangle.right - window_rectangle.left,
            window_rectangle.bottom - window_rectangle.top,
            nullptr,
            nullptr,
            instance_,
            this);
        if (window_ == nullptr) {
            return false;
        }

        apply_window_materials();
        ShowWindow(window_, show_command);
        UpdateWindow(window_);
        return true;
    }

    [[nodiscard]] int run()
    {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK window_proc(
        const HWND window,
        const UINT message,
        const WPARAM wparam,
        const LPARAM lparam)
    {
        MainWindow* self{};
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<MainWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<MainWindow*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return self != nullptr
            ? self->handle_message(message, wparam, lparam)
            : DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT handle_message(
        const UINT message,
        const WPARAM wparam,
        const LPARAM lparam)
    {
        switch (message) {
        case WM_CREATE:
            static_cast<void>(SetTimer(
                window_,
                kAnimationTimer,
                kAnimationIntervalMs,
                nullptr));
            refresh_disc();
            return 0;
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (render_target_) {
                const auto width = static_cast<UINT32>(LOWORD(lparam));
                const auto height = static_cast<UINT32>(HIWORD(lparam));
                static_cast<void>(render_target_->Resize(D2D1::SizeU(width, height)));
            }
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(
                window_,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            if (render_target_) {
                const float dpi = static_cast<float>(HIWORD(wparam));
                render_target_->SetDpi(dpi, dpi);
            }
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* const info = reinterpret_cast<MINMAXINFO*>(lparam);
            const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0F;
            info->ptMinTrackSize.x = static_cast<LONG>(std::lround(kMinimumWidth * scale));
            info->ptMinTrackSize.y = static_cast<LONG>(std::lround(kMinimumHeight * scale));
            return 0;
        }
        case WM_MOUSEMOVE:
            update_hover(D2D1::Point2F(
                pixel_to_dip(GET_X_LPARAM(lparam)),
                pixel_to_dip(GET_Y_LPARAM(lparam))));
            return 0;
        case WM_MOUSELEAVE:
            hovered_track_.reset();
            hovered_control_ = HoveredControl::none;
            tracking_mouse_ = false;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            scroll_tracks(GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(window_);
            handle_click(D2D1::Point2F(
                pixel_to_dip(GET_X_LPARAM(lparam)),
                pixel_to_dip(GET_Y_LPARAM(lparam))));
            return 0;
        case WM_LBUTTONDBLCLK:
            handle_double_click(D2D1::Point2F(
                pixel_to_dip(GET_X_LPARAM(lparam)),
                pixel_to_dip(GET_Y_LPARAM(lparam))));
            return 0;
        case WM_KEYDOWN:
            return handle_key_down(wparam);
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT &&
                (hovered_track_ || hovered_control_ != HoveredControl::none)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_DEVICECHANGE:
            refresh_disc();
            return 0;
        case WM_TIMER:
            if (wparam == kAnimationTimer) {
                update_playback_clock();
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case kDiscReadyMessage:
            receive_disc_snapshot(
                std::unique_ptr<DiscSnapshot>(
                    reinterpret_cast<DiscSnapshot*>(lparam)));
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kAnimationTimer);
            stop_playback();
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    enum class HoveredControl {
        none,
        refresh,
        eject,
        previous,
        play,
        next,
        progress,
    };

    enum class ControlIcon {
        refresh,
        eject,
        previous,
        play,
        stop,
        next,
    };

    [[nodiscard]] HRESULT create_independent_resources()
    {
        HRESULT result = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            factory_.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            return result;
        }

        result = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write_factory_.ReleaseAndGetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        result = create_text_format(
            18.0F,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            logo_format_);
        if (SUCCEEDED(result)) {
            result = create_text_format(
                29.0F,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                album_format_);
        }
        if (SUCCEEDED(result)) {
            result = create_text_format(
                18.0F,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                heading_format_);
        }
        if (SUCCEEDED(result)) {
            result = create_text_format(
                15.0F,
                DWRITE_FONT_WEIGHT_NORMAL,
                body_format_);
        }
        if (SUCCEEDED(result)) {
            result = create_text_format(
                13.0F,
                DWRITE_FONT_WEIGHT_NORMAL,
                small_format_);
        }
        if (SUCCEEDED(result)) {
            result = create_text_format(
                12.0F,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                caption_format_);
        }
        if (SUCCEEDED(result)) {
            result = create_text_format(
                20.0F,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                icon_format_);
        }
        if (FAILED(result)) {
            return result;
        }

        icon_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        icon_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        caption_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        caption_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        small_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return S_OK;
    }

    HRESULT create_text_format(
        const float size,
        const DWRITE_FONT_WEIGHT weight,
        ComPtr<IDWriteTextFormat>& target)
    {
        const HRESULT result = write_factory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size,
            L"zh-CN",
            target.ReleaseAndGetAddressOf());
        if (SUCCEEDED(result)) {
            target->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        return result;
    }

    [[nodiscard]] HRESULT create_device_resources()
    {
        if (render_target_) {
            return S_OK;
        }

        RECT client{};
        GetClientRect(window_, &client);
        const D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(client.right, 1)),
            static_cast<UINT32>(std::max<LONG>(client.bottom, 1)));
        HRESULT result = factory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(window_, size),
            render_target_.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            return result;
        }

        const float dpi = static_cast<float>(GetDpiForWindow(window_));
        render_target_->SetDpi(dpi, dpi);
        result = create_brush(color(0x0C0E12), background_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0x14171C), surface_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0x1B2027), elevated_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0x2A3038), border_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0xF2F4F7), text_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0xA8B0BB), secondary_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0x747D89), muted_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0x91A0FF), accent_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0x101218), accent_text_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0x62D3A4), success_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0xF0BD6A), warning_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0xEF737A), error_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0x91A0FF, 0.11F), selection_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0xFFFFFF, 0.055F), hover_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(0x050608, 0.72F), disc_brush_);
        if (FAILED(result)) {
            discard_device_resources();
            return result;
        }

        const D2D1_GRADIENT_STOP stops[]{
            {0.0F, color(0x5666D8)},
            {0.48F, color(0xA95A88)},
            {1.0F, color(0xF08A73)},
        };
        ComPtr<ID2D1GradientStopCollection> stop_collection;
        result = render_target_->CreateGradientStopCollection(
            stops,
            static_cast<UINT32>(std::size(stops)),
            stop_collection.ReleaseAndGetAddressOf());
        if (SUCCEEDED(result)) {
            result = render_target_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(0.0F, 0.0F),
                    D2D1::Point2F(340.0F, 340.0F)),
                stop_collection.Get(),
                cover_brush_.ReleaseAndGetAddressOf());
        }
        return result;
    }

    HRESULT create_brush(
        const D2D1_COLOR_F brush_color,
        ComPtr<ID2D1SolidColorBrush>& brush)
    {
        return render_target_->CreateSolidColorBrush(
            brush_color,
            brush.ReleaseAndGetAddressOf());
    }

    void discard_device_resources()
    {
        cover_brush_.Reset();
        disc_brush_.Reset();
        hover_brush_.Reset();
        selection_brush_.Reset();
        error_brush_.Reset();
        warning_brush_.Reset();
        success_brush_.Reset();
        accent_text_brush_.Reset();
        accent_brush_.Reset();
        muted_brush_.Reset();
        secondary_brush_.Reset();
        text_brush_.Reset();
        border_brush_.Reset();
        elevated_brush_.Reset();
        surface_brush_.Reset();
        background_brush_.Reset();
        render_target_.Reset();
    }

    void apply_window_materials() const
    {
        const BOOL dark = TRUE;
        static_cast<void>(DwmSetWindowAttribute(
            window_,
            kDwmUseImmersiveDarkMode,
            &dark,
            sizeof(dark)));
        const int rounded = 2;
        static_cast<void>(DwmSetWindowAttribute(
            window_,
            kDwmWindowCornerPreference,
            &rounded,
            sizeof(rounded)));
        const int backdrop = 2;
        static_cast<void>(DwmSetWindowAttribute(
            window_,
            kDwmSystemBackdropType,
            &backdrop,
            sizeof(backdrop)));
    }

    [[nodiscard]] Layout calculate_layout() const
    {
        const auto size = render_target_->GetSize();
        Layout result;
        result.width = size.width;
        result.height = size.height;

        const float margin = size.width < 960.0F ? 20.0F : 24.0F;
        const float top = 84.0F;
        const float transport_height = size.height < 680.0F ? 104.0F : 124.0F;
        const float transport_top = size.height - transport_height;
        const float left_width = std::clamp(size.width * 0.27F, 240.0F, 300.0F);
        const float gap = size.width < 960.0F ? 28.0F : 40.0F;
        const float cover_size = std::min(
            left_width,
            std::max(220.0F, transport_top - top - 185.0F));

        result.refresh_button = D2D1::RectF(
            size.width - 112.0F,
            12.0F,
            size.width - 72.0F,
            52.0F);
        result.eject_button = D2D1::RectF(
            size.width - 64.0F,
            12.0F,
            size.width - 24.0F,
            52.0F);
        result.cover = D2D1::RectF(
            margin,
            top,
            margin + cover_size,
            top + cover_size);
        result.track_list = D2D1::RectF(
            margin + left_width + gap,
            top + 52.0F,
            size.width - margin,
            transport_top - 12.0F);
        result.progress_hit = D2D1::RectF(
            margin,
            transport_top + 14.0F,
            size.width - margin,
            transport_top + 34.0F);

        const float center = size.width * 0.5F;
        const float controls_y = transport_top + transport_height * 0.57F;
        result.previous_button = D2D1::RectF(
            center - 96.0F,
            controls_y - 20.0F,
            center - 56.0F,
            controls_y + 20.0F);
        result.play_button = D2D1::RectF(
            center - 28.0F,
            controls_y - 28.0F,
            center + 28.0F,
            controls_y + 28.0F);
        result.next_button = D2D1::RectF(
            center + 56.0F,
            controls_y - 20.0F,
            center + 96.0F,
            controls_y + 20.0F);
        return result;
    }

    void paint()
    {
        PAINTSTRUCT paint_structure{};
        BeginPaint(window_, &paint_structure);
        if (SUCCEEDED(create_device_resources())) {
            render_target_->BeginDraw();
            render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
            render_target_->Clear(color(0x0C0E12));
            layout_ = calculate_layout();
            draw_header();
            draw_content();
            draw_transport();
            const HRESULT result = render_target_->EndDraw();
            if (result == D2DERR_RECREATE_TARGET) {
                discard_device_resources();
            }
        }
        EndPaint(window_, &paint_structure);
    }

    void draw_header()
    {
        const float width = layout_.width;
        render_target_->DrawLine(
            D2D1::Point2F(0.0F, 64.0F),
            D2D1::Point2F(width, 64.0F),
            border_brush_.Get(),
            1.0F);

        const auto logo_mark = D2D1::RoundedRect(
            D2D1::RectF(24.0F, 14.0F, 60.0F, 50.0F),
            10.0F,
            10.0F);
        render_target_->FillRoundedRectangle(logo_mark, accent_brush_.Get());
        draw_text(
            L"04",
            caption_format_.Get(),
            logo_mark.rect,
            accent_text_brush_.Get());
        draw_text(
            L"CD.404",
            logo_format_.Get(),
            D2D1::RectF(72.0F, 10.0F, 165.0F, 38.0F),
            text_brush_.Get());
        draw_text(
            L"AUDIO DISC PLAYER",
            caption_format_.Get(),
            D2D1::RectF(72.0F, 34.0F, 190.0F, 54.0F),
            muted_brush_.Get());

        const float device_left = layout_.width < 960.0F ? 202.0F : 224.0F;
        const float device_right = std::min(layout_.width - 360.0F, 540.0F);
        const auto device_pill = D2D1::RoundedRect(
            D2D1::RectF(device_left, 12.0F, device_right, 52.0F),
            10.0F,
            10.0F);
        render_target_->FillRoundedRectangle(device_pill, surface_brush_.Get());
        render_target_->DrawRoundedRectangle(device_pill, border_brush_.Get(), 1.0F);
        render_target_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(device_left + 20.0F, 32.0F), 4.0F, 4.0F),
            disc_.tracks.empty() ? warning_brush_.Get() : success_brush_.Get());
        const std::wstring drive_label = disc_.has_optical_drive
            ? L"音频 CD · 光驱"
            : L"未检测到光驱";
        draw_text(
            drive_label,
            body_format_.Get(),
            D2D1::RectF(device_left + 34.0F, 19.0F, device_right - 12.0F, 47.0F),
            text_brush_.Get());

        const float status_left = device_right + 18.0F;
        if (status_left < layout_.width - 130.0F) {
            draw_text(
                disc_loading_ ? L"正在读取目录…" : disc_.status,
                small_format_.Get(),
                D2D1::RectF(status_left, 21.0F, layout_.width - 132.0F, 48.0F),
                secondary_brush_.Get());
        }

        draw_icon_button(
            layout_.refresh_button,
            ControlIcon::refresh,
            hovered_control_ == HoveredControl::refresh,
            true);
        draw_icon_button(
            layout_.eject_button,
            ControlIcon::eject,
            hovered_control_ == HoveredControl::eject,
            disc_.drive.has_value());
    }

    void draw_content()
    {
        if (disc_loading_ && disc_.tracks.empty()) {
            draw_empty_state(
                L"正在读取光盘",
                L"先建立本地曲目结构，元数据将在后台继续补全");
            return;
        }
        if (disc_.tracks.empty()) {
            draw_empty_state(
                disc_.has_optical_drive ? L"插入一张音频 CD" : L"未检测到光驱",
                disc_.has_optical_drive
                    ? L"CD.404 会先显示曲目，再在后台补全资料"
                    : L"连接 USB 光驱后会自动刷新");
            return;
        }

        draw_cover();
        const float text_top = layout_.cover.bottom + 18.0F;
        draw_text(
            L"未知专辑",
            album_format_.Get(),
            D2D1::RectF(
                layout_.cover.left,
                text_top,
                layout_.cover.right,
                text_top + 40.0F),
            text_brush_.Get());
        draw_text(
            L"未知艺术家",
            body_format_.Get(),
            D2D1::RectF(
                layout_.cover.left,
                text_top + 42.0F,
                layout_.cover.right,
                text_top + 68.0F),
            secondary_brush_.Get());
        draw_text(
            L"正在查询 CD-TEXT 与 MusicBrainz",
            small_format_.Get(),
            D2D1::RectF(
                layout_.cover.left,
                text_top + 70.0F,
                layout_.cover.right,
                text_top + 94.0F),
            muted_brush_.Get());

        draw_status_pill(
            layout_.cover.left,
            text_top + 108.0F,
            92.0F,
            L"共享输出",
            accent_brush_.Get());
        draw_status_pill(
            layout_.cover.left + 102.0F,
            text_top + 108.0F,
            104.0F,
            playback_active_ ? L"连续流播放" : L"连续流就绪",
            success_brush_.Get());

        const float right_left = layout_.track_list.left;
        draw_text(
            L"曲目",
            heading_format_.Get(),
            D2D1::RectF(right_left, 84.0F, right_left + 130.0F, 122.0F),
            text_brush_.Get());
        draw_text(
            std::format(
                L"共 {} 轨 · {}",
                disc_.tracks.size(),
                format_duration(disc_.total_audio_frames)),
            small_format_.Get(),
            D2D1::RectF(right_left + 130.0F, 90.0F, layout_.width - 24.0F, 120.0F),
            secondary_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_TRAILING);
        draw_track_list();
    }

    void draw_cover()
    {
        const auto rounded = D2D1::RoundedRect(layout_.cover, 14.0F, 14.0F);
        cover_brush_->SetStartPoint(D2D1::Point2F(
            layout_.cover.left,
            layout_.cover.top));
        cover_brush_->SetEndPoint(D2D1::Point2F(
            layout_.cover.right,
            layout_.cover.bottom));
        render_target_->FillRoundedRectangle(rounded, cover_brush_.Get());

        const D2D1_POINT_2F center = D2D1::Point2F(
            (layout_.cover.left + layout_.cover.right) * 0.5F,
            (layout_.cover.top + layout_.cover.bottom) * 0.5F);
        const float radius = (layout_.cover.right - layout_.cover.left) * 0.315F;
        render_target_->FillEllipse(
            D2D1::Ellipse(center, radius, radius),
            disc_brush_.Get());
        for (int ring = 1; ring <= 4; ++ring) {
            const float ring_radius = radius * (0.35F + static_cast<float>(ring) * 0.13F);
            render_target_->DrawEllipse(
                D2D1::Ellipse(center, ring_radius, ring_radius),
                border_brush_.Get(),
                1.0F);
        }
        render_target_->FillEllipse(
            D2D1::Ellipse(center, radius * 0.19F, radius * 0.19F),
            accent_brush_.Get());
        render_target_->FillEllipse(
            D2D1::Ellipse(center, radius * 0.045F, radius * 0.045F),
            accent_text_brush_.Get());
        draw_text(
            L"CD.404",
            caption_format_.Get(),
            D2D1::RectF(
                layout_.cover.left + 18.0F,
                layout_.cover.bottom - 46.0F,
                layout_.cover.right - 18.0F,
                layout_.cover.bottom - 18.0F),
            text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    void draw_empty_state(
        const std::wstring& title,
        const std::wstring& description)
    {
        const float center_x = layout_.width * 0.5F;
        const float center_y = (64.0F + layout_.height - 124.0F) * 0.5F;
        render_target_->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(center_x, center_y - 56.0F), 54.0F, 54.0F),
            border_brush_.Get(),
            2.0F);
        render_target_->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(center_x, center_y - 56.0F), 18.0F, 18.0F),
            accent_brush_.Get(),
            3.0F);
        draw_text(
            title,
            heading_format_.Get(),
            D2D1::RectF(center_x - 260.0F, center_y + 22.0F, center_x + 260.0F, center_y + 58.0F),
            text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(
            description,
            body_format_.Get(),
            D2D1::RectF(center_x - 320.0F, center_y + 64.0F, center_x + 320.0F, center_y + 96.0F),
            secondary_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void draw_track_list()
    {
        const float row_height = layout_.height < 680.0F ? 40.0F : 44.0F;
        const float header_height = 28.0F;
        const auto list_background = D2D1::RoundedRect(
            layout_.track_list,
            12.0F,
            12.0F);
        render_target_->FillRoundedRectangle(list_background, surface_brush_.Get());
        render_target_->DrawRoundedRectangle(list_background, border_brush_.Get(), 1.0F);

        draw_text(
            L"#",
            caption_format_.Get(),
            D2D1::RectF(
                layout_.track_list.left + 10.0F,
                layout_.track_list.top + 4.0F,
                layout_.track_list.left + 52.0F,
                layout_.track_list.top + header_height),
            muted_brush_.Get());
        draw_text(
            L"标题",
            caption_format_.Get(),
            D2D1::RectF(
                layout_.track_list.left + 58.0F,
                layout_.track_list.top + 4.0F,
                layout_.track_list.right - 90.0F,
                layout_.track_list.top + header_height),
            muted_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(
            L"时长",
            caption_format_.Get(),
            D2D1::RectF(
                layout_.track_list.right - 76.0F,
                layout_.track_list.top + 4.0F,
                layout_.track_list.right - 14.0F,
                layout_.track_list.top + header_height),
            muted_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_TRAILING);

        track_hits_.clear();
        const float rows_top = layout_.track_list.top + header_height;
        const std::size_t visible_rows = static_cast<std::size_t>(std::max(
            1.0F,
            std::floor((layout_.track_list.bottom - rows_top) / row_height)));
        const std::size_t maximum_scroll = disc_.tracks.size() > visible_rows
            ? disc_.tracks.size() - visible_rows
            : 0;
        scroll_row_ = std::min(scroll_row_, maximum_scroll);

        render_target_->PushAxisAlignedClip(
            D2D1::RectF(
                layout_.track_list.left,
                rows_top,
                layout_.track_list.right,
                layout_.track_list.bottom),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const std::size_t end = std::min(
            disc_.tracks.size(),
            scroll_row_ + visible_rows + 1);
        for (std::size_t index = scroll_row_; index < end; ++index) {
            const float top = rows_top +
                              static_cast<float>(index - scroll_row_) * row_height;
            const D2D1_RECT_F row = D2D1::RectF(
                layout_.track_list.left + 4.0F,
                top,
                layout_.track_list.right - 4.0F,
                top + row_height);
            const bool selected = index == selected_track_;
            const bool hovered = hovered_track_ && *hovered_track_ == index;
            if (selected) {
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(row, 8.0F, 8.0F),
                    selection_brush_.Get());
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(row.left, row.top + 8.0F, row.left + 3.0F, row.bottom - 8.0F),
                        1.5F,
                        1.5F),
                    accent_brush_.Get());
            } else if (hovered) {
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(row, 8.0F, 8.0F),
                    hover_brush_.Get());
            }

            const auto& track = disc_.tracks[index];
            const std::wstring number = playback_active_ && selected
                ? L"▶"
                : std::format(L"{:02}", track.number);
            draw_text(
                number,
                caption_format_.Get(),
                D2D1::RectF(row.left + 8.0F, top + 5.0F, row.left + 49.0F, top + row_height - 4.0F),
                track.is_audio ? (selected ? accent_brush_.Get() : secondary_brush_.Get())
                               : muted_brush_.Get());
            draw_text(
                track.title,
                body_format_.Get(),
                D2D1::RectF(row.left + 56.0F, top + 6.0F, row.right - 88.0F, top + row_height - 4.0F),
                track.is_audio ? text_brush_.Get() : muted_brush_.Get());
            draw_text(
                track.is_audio ? format_duration(track.frame_count) : L"数据",
                small_format_.Get(),
                D2D1::RectF(row.right - 76.0F, top + 8.0F, row.right - 12.0F, top + row_height - 4.0F),
                track.is_audio ? secondary_brush_.Get() : warning_brush_.Get(),
                DWRITE_TEXT_ALIGNMENT_TRAILING);
            track_hits_.push_back(TrackHit{row, index});
        }
        render_target_->PopAxisAlignedClip();

        if (maximum_scroll != 0) {
            const float available = layout_.track_list.bottom - rows_top - 12.0F;
            const float thumb_height = std::max(
                28.0F,
                available * static_cast<float>(visible_rows) /
                    static_cast<float>(disc_.tracks.size()));
            const float thumb_top = rows_top + 6.0F +
                (available - thumb_height) * static_cast<float>(scroll_row_) /
                    static_cast<float>(maximum_scroll);
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(
                        layout_.track_list.right - 5.0F,
                        thumb_top,
                        layout_.track_list.right - 2.0F,
                        thumb_top + thumb_height),
                    1.5F,
                    1.5F),
                muted_brush_.Get());
        }
    }

    void draw_transport()
    {
        const float panel_top = layout_.progress_hit.top - 14.0F;
        render_target_->FillRectangle(
            D2D1::RectF(0.0F, panel_top, layout_.width, layout_.height),
            surface_brush_.Get());
        render_target_->DrawLine(
            D2D1::Point2F(0.0F, panel_top),
            D2D1::Point2F(layout_.width, panel_top),
            border_brush_.Get(),
            1.0F);

        const float progress_y = layout_.progress_hit.top;
        const float left = layout_.progress_hit.left;
        const float right = layout_.progress_hit.right;
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(left, progress_y, right, progress_y + 4.0F),
                2.0F,
                2.0F),
            border_brush_.Get());
        const float progress_right = left + (right - left) * playback_progress();
        if (progress_right > left) {
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(left, progress_y, progress_right, progress_y + 4.0F),
                    2.0F,
                    2.0F),
                accent_brush_.Get());
        }

        const float content_y = panel_top + 36.0F;
        const std::wstring current_title = selected_track_ < disc_.tracks.size()
            ? disc_.tracks[selected_track_].title
            : L"等待音频 CD";
        draw_text(
            current_title,
            body_format_.Get(),
            D2D1::RectF(24.0F, content_y, layout_.width * 0.35F, content_y + 25.0F),
            disc_.tracks.empty() ? muted_brush_.Get() : text_brush_.Get());
        draw_text(
            playback_active_ ? L"连续流播放中" : L"WASAPI 共享 · 44.1 kHz",
            small_format_.Get(),
            D2D1::RectF(24.0F, content_y + 27.0F, layout_.width * 0.35F, content_y + 51.0F),
            playback_active_ ? success_brush_.Get() : muted_brush_.Get());

        draw_round_control(
            layout_.previous_button,
            ControlIcon::previous,
            hovered_control_ == HoveredControl::previous,
            false);
        draw_round_control(
            layout_.play_button,
            playback_active_ ? ControlIcon::stop : ControlIcon::play,
            hovered_control_ == HoveredControl::play,
            true);
        draw_round_control(
            layout_.next_button,
            ControlIcon::next,
            hovered_control_ == HoveredControl::next,
            false);

        const float volume_right = layout_.width - 28.0F;
        const float volume_left = std::max(layout_.width * 0.77F, volume_right - 160.0F);
        draw_text(
            L"VOL",
            caption_format_.Get(),
            D2D1::RectF(volume_left - 48.0F, content_y + 13.0F, volume_left - 8.0F, content_y + 37.0F),
            muted_brush_.Get());
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(volume_left, content_y + 23.0F, volume_right, content_y + 27.0F),
                2.0F,
                2.0F),
            border_brush_.Get());
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(volume_left, content_y + 23.0F, volume_left + (volume_right - volume_left) * 0.72F, content_y + 27.0F),
                2.0F,
                2.0F),
            secondary_brush_.Get());

        if (!ui_message_.empty()) {
            draw_text(
                ui_message_,
                small_format_.Get(),
                D2D1::RectF(
                    layout_.width * 0.64F,
                    panel_top + 10.0F,
                    layout_.width - 24.0F,
                    panel_top + 31.0F),
                ui_message_is_error_ ? error_brush_.Get() : warning_brush_.Get(),
                DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }

    void draw_round_control(
        const D2D1_RECT_F rectangle,
        const ControlIcon icon,
        const bool hovered,
        const bool primary)
    {
        const D2D1_POINT_2F center = D2D1::Point2F(
            (rectangle.left + rectangle.right) * 0.5F,
            (rectangle.top + rectangle.bottom) * 0.5F);
        const float radius = (rectangle.right - rectangle.left) * 0.5F;
        ID2D1Brush* fill = primary
            ? accent_brush_.Get()
            : (hovered ? elevated_brush_.Get() : surface_brush_.Get());
        render_target_->FillEllipse(D2D1::Ellipse(center, radius, radius), fill);
        if (!primary) {
            render_target_->DrawEllipse(
                D2D1::Ellipse(center, radius, radius),
                border_brush_.Get(),
                1.0F);
        }
        draw_control_icon(
            icon,
            center,
            primary ? accent_text_brush_.Get() : text_brush_.Get());
    }

    void draw_icon_button(
        const D2D1_RECT_F rectangle,
        const ControlIcon icon,
        const bool hovered,
        const bool enabled)
    {
        const auto rounded = D2D1::RoundedRect(rectangle, 10.0F, 10.0F);
        render_target_->FillRoundedRectangle(
            rounded,
            hovered && enabled ? elevated_brush_.Get() : surface_brush_.Get());
        render_target_->DrawRoundedRectangle(rounded, border_brush_.Get(), 1.0F);
        draw_control_icon(
            icon,
            D2D1::Point2F(
                (rectangle.left + rectangle.right) * 0.5F,
                (rectangle.top + rectangle.bottom) * 0.5F),
            enabled ? secondary_brush_.Get() : muted_brush_.Get());
    }

    void fill_triangle(
        const D2D1_POINT_2F first,
        const D2D1_POINT_2F second,
        const D2D1_POINT_2F third,
        ID2D1Brush* brush)
    {
        ComPtr<ID2D1PathGeometry> geometry;
        if (FAILED(factory_->CreatePathGeometry(geometry.ReleaseAndGetAddressOf()))) {
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
            render_target_->FillGeometry(geometry.Get(), brush);
        }
    }

    void draw_control_icon(
        const ControlIcon icon,
        const D2D1_POINT_2F center,
        ID2D1Brush* brush)
    {
        constexpr float half = 7.0F;
        switch (icon) {
        case ControlIcon::play:
            fill_triangle(
                D2D1::Point2F(center.x - 4.5F, center.y - half),
                D2D1::Point2F(center.x + 7.0F, center.y),
                D2D1::Point2F(center.x - 4.5F, center.y + half),
                brush);
            break;
        case ControlIcon::stop:
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(
                        center.x - 6.0F,
                        center.y - 6.0F,
                        center.x + 6.0F,
                        center.y + 6.0F),
                    1.5F,
                    1.5F),
                brush);
            break;
        case ControlIcon::previous:
            render_target_->DrawLine(
                D2D1::Point2F(center.x - 6.5F, center.y - half),
                D2D1::Point2F(center.x - 6.5F, center.y + half),
                brush,
                2.0F);
            fill_triangle(
                D2D1::Point2F(center.x + 6.0F, center.y - half),
                D2D1::Point2F(center.x - 4.0F, center.y),
                D2D1::Point2F(center.x + 6.0F, center.y + half),
                brush);
            break;
        case ControlIcon::next:
            render_target_->DrawLine(
                D2D1::Point2F(center.x + 6.5F, center.y - half),
                D2D1::Point2F(center.x + 6.5F, center.y + half),
                brush,
                2.0F);
            fill_triangle(
                D2D1::Point2F(center.x - 6.0F, center.y - half),
                D2D1::Point2F(center.x + 4.0F, center.y),
                D2D1::Point2F(center.x - 6.0F, center.y + half),
                brush);
            break;
        case ControlIcon::eject:
            fill_triangle(
                D2D1::Point2F(center.x, center.y - 7.0F),
                D2D1::Point2F(center.x + 8.0F, center.y + 4.0F),
                D2D1::Point2F(center.x - 8.0F, center.y + 4.0F),
                brush);
            render_target_->DrawLine(
                D2D1::Point2F(center.x - 8.0F, center.y + 8.0F),
                D2D1::Point2F(center.x + 8.0F, center.y + 8.0F),
                brush,
                2.0F);
            break;
        case ControlIcon::refresh: {
            ComPtr<ID2D1PathGeometry> geometry;
            if (FAILED(factory_->CreatePathGeometry(geometry.ReleaseAndGetAddressOf()))) {
                break;
            }
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(geometry->Open(sink.ReleaseAndGetAddressOf()))) {
                break;
            }
            sink->BeginFigure(
                D2D1::Point2F(center.x + 6.5F, center.y - 3.5F),
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
                render_target_->DrawGeometry(geometry.Get(), brush, 2.0F);
            }
            fill_triangle(
                D2D1::Point2F(center.x + 7.0F, center.y - 7.5F),
                D2D1::Point2F(center.x + 9.0F, center.y - 1.0F),
                D2D1::Point2F(center.x + 2.5F, center.y - 2.5F),
                brush);
            break;
        }
        }
    }

    void draw_status_pill(
        const float left,
        const float top,
        const float width,
        const std::wstring& label,
        ID2D1Brush* dot_brush)
    {
        const D2D1_RECT_F rectangle = D2D1::RectF(left, top, left + width, top + 28.0F);
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(rectangle, 14.0F, 14.0F),
            surface_brush_.Get());
        render_target_->DrawRoundedRectangle(
            D2D1::RoundedRect(rectangle, 14.0F, 14.0F),
            border_brush_.Get(),
            1.0F);
        render_target_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(left + 14.0F, top + 14.0F), 3.0F, 3.0F),
            dot_brush);
        draw_text(
            label,
            caption_format_.Get(),
            D2D1::RectF(left + 21.0F, top, left + width - 8.0F, top + 28.0F),
            secondary_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    void draw_text(
        const std::wstring& text,
        IDWriteTextFormat* format,
        const D2D1_RECT_F rectangle,
        ID2D1Brush* brush,
        const DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING)
    {
        const DWRITE_TEXT_ALIGNMENT previous = format->GetTextAlignment();
        format->SetTextAlignment(alignment);
        render_target_->DrawTextW(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            format,
            rectangle,
            brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        format->SetTextAlignment(previous);
    }

    void refresh_disc()
    {
        if (disc_loading_ || disc_worker_.joinable()) {
            return;
        }
        stop_playback();
        disc_loading_ = true;
        ui_message_.clear();
        InvalidateRect(window_, nullptr, FALSE);
        const HWND target_window = window_;
        disc_worker_ = std::jthread([target_window] {
            auto snapshot = std::make_unique<DiscSnapshot>(load_disc_snapshot());
            auto* const raw_snapshot = snapshot.release();
            if (PostMessageW(
                    target_window,
                    kDiscReadyMessage,
                    0,
                    reinterpret_cast<LPARAM>(raw_snapshot)) == FALSE) {
                delete raw_snapshot;
            }
        });
    }

    void receive_disc_snapshot(std::unique_ptr<DiscSnapshot> snapshot)
    {
        if (disc_worker_.joinable()) {
            disc_worker_.join();
        }
        disc_loading_ = false;
        disc_ = std::move(*snapshot);
        selected_track_ = first_audio_track();
        scroll_row_ = 0;
        InvalidateRect(window_, nullptr, FALSE);
    }

    [[nodiscard]] std::size_t first_audio_track() const
    {
        const auto iterator = std::find_if(
            disc_.tracks.begin(),
            disc_.tracks.end(),
            [](const UiTrack& track) { return track.is_audio; });
        return iterator == disc_.tracks.end()
            ? 0
            : static_cast<std::size_t>(iterator - disc_.tracks.begin());
    }

    void update_hover(const D2D1_POINT_2F point)
    {
        if (!tracking_mouse_) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = window_;
            tracking_mouse_ = TrackMouseEvent(&tracking) != FALSE;
        }

        const auto previous_track = hovered_track_;
        const auto previous_control = hovered_control_;
        hovered_track_.reset();
        hovered_control_ = hit_control(point);
        for (const auto& hit : track_hits_) {
            if (contains(hit.rectangle, point)) {
                hovered_track_ = hit.track_index;
                break;
            }
        }
        if (previous_track != hovered_track_ || previous_control != hovered_control_) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    [[nodiscard]] HoveredControl hit_control(const D2D1_POINT_2F point) const
    {
        if (contains(layout_.refresh_button, point)) return HoveredControl::refresh;
        if (contains(layout_.eject_button, point)) return HoveredControl::eject;
        if (contains(layout_.previous_button, point)) return HoveredControl::previous;
        if (contains(layout_.play_button, point)) return HoveredControl::play;
        if (contains(layout_.next_button, point)) return HoveredControl::next;
        if (contains(layout_.progress_hit, point)) return HoveredControl::progress;
        return HoveredControl::none;
    }

    void handle_click(const D2D1_POINT_2F point)
    {
        for (const auto& hit : track_hits_) {
            if (contains(hit.rectangle, point)) {
                selected_track_ = hit.track_index;
                ensure_selected_track_visible();
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
        }

        switch (hit_control(point)) {
        case HoveredControl::refresh:
            refresh_disc();
            break;
        case HoveredControl::eject:
            eject_disc();
            break;
        case HoveredControl::previous:
            select_relative_track(-1);
            break;
        case HoveredControl::play:
            toggle_playback();
            break;
        case HoveredControl::next:
            select_relative_track(1);
            break;
        case HoveredControl::progress:
            seek_from_point(point.x);
            break;
        case HoveredControl::none:
            break;
        }
    }

    void handle_double_click(const D2D1_POINT_2F point)
    {
        for (const auto& hit : track_hits_) {
            if (contains(hit.rectangle, point)) {
                selected_track_ = hit.track_index;
                start_playback(0);
                return;
            }
        }
    }

    LRESULT handle_key_down(const WPARAM key)
    {
        switch (key) {
        case VK_SPACE:
        case VK_RETURN:
            toggle_playback();
            return 0;
        case VK_UP:
            select_relative_track(-1);
            return 0;
        case VK_DOWN:
            select_relative_track(1);
            return 0;
        case VK_HOME:
            selected_track_ = first_audio_track();
            ensure_selected_track_visible();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case VK_END:
            select_last_audio_track();
            return 0;
        case VK_ESCAPE:
            stop_playback();
            return 0;
        case VK_F5:
            refresh_disc();
            return 0;
        default:
            break;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (key == VK_LEFT) {
                select_relative_track(-1);
                return 0;
            }
            if (key == VK_RIGHT) {
                select_relative_track(1);
                return 0;
            }
            if (key == 'E') {
                eject_disc();
                return 0;
            }
        }
        return DefWindowProcW(window_, WM_KEYDOWN, key, 0);
    }

    void scroll_tracks(const short wheel_delta)
    {
        if (disc_.tracks.empty()) {
            return;
        }
        const int direction = wheel_delta > 0 ? -3 : 3;
        const auto next = static_cast<long long>(scroll_row_) + direction;
        scroll_row_ = static_cast<std::size_t>(std::max<long long>(next, 0));
        InvalidateRect(window_, nullptr, FALSE);
    }

    void select_relative_track(const int direction)
    {
        if (disc_.tracks.empty()) {
            return;
        }
        auto index = static_cast<long long>(selected_track_);
        while (true) {
            index += direction;
            if (index < 0 || index >= static_cast<long long>(disc_.tracks.size())) {
                return;
            }
            if (disc_.tracks[static_cast<std::size_t>(index)].is_audio) {
                stop_playback();
                selected_track_ = static_cast<std::size_t>(index);
                ensure_selected_track_visible();
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
        }
    }

    void select_last_audio_track()
    {
        for (std::size_t index = disc_.tracks.size(); index > 0; --index) {
            if (disc_.tracks[index - 1].is_audio) {
                stop_playback();
                selected_track_ = index - 1;
                ensure_selected_track_visible();
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
        }
    }

    void ensure_selected_track_visible()
    {
        const float row_height = layout_.height < 680.0F ? 40.0F : 44.0F;
        const float available = layout_.track_list.bottom - layout_.track_list.top - 28.0F;
        const std::size_t visible = static_cast<std::size_t>(
            std::max(1.0F, std::floor(available / row_height)));
        if (selected_track_ < scroll_row_) {
            scroll_row_ = selected_track_;
        } else if (selected_track_ >= scroll_row_ + visible) {
            scroll_row_ = selected_track_ - visible + 1;
        }
    }

    void toggle_playback()
    {
        if (playback_active_) {
            stop_playback();
        } else {
            start_playback(0);
        }
    }

    void start_playback(const unsigned int offset_seconds)
    {
        if (selected_track_ >= disc_.tracks.size() ||
            !disc_.tracks[selected_track_].is_audio) {
            return;
        }
        stop_playback();

        const std::filesystem::path probe_path = playback_probe_path();
        if (!std::filesystem::exists(probe_path)) {
            ui_message_ = L"找不到播放组件，请先构建 cd404_play_probe";
            ui_message_is_error_ = true;
            return;
        }

        std::wstring command = std::format(
            L"\"{}\" --track {} --all",
            probe_path.wstring(),
            disc_.tracks[selected_track_].number);
        if (offset_seconds != 0) {
            command += std::format(L" --offset-seconds {}", offset_seconds);
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (CreateProcessW(
                nullptr,
                command.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                probe_path.parent_path().c_str(),
                &startup,
                &process) == FALSE) {
            ui_message_ = L"无法启动播放组件：" +
                          platform::windows::format_system_error(GetLastError());
            ui_message_is_error_ = true;
            return;
        }

        CloseHandle(process.hThread);
        playback_process_ = process.hProcess;
        playback_active_ = true;
        playback_start_track_ = selected_track_;
        playback_start_offset_seconds_ = offset_seconds;
        playback_started_at_ = GetTickCount64();
        playback_track_seconds_ = static_cast<double>(offset_seconds);
        ui_message_.clear();
        ui_message_is_error_ = false;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void stop_playback()
    {
        if (playback_process_ != nullptr) {
            DWORD exit_code{};
            if (GetExitCodeProcess(playback_process_, &exit_code) != FALSE &&
                exit_code == STILL_ACTIVE) {
                static_cast<void>(TerminateProcess(playback_process_, 130));
                static_cast<void>(WaitForSingleObject(playback_process_, 1'000));
            }
            CloseHandle(playback_process_);
            playback_process_ = nullptr;
        }
        playback_active_ = false;
        playback_track_seconds_ = 0.0;
        if (window_ != nullptr) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void update_playback_clock()
    {
        if (!playback_active_ || playback_process_ == nullptr) {
            return;
        }

        DWORD exit_code{};
        if (GetExitCodeProcess(playback_process_, &exit_code) == FALSE ||
            exit_code != STILL_ACTIVE) {
            CloseHandle(playback_process_);
            playback_process_ = nullptr;
            playback_active_ = false;
            return;
        }

        const double elapsed = static_cast<double>(GetTickCount64() - playback_started_at_) /
                               1'000.0;
        double disc_seconds = elapsed +
                              static_cast<double>(playback_start_offset_seconds_);
        std::size_t track_index = playback_start_track_;
        while (track_index < disc_.tracks.size()) {
            if (!disc_.tracks[track_index].is_audio) {
                break;
            }
            const double duration = static_cast<double>(
                disc_.tracks[track_index].frame_count) /
                static_cast<double>(core::kCdSampleFramesPerSecond);
            if (disc_seconds < duration) {
                selected_track_ = track_index;
                playback_track_seconds_ = disc_seconds;
                ensure_selected_track_visible();
                return;
            }
            disc_seconds -= duration;
            ++track_index;
        }
    }

    [[nodiscard]] float playback_progress() const
    {
        if (selected_track_ >= disc_.tracks.size()) {
            return 0.0F;
        }
        const double duration = static_cast<double>(
            disc_.tracks[selected_track_].frame_count) /
            static_cast<double>(core::kCdSampleFramesPerSecond);
        return duration <= 0.0
            ? 0.0F
            : clamp01(static_cast<float>(playback_track_seconds_ / duration));
    }

    void seek_from_point(const float x)
    {
        if (selected_track_ >= disc_.tracks.size() ||
            !disc_.tracks[selected_track_].is_audio) {
            return;
        }
        const float ratio = clamp01(
            (x - layout_.progress_hit.left) /
            (layout_.progress_hit.right - layout_.progress_hit.left));
        const auto total_seconds = static_cast<unsigned int>(
            disc_.tracks[selected_track_].frame_count /
            core::kCdSampleFramesPerSecond);
        const unsigned int offset = static_cast<unsigned int>(
            std::floor(static_cast<float>(total_seconds) * ratio));
        start_playback(std::min(offset, total_seconds > 0 ? total_seconds - 1 : 0));
    }

    void eject_disc()
    {
        if (!disc_.drive) {
            return;
        }
        stop_playback();
        const unsigned long error = platform::windows::eject_media(*disc_.drive);
        if (error != ERROR_SUCCESS) {
            ui_message_ = L"无法弹出光盘：" +
                          platform::windows::format_system_error(error);
            ui_message_is_error_ = true;
        } else {
            disc_.tracks.clear();
            disc_.total_audio_frames = 0;
            disc_.status = L"光盘已弹出";
            ui_message_ = L"光盘已安全弹出";
            ui_message_is_error_ = false;
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    [[nodiscard]] std::filesystem::path playback_probe_path() const
    {
        std::wstring module_path(32'768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr,
            module_path.data(),
            static_cast<DWORD>(module_path.size()));
        module_path.resize(length);
        const std::filesystem::path app_directory =
            std::filesystem::path(module_path).parent_path();
        return app_directory.parent_path() /
               L"play_probe" /
               L"cd404_play_probe.exe";
    }

    [[nodiscard]] float pixel_to_dip(const int pixel) const
    {
        const float dpi = static_cast<float>(GetDpiForWindow(window_));
        return static_cast<float>(pixel) * 96.0F / dpi;
    }

    HINSTANCE instance_{};
    HWND window_{};
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_factory_;
    ComPtr<ID2D1HwndRenderTarget> render_target_;
    ComPtr<ID2D1SolidColorBrush> background_brush_;
    ComPtr<ID2D1SolidColorBrush> surface_brush_;
    ComPtr<ID2D1SolidColorBrush> elevated_brush_;
    ComPtr<ID2D1SolidColorBrush> border_brush_;
    ComPtr<ID2D1SolidColorBrush> text_brush_;
    ComPtr<ID2D1SolidColorBrush> secondary_brush_;
    ComPtr<ID2D1SolidColorBrush> muted_brush_;
    ComPtr<ID2D1SolidColorBrush> accent_brush_;
    ComPtr<ID2D1SolidColorBrush> accent_text_brush_;
    ComPtr<ID2D1SolidColorBrush> success_brush_;
    ComPtr<ID2D1SolidColorBrush> warning_brush_;
    ComPtr<ID2D1SolidColorBrush> error_brush_;
    ComPtr<ID2D1SolidColorBrush> selection_brush_;
    ComPtr<ID2D1SolidColorBrush> hover_brush_;
    ComPtr<ID2D1SolidColorBrush> disc_brush_;
    ComPtr<ID2D1LinearGradientBrush> cover_brush_;
    ComPtr<IDWriteTextFormat> logo_format_;
    ComPtr<IDWriteTextFormat> album_format_;
    ComPtr<IDWriteTextFormat> heading_format_;
    ComPtr<IDWriteTextFormat> body_format_;
    ComPtr<IDWriteTextFormat> small_format_;
    ComPtr<IDWriteTextFormat> caption_format_;
    ComPtr<IDWriteTextFormat> icon_format_;
    DiscSnapshot disc_;
    std::jthread disc_worker_;
    bool disc_loading_{};
    Layout layout_{};
    std::vector<TrackHit> track_hits_;
    std::optional<std::size_t> hovered_track_;
    HoveredControl hovered_control_{HoveredControl::none};
    bool tracking_mouse_{};
    std::size_t selected_track_{};
    std::size_t scroll_row_{};
    HANDLE playback_process_{};
    bool playback_active_{};
    std::size_t playback_start_track_{};
    unsigned int playback_start_offset_seconds_{};
    ULONGLONG playback_started_at_{};
    double playback_track_seconds_{};
    std::wstring ui_message_;
    bool ui_message_is_error_{};
};

void enable_per_monitor_dpi_awareness()
{
    using SetDpiAwarenessContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto set_awareness = reinterpret_cast<SetDpiAwarenessContext>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_awareness != nullptr) {
        static_cast<void>(set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    } else {
        static_cast<void>(SetProcessDPIAware());
    }
}

} // namespace

int run_application(HINSTANCE instance, const int show_command)
{
    enable_per_monitor_dpi_awareness();
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        return 1;
    }

    int exit_code{1};
    {
        MainWindow window(instance);
        if (window.create(show_command)) {
            exit_code = window.run();
        }
    }
    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }
    return exit_code;
}

} // namespace cd404::ui
