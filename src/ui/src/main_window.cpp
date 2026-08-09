#include <windows.h>
#include <windowsx.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cd404/audio/playback_recovery.hpp>
#include <cd404/core/cd_time.hpp>
#include <cd404/listenbrainz/playback_tracker.hpp>
#include <cd404/platform/windows/cdda_playback_engine.hpp>
#include <cd404/platform/windows/device_lifecycle.hpp>
#include <cd404/platform/windows/listenbrainz_reporter.hpp>
#include <cd404/platform/windows/online_metadata.hpp>
#include <cd404/platform/windows/optical_drive.hpp>
#include <cd404/platform/windows/system_media_controls.hpp>
#include <cd404/platform/windows/user_settings.hpp>
#include <cd404/ui/main_window.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cd404::ui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClassName[] = L"CD404.MainWindow";
constexpr wchar_t kWindowTitle[] = L"CD.404";
constexpr UINT kDiscReadyMessage = WM_APP + 1;
constexpr UINT kMetadataReadyMessage = WM_APP + 2;
constexpr UINT kPlaybackReadyMessage = WM_APP + 3;
constexpr UINT kSystemMediaRequestMessage = WM_APP + 4;
constexpr UINT kSettingsSaveMessage = WM_APP + 5;
constexpr UINT kSettingsCloseMessage = WM_APP + 6;
constexpr int kSettingsTokenEditId = 1'001;
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

[[nodiscard]] std::int64_t unix_time_now() noexcept
{
    return static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
}

[[nodiscard]] std::wstring to_wstring(const std::u16string_view text)
{
    std::wstring result;
    result.reserve(text.size());
    std::ranges::transform(text, std::back_inserter(result), [](const char16_t character) {
        return static_cast<wchar_t>(character);
    });
    return result;
}

struct UiTrack final {
    std::uint8_t number{};
    core::SampleFrame frame_count{};
    bool is_audio{};
    bool has_metadata_title{};
    std::wstring title;
    std::wstring artist;
    std::wstring track_mbid;
    std::wstring recording_mbid;
    std::vector<std::wstring> artist_mbids;
};

struct DiscSnapshot final {
    std::optional<platform::windows::OpticalDrive> drive;
    std::optional<disc::Toc> toc;
    std::vector<UiTrack> tracks;
    core::SampleFrame total_audio_frames{};
    std::wstring album_title;
    std::wstring album_artist;
    std::wstring release_mbid;
    std::wstring release_group_mbid;
    std::wstring metadata_source;
    std::filesystem::path cover_art_path;
    std::wstring status;
    bool has_cd_text{};
    bool has_optical_drive{};
};

struct OnlineMetadataSnapshot final {
    std::optional<platform::windows::OnlineMetadata> metadata;
    std::uint64_t disc_generation{};
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
        snapshot.toc = *toc_result.toc;
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

        const auto cd_text_result = platform::windows::read_cd_text(drive);
        if (cd_text_result.metadata) {
            snapshot.has_cd_text = true;
            snapshot.metadata_source = L"CD-TEXT";
            snapshot.album_title = to_wstring(cd_text_result.metadata->album_title);
            snapshot.album_artist = to_wstring(cd_text_result.metadata->album_performer);
            for (auto& track : snapshot.tracks) {
                const auto& metadata = cd_text_result.metadata->tracks[track.number];
                if (!metadata.title.empty()) {
                    track.title = to_wstring(metadata.title);
                    track.has_metadata_title = true;
                }
                if (!metadata.performer.empty()) {
                    track.artist = to_wstring(metadata.performer);
                }
            }
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

struct ScrollbarGeometry final {
    D2D1_RECT_F track{};
    D2D1_RECT_F thumb{};
    std::size_t visible_rows{};
    std::size_t maximum_scroll{};
    bool visible{};
};

struct Layout final {
    float width{};
    float height{};
    D2D1_RECT_F refresh_button{};
    D2D1_RECT_F eject_button{};
    D2D1_RECT_F settings_button{};
    D2D1_RECT_F cover{};
    D2D1_RECT_F track_list{};
    D2D1_RECT_F progress_hit{};
    D2D1_RECT_F previous_button{};
    D2D1_RECT_F play_button{};
    D2D1_RECT_F next_button{};
    D2D1_RECT_F volume_hit{};
    D2D1_RECT_F settings_page{};
    D2D1_RECT_F settings_listenbrainz_card{};
    D2D1_RECT_F settings_back{};
    D2D1_RECT_F settings_edit{};
    D2D1_RECT_F settings_save{};
    D2D1_RECT_F settings_clear{};
    D2D1_RECT_F settings_listenbrainz_toggle{};
};

class MainWindow final {
public:
    explicit MainWindow(HINSTANCE instance)
        : instance_(instance),
          user_settings_(platform::windows::load_user_settings()),
          volume_(user_settings_.volume),
          listenbrainz_tracker_([this](const listenbrainz::Submission& submission) {
              listenbrainz_reporter_.submit(submission);
          })
    {
        listenbrainz_reporter_.set_reporting_enabled(
            user_settings_.listenbrainz_reporting_enabled);
    }

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    ~MainWindow()
    {
        close_settings();
        stop_playback();
        user_settings_.volume = volume_;
        static_cast<void>(platform::windows::save_user_settings(user_settings_));
        if (settings_font_ != nullptr) {
            DeleteObject(settings_font_);
        }
        if (settings_edit_brush_ != nullptr) {
            DeleteObject(settings_edit_brush_);
        }
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

        static_cast<void>(system_media_controls_.initialize(
            window_,
            [target_window = window_](const platform::windows::SystemMediaRequest request) {
                static_cast<void>(PostMessageW(
                    target_window,
                    kSystemMediaRequestMessage,
                    static_cast<WPARAM>(request.command),
                    static_cast<LPARAM>(request.position_milliseconds)));
            }));
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
            if (volume_dragging_) {
                set_volume_from_point(pixel_to_dip(GET_X_LPARAM(lparam)));
                return 0;
            }
            if (scrollbar_dragging_) {
                update_scrollbar_drag(D2D1::Point2F(
                    pixel_to_dip(GET_X_LPARAM(lparam)),
                    pixel_to_dip(GET_Y_LPARAM(lparam))));
                return 0;
            }
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
            if (active_page_ == AppPage::settings) {
                return 0;
            }
            handle_mouse_wheel(
                GET_WHEEL_DELTA_WPARAM(wparam),
                GET_X_LPARAM(lparam),
                GET_Y_LPARAM(lparam));
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(window_);
            if (active_page_ == AppPage::settings) {
                handle_settings_click(D2D1::Point2F(
                    pixel_to_dip(GET_X_LPARAM(lparam)),
                    pixel_to_dip(GET_Y_LPARAM(lparam))));
                return 0;
            }
            if (contains(layout_.volume_hit, D2D1::Point2F(
                    pixel_to_dip(GET_X_LPARAM(lparam)),
                    pixel_to_dip(GET_Y_LPARAM(lparam))))) {
                volume_dragging_ = true;
                SetCapture(window_);
                set_volume_from_point(pixel_to_dip(GET_X_LPARAM(lparam)));
                return 0;
            }
            if (handle_scrollbar_press(D2D1::Point2F(
                    pixel_to_dip(GET_X_LPARAM(lparam)),
                    pixel_to_dip(GET_Y_LPARAM(lparam))))) {
                return 0;
            }
            handle_click(D2D1::Point2F(
                pixel_to_dip(GET_X_LPARAM(lparam)),
                pixel_to_dip(GET_Y_LPARAM(lparam))));
            return 0;
        case WM_LBUTTONUP:
            if (volume_dragging_) {
                volume_dragging_ = false;
                ReleaseCapture();
                persist_user_settings();
                return 0;
            }
            if (scrollbar_dragging_) {
                scrollbar_dragging_ = false;
                ReleaseCapture();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            volume_dragging_ = false;
            scrollbar_dragging_ = false;
            return 0;
        case WM_LBUTTONDBLCLK:
            // The first click in a Windows double-click sequence has already
            // performed the action. Ignore the second notification so track
            // playback and settings toggles are not restarted or reversed.
            return 0;
        case WM_KEYDOWN:
            return handle_key_down(wparam);
        case WM_CTLCOLOREDIT:
            if (reinterpret_cast<HWND>(lparam) == settings_token_edit_) {
                const auto device_context = reinterpret_cast<HDC>(wparam);
                SetTextColor(device_context, RGB(242, 244, 248));
                SetBkColor(device_context, RGB(29, 33, 42));
                return reinterpret_cast<LRESULT>(settings_edit_brush_);
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT &&
                (hovered_track_ || hovered_control_ != HoveredControl::none)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_DEVICECHANGE:
        case WM_POWERBROADCAST:
            handle_device_lifecycle(
                platform::windows::classify_device_lifecycle_message(
                    message,
                    wparam,
                    reinterpret_cast<const void*>(lparam)));
            return message == WM_POWERBROADCAST ? TRUE : 0;
        case WM_TIMER:
            if (wparam == kAnimationTimer) {
                update_playback_clock();
                maybe_persist_playback_position();
                if (active_page_ == AppPage::settings && settings_saved_ &&
                    listenbrainz_reporter_.status().state !=
                        platform::windows::ListenBrainzState::validating) {
                    settings_saved_ = false;
                }
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case kDiscReadyMessage:
            receive_disc_snapshot(
                std::unique_ptr<DiscSnapshot>(
                    reinterpret_cast<DiscSnapshot*>(lparam)));
            return 0;
        case kMetadataReadyMessage:
            receive_online_metadata(
                std::unique_ptr<OnlineMetadataSnapshot>(
                    reinterpret_cast<OnlineMetadataSnapshot*>(lparam)));
            return 0;
        case kPlaybackReadyMessage:
            receive_playback_result(static_cast<std::uint64_t>(wparam));
            return 0;
        case kSystemMediaRequestMessage:
            handle_system_media_request(platform::windows::SystemMediaRequest{
                static_cast<platform::windows::SystemMediaCommand>(wparam),
                static_cast<std::uint64_t>(lparam),
            });
            return 0;
        case kSettingsSaveMessage:
            save_settings();
            return 0;
        case kSettingsCloseMessage:
            close_settings();
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kAnimationTimer);
            persist_playback_position();
            persist_user_settings();
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
        settings,
        previous,
        play,
        next,
        progress,
        volume,
    };

    enum class AppPage {
        player,
        settings,
    };

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

        result = CoCreateInstance(
            CLSID_WICImagingFactory2,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(imaging_factory_.ReleaseAndGetAddressOf()));
        if (FAILED(result)) {
            result = CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(imaging_factory_.ReleaseAndGetAddressOf()));
        }
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
                14.0F,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                button_format_);
        }
        if (SUCCEEDED(result)) {
            result = create_text_format(
                15.0F,
                DWRITE_FONT_WEIGHT_NORMAL,
                track_format_);
        }
        if (SUCCEEDED(result)) {
            result = create_text_format(
                13.0F,
                DWRITE_FONT_WEIGHT_NORMAL,
                track_duration_format_);
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
        button_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        button_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        caption_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        caption_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        track_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        track_duration_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
        if (SUCCEEDED(result) && !disc_.cover_art_path.empty()) {
            load_cover_bitmap(disc_.cover_art_path);
        }
        return result;
    }

    void load_cover_bitmap(const std::filesystem::path& path)
    {
        cover_bitmap_.Reset();
        if (!render_target_ || !imaging_factory_ || path.empty()) {
            return;
        }

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(imaging_factory_->CreateDecoderFromFilename(
                path.c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                decoder.ReleaseAndGetAddressOf()))) {
            return;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf()))) {
            return;
        }
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(imaging_factory_->CreateFormatConverter(
                converter.ReleaseAndGetAddressOf())) ||
            FAILED(converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeMedianCut))) {
            return;
        }
        static_cast<void>(render_target_->CreateBitmapFromWicBitmap(
            converter.Get(),
            nullptr,
            cover_bitmap_.ReleaseAndGetAddressOf()));
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
        cover_bitmap_.Reset();
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
        const float cover_top = top + 52.0F;

        result.refresh_button = D2D1::RectF(
            size.width - 160.0F,
            12.0F,
            size.width - 120.0F,
            52.0F);
        result.eject_button = D2D1::RectF(
            size.width - 112.0F,
            12.0F,
            size.width - 72.0F,
            52.0F);
        result.settings_button = D2D1::RectF(
            size.width - 64.0F,
            12.0F,
            size.width - 24.0F,
            52.0F);
        result.cover = D2D1::RectF(
            margin,
            cover_top,
            margin + cover_size,
            cover_top + cover_size);
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
        const float volume_right = size.width - 24.0F;
        const float volume_left = std::max(size.width * 0.77F, volume_right - 164.0F);
        result.volume_hit = D2D1::RectF(
            volume_left - 4.0F,
            controls_y - 16.0F,
            volume_right + 4.0F,
            controls_y + 16.0F);

        result.settings_page = D2D1::RectF(
            margin,
            84.0F,
            size.width - margin,
            size.height - 24.0F);
        result.settings_back = D2D1::RectF(
            size.width - margin - 132.0F,
            88.0F,
            size.width - margin,
            128.0F);
        result.settings_listenbrainz_card = D2D1::RectF(
            margin,
            148.0F,
            size.width - margin,
            342.0F);
        result.settings_edit = D2D1::RectF(
            result.settings_listenbrainz_card.left + 24.0F,
            result.settings_listenbrainz_card.top + 112.0F,
            result.settings_listenbrainz_card.right - 224.0F,
            result.settings_listenbrainz_card.top + 156.0F);
        result.settings_clear = D2D1::RectF(
            result.settings_listenbrainz_card.right - 208.0F,
            result.settings_listenbrainz_card.top + 112.0F,
            result.settings_listenbrainz_card.right - 112.0F,
            result.settings_listenbrainz_card.top + 156.0F);
        result.settings_save = D2D1::RectF(
            result.settings_listenbrainz_card.right - 104.0F,
            result.settings_listenbrainz_card.top + 112.0F,
            result.settings_listenbrainz_card.right - 24.0F,
            result.settings_listenbrainz_card.top + 156.0F);
        result.settings_listenbrainz_toggle = D2D1::RectF(
            result.settings_listenbrainz_card.right - 76.0F,
            result.settings_listenbrainz_card.top + 22.0F,
            result.settings_listenbrainz_card.right - 24.0F,
            result.settings_listenbrainz_card.top + 50.0F);
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
            if (active_page_ == AppPage::settings) {
                update_settings_edit_bounds();
                draw_settings_page();
            } else {
                draw_content();
                draw_transport();
            }
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
        if ((disc_loading_ || disc_.tracks.empty()) &&
            status_left < layout_.width - 178.0F) {
            draw_text(
                disc_loading_ ? L"正在读取目录…" : disc_.status,
                small_format_.Get(),
                D2D1::RectF(status_left, 21.0F, layout_.width - 180.0F, 48.0F),
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
        draw_icon_button(
            layout_.settings_button,
            ControlIcon::settings,
            hovered_control_ == HoveredControl::settings ||
                active_page_ == AppPage::settings,
            true);
    }

    void draw_content()
    {
        if (disc_loading_ && disc_.tracks.empty()) {
            draw_empty_state(
                L"正在读取光盘",
                L"");
            return;
        }
        if (disc_.tracks.empty()) {
            draw_empty_state(
                disc_.has_optical_drive ? L"插入一张音频 CD" : L"未检测到光驱",
                disc_.has_optical_drive
                    ? L""
                    : L"连接 USB 光驱后会自动刷新");
            return;
        }

        draw_cover();
        const float album_info_height = 88.0F;
        const float text_top = layout_.track_list.bottom - album_info_height;
        draw_text(
            disc_.album_title.empty() ? L"未知专辑" : disc_.album_title,
            album_format_.Get(),
            D2D1::RectF(
                layout_.cover.left,
                text_top,
                layout_.cover.right,
                text_top + 36.0F),
            text_brush_.Get());
        draw_text(
            disc_.album_artist.empty() ? L"未知艺术家" : disc_.album_artist,
            body_format_.Get(),
            D2D1::RectF(
                layout_.cover.left,
                text_top + 36.0F,
                layout_.cover.right,
                text_top + 60.0F),
            secondary_brush_.Get());
        if (!disc_.metadata_source.empty()) {
            draw_metadata_source_pill(
                layout_.cover.left,
                text_top + 64.0F,
                layout_.cover.right - layout_.cover.left,
                disc_.metadata_source);
        }
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
        if (cover_bitmap_) {
            const D2D1_SIZE_U bitmap_size = cover_bitmap_->GetPixelSize();
            const float cover_width = layout_.cover.right - layout_.cover.left;
            const float cover_height = layout_.cover.bottom - layout_.cover.top;
            const float scale = std::min(
                cover_width / static_cast<float>(bitmap_size.width),
                cover_height / static_cast<float>(bitmap_size.height));
            const float image_width = static_cast<float>(bitmap_size.width) * scale;
            const float image_height = static_cast<float>(bitmap_size.height) * scale;
            const float image_left =
                layout_.cover.left + (cover_width - image_width) * 0.5F;
            const float image_top = layout_.track_list.top;
            const D2D1_RECT_F image_rect = D2D1::RectF(
                image_left,
                image_top,
                image_left + image_width,
                image_top + image_height);
            const auto rounded = D2D1::RoundedRect(image_rect, 14.0F, 14.0F);

            ComPtr<ID2D1RoundedRectangleGeometry> clip_geometry;
            ComPtr<ID2D1Layer> layer;
            if (SUCCEEDED(factory_->CreateRoundedRectangleGeometry(
                    rounded,
                    clip_geometry.ReleaseAndGetAddressOf())) &&
                SUCCEEDED(render_target_->CreateLayer(
                    nullptr,
                    layer.ReleaseAndGetAddressOf()))) {
                render_target_->PushLayer(
                    D2D1::LayerParameters(
                        image_rect,
                        clip_geometry.Get()),
                    layer.Get());
                render_target_->DrawBitmap(
                    cover_bitmap_.Get(),
                    image_rect,
                    1.0F,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                render_target_->PopLayer();
                render_target_->DrawRoundedRectangle(rounded, border_brush_.Get(), 1.0F);
                return;
            }
        }
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
        const float content_top = 64.0F;
        const float content_bottom = layout_.progress_hit.top - 14.0F;
        const float group_top_offset = -110.0F;
        const float group_bottom_offset = description.empty() ? 58.0F : 96.0F;
        const float center_y =
            (content_top + content_bottom - group_top_offset - group_bottom_offset) *
            0.5F;
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

    [[nodiscard]] ScrollbarGeometry track_scrollbar_geometry() const
    {
        const float row_height = layout_.height < 680.0F ? 40.0F : 44.0F;
        const float rows_top = layout_.track_list.top + 28.0F;
        ScrollbarGeometry result;
        result.visible_rows = static_cast<std::size_t>(std::max(
            1.0F,
            std::floor((layout_.track_list.bottom - rows_top) / row_height)));
        result.maximum_scroll = disc_.tracks.size() > result.visible_rows
            ? disc_.tracks.size() - result.visible_rows
            : 0;
        result.visible = result.maximum_scroll != 0;
        result.track = D2D1::RectF(
            layout_.track_list.right - 14.0F,
            rows_top + 6.0F,
            layout_.track_list.right,
            layout_.track_list.bottom - 6.0F);
        if (!result.visible) {
            return result;
        }

        const float available = result.track.bottom - result.track.top;
        const float thumb_height = std::max(
            28.0F,
            available * static_cast<float>(result.visible_rows) /
                static_cast<float>(disc_.tracks.size()));
        const float travel = available - thumb_height;
        const float thumb_top = result.track.top +
            travel * static_cast<float>(std::min(scroll_row_, result.maximum_scroll)) /
                static_cast<float>(result.maximum_scroll);
        result.thumb = D2D1::RectF(
            result.track.left,
            thumb_top,
            result.track.right,
            thumb_top + thumb_height);
        return result;
    }

    void draw_track_list()
    {
        const float row_height = layout_.height < 680.0F ? 40.0F : 44.0F;
        const float header_height = 28.0F;
        const ScrollbarGeometry scrollbar = track_scrollbar_geometry();
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
        const std::size_t visible_rows = scrollbar.visible_rows;
        scroll_row_ = std::min(scroll_row_, scrollbar.maximum_scroll);

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
                track_format_.Get(),
                D2D1::RectF(row.left + 56.0F, top, row.right - 88.0F, top + row_height),
                track.is_audio ? text_brush_.Get() : muted_brush_.Get());
            draw_text(
                track.is_audio ? format_duration(track.frame_count) : L"数据",
                track_duration_format_.Get(),
                D2D1::RectF(row.right - 76.0F, top, row.right - 12.0F, top + row_height),
                track.is_audio ? secondary_brush_.Get() : warning_brush_.Get(),
                DWRITE_TEXT_ALIGNMENT_TRAILING);
            track_hits_.push_back(TrackHit{row, index});
        }
        render_target_->PopAxisAlignedClip();

        if (scrollbar.visible) {
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(
                        layout_.track_list.right - 6.0F,
                        scrollbar.thumb.top,
                        layout_.track_list.right - 2.0F,
                        scrollbar.thumb.bottom),
                    2.0F,
                    2.0F),
                scrollbar_dragging_ ? secondary_brush_.Get() : muted_brush_.Get());
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
        const core::SampleFrame current_track_frames = playback_track_frame_;
        const core::SampleFrame selected_duration =
            selected_track_ < disc_.tracks.size()
                ? disc_.tracks[selected_track_].frame_count
                : 0;
        draw_text(
            format_duration(std::clamp<core::SampleFrame>(
                current_track_frames,
                0,
                selected_duration)),
            caption_format_.Get(),
            D2D1::RectF(left, progress_y + 7.0F, left + 90.0F, progress_y + 27.0F),
            muted_brush_.Get());
        draw_text(
            format_duration(selected_duration),
            caption_format_.Get(),
            D2D1::RectF(right - 90.0F, progress_y + 7.0F, right, progress_y + 27.0F),
            muted_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_TRAILING);
        const float content_center_y =
            (layout_.play_button.top + layout_.play_button.bottom) * 0.5F;
        const float content_top = content_center_y - 24.0F;
        const std::wstring current_title = selected_track_ < disc_.tracks.size()
            ? disc_.tracks[selected_track_].title
            : L"等待音频 CD";
        const std::wstring current_artist = selected_track_ < disc_.tracks.size() &&
                !disc_.tracks[selected_track_].artist.empty()
            ? disc_.tracks[selected_track_].artist
            : disc_.album_artist;
        draw_text(
            current_title,
            heading_format_.Get(),
            D2D1::RectF(
                24.0F,
                content_top,
                layout_.width * 0.35F,
                content_top + 28.0F),
            disc_.tracks.empty() ? muted_brush_.Get() : text_brush_.Get());
        if (!current_artist.empty()) {
            draw_text(
                current_artist,
                small_format_.Get(),
                D2D1::RectF(
                    24.0F,
                    content_top + 28.0F,
                    layout_.width * 0.35F,
                    content_top + 50.0F),
                secondary_brush_.Get());
        }
        draw_round_control(
            layout_.previous_button,
            ControlIcon::previous,
            hovered_control_ == HoveredControl::previous,
            false);
        draw_round_control(
            layout_.play_button,
            playback_active_ && !playback_paused_
                ? ControlIcon::pause
                : ControlIcon::play,
            hovered_control_ == HoveredControl::play,
            true);
        draw_round_control(
            layout_.next_button,
            ControlIcon::next,
            hovered_control_ == HoveredControl::next,
            false);

        const float volume_left = layout_.volume_hit.left + 4.0F;
        const float volume_right = layout_.volume_hit.right - 4.0F;
        const float volume_center_y =
            (layout_.play_button.top + layout_.play_button.bottom) * 0.5F;
        draw_text(
            L"VOL",
            caption_format_.Get(),
            D2D1::RectF(
                volume_left - 48.0F,
                volume_center_y - 12.0F,
                volume_left - 8.0F,
                volume_center_y + 12.0F),
            muted_brush_.Get());
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(
                    volume_left,
                    volume_center_y - 2.0F,
                    volume_right,
                    volume_center_y + 2.0F),
                2.0F,
                2.0F),
            border_brush_.Get());
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(
                    volume_left,
                    volume_center_y - 2.0F,
                    volume_left + (volume_right - volume_left) * volume_,
                    volume_center_y + 2.0F),
                2.0F,
                2.0F),
            accent_brush_.Get());
        const float volume_x = volume_left + (volume_right - volume_left) * volume_;
        render_target_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(volume_x, volume_center_y), 5.0F, 5.0F),
            text_brush_.Get());

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
        case ControlIcon::pause:
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(
                        center.x - 6.5F,
                        center.y - 7.0F,
                        center.x - 2.0F,
                        center.y + 7.0F),
                    1.0F,
                    1.0F),
                brush);
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(
                        center.x + 2.0F,
                        center.y - 7.0F,
                        center.x + 6.5F,
                        center.y + 7.0F),
                    1.0F,
                    1.0F),
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
        case ControlIcon::settings:
            render_target_->DrawEllipse(
                D2D1::Ellipse(center, 6.0F, 6.0F),
                brush,
                2.0F);
            render_target_->DrawEllipse(
                D2D1::Ellipse(center, 2.0F, 2.0F),
                brush,
                1.5F);
            for (int tooth = 0; tooth < 8; ++tooth) {
                const float angle = static_cast<float>(tooth) * 3.14159265F / 4.0F;
                const float cosine = std::cos(angle);
                const float sine = std::sin(angle);
                render_target_->DrawLine(
                    D2D1::Point2F(
                        center.x + cosine * 7.5F,
                        center.y + sine * 7.5F),
                    D2D1::Point2F(
                        center.x + cosine * 10.0F,
                        center.y + sine * 10.0F),
                    brush,
                    2.0F);
            }
            break;
        }
    }

    void draw_metadata_source_pill(
        const float left,
        const float top,
        const float maximum_width,
        const std::wstring& source)
    {
        const float width = std::min(
            maximum_width,
            std::max(76.0F, 28.0F + static_cast<float>(source.size()) * 7.0F));
        const auto capsule = D2D1::RoundedRect(
            D2D1::RectF(left, top, left + width, top + 24.0F),
            12.0F,
            12.0F);

        accent_brush_->SetOpacity(0.10F);
        render_target_->DrawRoundedRectangle(capsule, accent_brush_.Get(), 6.0F);
        accent_brush_->SetOpacity(0.14F);
        render_target_->FillRoundedRectangle(capsule, accent_brush_.Get());
        accent_brush_->SetOpacity(0.72F);
        render_target_->DrawRoundedRectangle(capsule, accent_brush_.Get(), 1.0F);
        accent_brush_->SetOpacity(1.0F);

        draw_text(
            source,
            caption_format_.Get(),
            capsule.rect,
            text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);
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
        const auto actions = playback_recovery_.request_disc_refresh();
        if (!actions.refresh_disc) {
            return;
        }
        begin_disc_refresh(true);
    }

    void begin_disc_refresh(const bool stop_first)
    {
        if (stop_first) {
            persist_playback_position();
            stop_playback();
        }
        if (disc_loading_ || disc_worker_.joinable()) {
            return;
        }
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
        const std::wstring snapshot_key = snapshot->toc
            ? platform::windows::make_disc_settings_key(*snapshot->toc)
            : std::wstring{};
        const auto recovery_actions = playback_recovery_.complete_disc_refresh(
            snapshot_key,
            snapshot->toc.has_value());
        if (recovery_actions.discard_disc_snapshot) {
            if (recovery_actions.refresh_disc) {
                refresh_disc();
            }
            return;
        }
        ++disc_generation_;
        disc_ = std::move(*snapshot);
        selected_track_ = first_audio_track();
        restore_playback_position();
        scroll_row_ = 0;
        InvalidateRect(window_, nullptr, FALSE);
        sync_system_media(true);
        start_online_metadata_lookup();
        if (recovery_actions.restart_playback) {
            start_playback(playback_track_frame_);
        }
    }

    void handle_device_lifecycle(
        const platform::windows::DeviceLifecycleEvent event)
    {
        using platform::windows::DeviceLifecycleEvent;

        switch (event) {
        case DeviceLifecycleEvent::optical_media_changed: {
            const auto actions = playback_recovery_.media_changed(playback_active_);
            if (actions.stop_playback) {
                persist_playback_position();
                stop_playback();
            }
            if (actions.refresh_disc) {
                begin_disc_refresh(!actions.stop_playback);
            }
            return;
        }
        case DeviceLifecycleEvent::suspending: {
            const auto actions = playback_recovery_.suspend(
                playback_active_,
                playback_paused_,
                current_disc_key_);
            if (actions.stop_playback) {
                persist_playback_position();
                stop_playback(true);
                ui_message_ = L"播放已暂停，等待系统唤醒";
                ui_message_is_error_ = false;
            }
            return;
        }
        case DeviceLifecycleEvent::resumed: {
            const auto actions = playback_recovery_.resume();
            if (actions.refresh_disc) {
                ui_message_ = L"正在重新检查光盘和音频设备";
                ui_message_is_error_ = false;
                begin_disc_refresh(false);
            }
            return;
        }
        case DeviceLifecycleEvent::none:
            return;
        }
    }

    void start_online_metadata_lookup()
    {
        if (!disc_.toc || metadata_worker_.joinable()) {
            return;
        }
        const HWND target_window = window_;
        const disc::Toc toc = *disc_.toc;
        const std::wstring album_title = disc_.album_title;
        const std::wstring album_artist = disc_.album_artist;
        const std::uint64_t disc_generation = disc_generation_;
        metadata_worker_ = std::jthread([
            target_window,
            toc,
            album_title,
            album_artist,
            disc_generation] {
            auto snapshot = std::make_unique<OnlineMetadataSnapshot>();
            snapshot->disc_generation = disc_generation;
            snapshot->metadata = platform::windows::lookup_online_metadata(
                toc,
                album_title,
                album_artist);
            auto* const raw_snapshot = snapshot.release();
            if (PostMessageW(
                    target_window,
                    kMetadataReadyMessage,
                    0,
                    reinterpret_cast<LPARAM>(raw_snapshot)) == FALSE) {
                delete raw_snapshot;
            }
        });
    }

    void receive_online_metadata(std::unique_ptr<OnlineMetadataSnapshot> snapshot)
    {
        if (metadata_worker_.joinable()) {
            metadata_worker_.join();
        }

        if (snapshot->disc_generation != disc_generation_ || disc_loading_) {
            if (!disc_loading_) {
                start_online_metadata_lookup();
            }
            return;
        }

        if (!snapshot->metadata) {
            return;
        }

        const auto& metadata = *snapshot->metadata;
        disc_.metadata_source = disc_.has_cd_text ? L"CD-TEXT" : L"";
        for (const auto& source : metadata.sources) {
            if (!disc_.metadata_source.empty()) {
                disc_.metadata_source += L" · ";
            }
            disc_.metadata_source += source;
        }
        if (disc_.album_title.empty()) {
            disc_.album_title = metadata.album_title;
        }
        if (disc_.album_artist.empty()) {
            disc_.album_artist = metadata.album_artist;
        }
        disc_.cover_art_path = metadata.cover_art_path;
        disc_.release_mbid = metadata.release_mbid;
        disc_.release_group_mbid = metadata.release_group_mbid;
        if (!disc_.cover_art_path.empty()) {
            load_cover_bitmap(disc_.cover_art_path);
        }
        const std::size_t title_count = std::min(
            disc_.tracks.size(),
            metadata.track_titles.size());
        for (std::size_t index = 0; index < title_count; ++index) {
            if (!disc_.tracks[index].has_metadata_title &&
                !metadata.track_titles[index].empty()) {
                disc_.tracks[index].title = metadata.track_titles[index];
                disc_.tracks[index].has_metadata_title = true;
            }
        }
        const std::size_t artist_count = std::min(
            disc_.tracks.size(),
            metadata.track_artists.size());
        for (std::size_t index = 0; index < artist_count; ++index) {
            if (disc_.tracks[index].artist.empty() &&
                !metadata.track_artists[index].empty()) {
                disc_.tracks[index].artist = metadata.track_artists[index];
            }
        }
        const std::size_t identity_count = std::min(
            disc_.tracks.size(),
            metadata.recording_mbids.size());
        for (std::size_t index = 0; index < identity_count; ++index) {
            disc_.tracks[index].recording_mbid = metadata.recording_mbids[index];
        }
        const std::size_t track_id_count = std::min(
            disc_.tracks.size(),
            metadata.track_mbids.size());
        for (std::size_t index = 0; index < track_id_count; ++index) {
            disc_.tracks[index].track_mbid = metadata.track_mbids[index];
        }
        const std::size_t artist_id_count = std::min(
            disc_.tracks.size(),
            metadata.track_artist_mbids.size());
        for (std::size_t index = 0; index < artist_id_count; ++index) {
            disc_.tracks[index].artist_mbids = metadata.track_artist_mbids[index];
        }
        if (listenbrainz_tracker_.active() &&
            selected_track_ < disc_.tracks.size()) {
            listenbrainz_tracker_.update(
                listen_metadata(selected_track_),
                playback_track_frame_,
                unix_time_now());
        }
        sync_system_media(true);
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

        if (active_page_ == AppPage::settings) {
            const bool changed = hovered_track_.has_value() ||
                hovered_control_ != HoveredControl::none;
            hovered_track_.reset();
            hovered_control_ = HoveredControl::none;
            if (changed) {
                InvalidateRect(window_, nullptr, FALSE);
            }
            return;
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
        if (contains(layout_.settings_button, point)) return HoveredControl::settings;
        if (contains(layout_.previous_button, point)) return HoveredControl::previous;
        if (contains(layout_.play_button, point)) return HoveredControl::play;
        if (contains(layout_.next_button, point)) return HoveredControl::next;
        if (contains(layout_.progress_hit, point)) return HoveredControl::progress;
        if (contains(layout_.volume_hit, point)) return HoveredControl::volume;
        return HoveredControl::none;
    }

    void handle_click(const D2D1_POINT_2F point)
    {
        for (const auto& hit : track_hits_) {
            if (contains(hit.rectangle, point)) {
                if (hit.track_index < disc_.tracks.size() &&
                    disc_.tracks[hit.track_index].is_audio) {
                    select_track(hit.track_index, false);
                    if (playback_active_) {
                        resume_playback();
                    } else {
                        start_playback(0);
                    }
                }
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
        case HoveredControl::settings:
            open_settings();
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
        case HoveredControl::volume:
            set_volume_from_point(point.x);
            break;
        case HoveredControl::none:
            break;
        }
    }

    LRESULT handle_key_down(const WPARAM key)
    {
        if (active_page_ == AppPage::settings) {
            if (key == VK_ESCAPE) {
                close_settings();
            } else if (key == VK_RETURN) {
                save_settings();
            }
            return 0;
        }
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
            select_track(first_audio_track());
            return 0;
        case VK_END:
            select_last_audio_track();
            return 0;
        case VK_ESCAPE:
            stop_playback();
            persist_playback_position();
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

    void handle_mouse_wheel(
        const short wheel_delta,
        const int screen_x,
        const int screen_y)
    {
        POINT client_point{screen_x, screen_y};
        if (ScreenToClient(window_, &client_point) == FALSE) {
            return;
        }
        const D2D1_POINT_2F point = D2D1::Point2F(
            pixel_to_dip(client_point.x),
            pixel_to_dip(client_point.y));
        if (contains(layout_.volume_hit, point)) {
            const float steps = static_cast<float>(wheel_delta) /
                static_cast<float>(WHEEL_DELTA);
            set_volume(volume_ + steps * 0.05F);
            persist_user_settings();
            return;
        }
        if (!contains(layout_.track_list, point)) {
            return;
        }

        wheel_delta_remainder_ += wheel_delta;
        const int notches = wheel_delta_remainder_ / WHEEL_DELTA;
        wheel_delta_remainder_ %= WHEEL_DELTA;
        if (notches == 0) {
            return;
        }

        UINT lines_per_notch{3};
        static_cast<void>(SystemParametersInfoW(
            SPI_GETWHEELSCROLLLINES,
            0,
            &lines_per_notch,
            0));
        const auto geometry = track_scrollbar_geometry();
        const int lines = lines_per_notch == WHEEL_PAGESCROLL
            ? static_cast<int>(geometry.visible_rows)
            : static_cast<int>(std::max<UINT>(lines_per_notch, 1));
        scroll_by_rows(-notches * lines);
    }

    void scroll_by_rows(const int rows)
    {
        const auto geometry = track_scrollbar_geometry();
        const auto next = std::clamp<long long>(
            static_cast<long long>(scroll_row_) + rows,
            0,
            static_cast<long long>(geometry.maximum_scroll));
        const auto next_row = static_cast<std::size_t>(next);
        if (next_row == scroll_row_) {
            return;
        }
        scroll_row_ = next_row;
        InvalidateRect(window_, nullptr, FALSE);
    }

    [[nodiscard]] bool handle_scrollbar_press(const D2D1_POINT_2F point)
    {
        const auto geometry = track_scrollbar_geometry();
        if (!geometry.visible || !contains(geometry.track, point)) {
            return false;
        }
        if (contains(geometry.thumb, point)) {
            scrollbar_dragging_ = true;
            scrollbar_drag_offset_ = point.y - geometry.thumb.top;
            SetCapture(window_);
        } else {
            const int page = static_cast<int>(std::max<std::size_t>(
                geometry.visible_rows - 1,
                1));
            scroll_by_rows(point.y < geometry.thumb.top ? -page : page);
        }
        return true;
    }

    void update_scrollbar_drag(const D2D1_POINT_2F point)
    {
        const auto geometry = track_scrollbar_geometry();
        if (!geometry.visible) {
            return;
        }
        const float thumb_height = geometry.thumb.bottom - geometry.thumb.top;
        const float travel = geometry.track.bottom - geometry.track.top - thumb_height;
        if (travel <= 0.0F) {
            return;
        }
        const float thumb_top = std::clamp(
            point.y - scrollbar_drag_offset_,
            geometry.track.top,
            geometry.track.bottom - thumb_height);
        const float ratio = (thumb_top - geometry.track.top) / travel;
        const auto next = static_cast<std::size_t>(std::lround(
            ratio * static_cast<float>(geometry.maximum_scroll)));
        if (next != scroll_row_) {
            scroll_row_ = next;
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void select_track(
        const std::size_t track_index,
        const bool continue_active_playback = true)
    {
        if (track_index >= disc_.tracks.size() ||
            !disc_.tracks[track_index].is_audio) {
            return;
        }
        if (playback_active_ &&
            seek_in_active_session(track_index, 0)) {
            return;
        }
        const bool should_continue = continue_active_playback &&
            playback_active_ && !playback_paused_;
        if (playback_active_) {
            stop_playback();
        }
        selected_track_ = track_index;
        playback_track_frame_ = 0;
        playback_paused_ = false;
        playback_completed_ = false;
        ensure_selected_track_visible();
        sync_system_media(true);
        if (should_continue) {
            start_playback(0);
        } else {
            persist_playback_position();
        }
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
                select_track(static_cast<std::size_t>(index));
                return;
            }
        }
    }

    void select_last_audio_track()
    {
        for (std::size_t index = disc_.tracks.size(); index > 0; --index) {
            if (disc_.tracks[index - 1].is_audio) {
                select_track(index - 1);
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
            if (playback_paused_) {
                resume_playback();
            } else {
                pause_playback();
            }
        } else {
            start_playback(playback_track_frame_);
        }
    }

    void start_playback(
        const core::SampleFrame offset_frames,
        const bool preserve_listen_session = false,
        const bool endpoint_recovery = false)
    {
        if (selected_track_ >= disc_.tracks.size() ||
            !disc_.tracks[selected_track_].is_audio || !disc_.drive) {
            return;
        }
        if (!endpoint_recovery) {
            playback_recovery_.begin_playback_intent();
        }
        playback_seek_pending_ = false;
        const bool resume_session =
            (playback_paused_ || preserve_listen_session) &&
            listenbrainz_tracker_.active();
        stop_playback(resume_session, resume_session);

        platform::windows::CddaPlaybackRequest request;
        request.drive = disc_.drive;
        request.track_number = disc_.tracks[selected_track_].number;
        request.offset_frames = std::clamp<core::SampleFrame>(
            offset_frames,
            0,
            std::max<core::SampleFrame>(
                0,
                disc_.tracks[selected_track_].frame_count - 1));
        request.maximum_frames.reset();
        if (resume_session) {
            listenbrainz_tracker_.seek(request.offset_frames);
        }

        const std::uint64_t generation = playback_generation_;
        const HWND target_window = window_;
        {
            std::scoped_lock lock(playback_result_mutex_);
            playback_result_.reset();
        }
        playback_active_ = true;
        playback_paused_ = false;
        playback_completed_ = false;
        playback_start_track_ = selected_track_;
        playback_start_offset_frames_ = request.offset_frames;
        playback_track_frame_ = request.offset_frames;
        if (!resume_session) {
            listenbrainz_tracker_.begin(
                listen_metadata(selected_track_),
                request.offset_frames,
                unix_time_now());
        }
        ui_message_.clear();
        ui_message_is_error_ = false;
        playback_engine_.request_resume();
        playback_engine_.set_volume(volume_);
        system_media_controls_.set_playback_state(
            platform::windows::SystemMediaPlaybackState::playing);
        sync_system_media(true);
        playback_worker_ = std::jthread([
            this,
            target_window,
            request = std::move(request),
            generation] {
            auto result = playback_engine_.play(request);
            {
                std::scoped_lock lock(playback_result_mutex_);
                playback_result_ = std::move(result);
            }
            static_cast<void>(PostMessageW(
                target_window,
                kPlaybackReadyMessage,
                static_cast<WPARAM>(generation),
                0));
        });
        InvalidateRect(window_, nullptr, FALSE);
    }

    void stop_playback(
        const bool preserve_position = false,
        const bool preserve_listen_session = false)
    {
        if (playback_active_) {
            update_playback_clock();
        }
        if (!preserve_listen_session) {
            const bool had_listenbrainz_session = listenbrainz_tracker_.active();
            listenbrainz_tracker_.end();
            if (had_listenbrainz_session) {
                listenbrainz_reporter_.clear_playing_now();
            }
        }
        ++playback_generation_;
        if (playback_worker_.joinable()) {
            playback_engine_.request_stop();
            playback_worker_.join();
        }
        {
            std::scoped_lock lock(playback_result_mutex_);
            playback_result_.reset();
        }
        playback_active_ = false;
        playback_seek_pending_ = false;
        if (!preserve_position) {
            playback_paused_ = false;
            playback_track_frame_ = 0;
        }
        system_media_controls_.set_playback_state(
            preserve_position
                ? platform::windows::SystemMediaPlaybackState::paused
                : platform::windows::SystemMediaPlaybackState::stopped);
        sync_system_media(true);
        if (window_ != nullptr) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void update_playback_clock()
    {
        if (!playback_active_) {
            return;
        }

        const auto progress = playback_engine_.progress();
        if (progress.frames_rendered >= core::kCdSampleFramesPerSecond) {
            playback_recovery_.playback_became_stable();
        }
        if (playback_seek_pending_) {
            if (progress.applied_seek_sequence < playback_seek_sequence_) {
                return;
            }
            playback_seek_pending_ = false;
        }
        std::size_t track_index = disc_.tracks.size();
        for (std::size_t index = 0; index < disc_.tracks.size(); ++index) {
            if (disc_.tracks[index].number == progress.base_track_number) {
                track_index = index;
                break;
            }
        }
        if (track_index == disc_.tracks.size()) {
            return;
        }
        core::SampleFrame track_frame =
            progress.base_track_offset_frames + progress.frames_rendered;
        std::optional<std::size_t> last_audio_track;
        while (track_index < disc_.tracks.size()) {
            if (!disc_.tracks[track_index].is_audio) {
                break;
            }
            last_audio_track = track_index;
            const core::SampleFrame duration = disc_.tracks[track_index].frame_count;
            if (track_frame < duration) {
                selected_track_ = track_index;
                playback_track_frame_ = track_frame;
                ensure_selected_track_visible();
                update_listenbrainz_tracker(
                    track_index,
                    track_frame,
                    progress.state);
                sync_system_media(false);
                return;
            }
            track_frame -= duration;
            ++track_index;
        }
        if (last_audio_track) {
            selected_track_ = *last_audio_track;
            playback_track_frame_ = disc_.tracks[*last_audio_track].frame_count;
            ensure_selected_track_visible();
            update_listenbrainz_tracker(
                *last_audio_track,
                disc_.tracks[*last_audio_track].frame_count,
                progress.state);
        }
        sync_system_media(false);
    }

    void receive_playback_result(const std::uint64_t generation)
    {
        if (generation != playback_generation_) {
            return;
        }
        if (playback_worker_.joinable()) {
            playback_worker_.join();
        }

        std::optional<platform::windows::CddaPlaybackResult> result;
        {
            std::scoped_lock lock(playback_result_mutex_);
            result = std::move(playback_result_);
            playback_result_.reset();
        }
        if (!result) {
            return;
        }

        update_playback_clock();
        if (platform::windows::is_recoverable_default_endpoint_failure(*result)) {
            const auto recovery_actions =
                playback_recovery_.endpoint_failed(playback_active_);
            if (recovery_actions.restart_playback) {
                const core::SampleFrame restart_offset = playback_track_frame_;
                playback_active_ = false;
                playback_paused_ = false;
                ui_message_ = L"默认音频设备已失效，正在切换并恢复播放";
                ui_message_is_error_ = false;
                start_playback(restart_offset, true, true);
                return;
            }
        }
        const bool media_unavailable =
            platform::windows::is_media_unavailable_failure(*result);
        const bool had_listenbrainz_session = listenbrainz_tracker_.active();
        listenbrainz_tracker_.end();
        if (had_listenbrainz_session) {
            listenbrainz_reporter_.clear_playing_now();
        }
        if (result->succeeded()) {
            playback_completed_ = true;
            if (!current_disc_key_.empty()) {
                user_settings_.playback_positions.erase(current_disc_key_);
                persist_user_settings();
            }
        } else {
            playback_completed_ = false;
            persist_playback_position();
        }
        playback_active_ = false;
        playback_paused_ = false;
        playback_seek_pending_ = false;
        system_media_controls_.set_playback_state(
            platform::windows::SystemMediaPlaybackState::stopped);
        sync_system_media(true);
        if (!result->succeeded() &&
            result->error != platform::windows::CddaPlaybackError::cancelled) {
            set_playback_error(*result);
        }
        if (media_unavailable) {
            const auto recovery_actions = playback_recovery_.media_changed(false);
            if (recovery_actions.refresh_disc) {
                begin_disc_refresh(false);
            }
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void set_playback_error(
        const platform::windows::CddaPlaybackResult& result)
    {
        using platform::windows::CddaPlaybackError;

        switch (result.error) {
        case CddaPlaybackError::no_ready_audio_cd:
            ui_message_ = L"当前光盘已不可用";
            break;
        case CddaPlaybackError::source_open_failed:
        case CddaPlaybackError::read_failed:
            ui_message_ = L"读取光盘失败：" +
                platform::windows::format_system_error(result.system_error);
            break;
        case CddaPlaybackError::endpoint_underrun:
            ui_message_ = L"光驱供给不足，播放已停止";
            break;
        case CddaPlaybackError::output_open_failed:
        case CddaPlaybackError::output_failed:
            if (platform::windows::is_recoverable_default_endpoint_failure(result)) {
                ui_message_ = std::format(
                    L"默认音频设备恢复失败：0x{:08X}，请检查输出设备",
                    static_cast<std::uint32_t>(result.audio_status));
            } else {
                ui_message_ = std::format(
                    L"音频设备错误：0x{:08X}",
                    static_cast<std::uint32_t>(result.audio_status));
            }
            break;
        default:
            ui_message_ = L"播放未能完成";
            break;
        }
        ui_message_is_error_ = true;
    }

    [[nodiscard]] float playback_progress() const
    {
        if (selected_track_ >= disc_.tracks.size()) {
            return 0.0F;
        }
        const core::SampleFrame duration =
            disc_.tracks[selected_track_].frame_count;
        return duration <= 0
            ? 0.0F
            : clamp01(static_cast<float>(
                static_cast<double>(playback_track_frame_) /
                static_cast<double>(duration)));
    }

    [[nodiscard]] std::wstring listenbrainz_settings_detail() const
    {
        const auto status = listenbrainz_reporter_.status();
        std::wstring detail;
        const auto append = [&detail](const std::wstring& value) {
            if (value.empty()) {
                return;
            }
            if (!detail.empty()) {
                detail += L" · ";
            }
            detail += value;
        };
        if (!status.user_name.empty()) {
            append(L"账户 " + status.user_name);
        }
        if (status.pending_listens != 0) {
            append(std::format(L"{} 条待同步", status.pending_listens));
        }
        if (status.failed_listens != 0) {
            append(std::format(L"{} 条需重试", status.failed_listens));
        }
        if (status.state == platform::windows::ListenBrainzState::retry_wait &&
            status.retry_after_seconds != 0) {
            append(std::format(L"{} 秒后重试", status.retry_after_seconds));
        }
        if (detail.empty()) {
            append(platform::windows::to_string(status.state));
        }
        return detail;
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
        const core::SampleFrame total_frames =
            disc_.tracks[selected_track_].frame_count;
        const core::SampleFrame offset = static_cast<core::SampleFrame>(
            std::floor(static_cast<double>(total_frames) * ratio));
        const core::SampleFrame target = std::min(
            offset,
            std::max<core::SampleFrame>(0, total_frames - 1));
        if (!seek_in_active_session(selected_track_, target)) {
            start_playback(target, true);
        }
    }

    [[nodiscard]] bool seek_in_active_session(
        const std::size_t track_index,
        const core::SampleFrame offset_frames)
    {
        if (!playback_active_ || track_index >= disc_.tracks.size() ||
            !disc_.tracks[track_index].is_audio) {
            return false;
        }
        const auto& track = disc_.tracks[track_index];
        const core::SampleFrame target = std::clamp<core::SampleFrame>(
            offset_frames,
            0,
            std::max<core::SampleFrame>(0, track.frame_count - 1));
        const auto seek_result = playback_engine_.request_seek(
            track.number,
            target);
        if (seek_result.result !=
            platform::windows::CddaSeekRequestResult::queued) {
            return false;
        }

        const bool track_changed = selected_track_ != track_index;
        selected_track_ = track_index;
        playback_track_frame_ = target;
        playback_start_track_ = track_index;
        playback_start_offset_frames_ = target;
        playback_completed_ = false;
        playback_seek_pending_ = true;
        playback_seek_sequence_ = seek_result.sequence;
        ensure_selected_track_visible();

        if (listenbrainz_tracker_.active()) {
            if (track_changed) {
                if (playback_paused_) {
                    listenbrainz_tracker_.end();
                    listenbrainz_reporter_.clear_playing_now();
                } else {
                    listenbrainz_tracker_.begin(
                        listen_metadata(track_index),
                        target,
                        unix_time_now());
                }
            } else {
                listenbrainz_tracker_.seek(target);
            }
        }
        sync_system_media(true);
        persist_playback_position();
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    void set_volume(const float volume)
    {
        volume_ = std::clamp(volume, 0.0F, 1.0F);
        user_settings_.volume = volume_;
        playback_engine_.set_volume(volume_);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void set_volume_from_point(const float x)
    {
        const float left = layout_.volume_hit.left + 4.0F;
        const float right = layout_.volume_hit.right - 4.0F;
        if (right <= left) {
            return;
        }
        set_volume((x - left) / (right - left));
    }

    void pause_playback()
    {
        if (!playback_active_ || playback_paused_) {
            return;
        }
        playback_engine_.request_pause();
        playback_paused_ = true;
        system_media_controls_.set_playback_state(
            platform::windows::SystemMediaPlaybackState::paused);
        sync_system_media(true);
        persist_playback_position();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void resume_playback()
    {
        if (!playback_active_ || !playback_paused_) {
            return;
        }
        playback_engine_.request_resume();
        playback_paused_ = false;
        system_media_controls_.set_playback_state(
            platform::windows::SystemMediaPlaybackState::playing);
        sync_system_media(true);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void handle_system_media_request(
        const platform::windows::SystemMediaRequest request)
    {
        using platform::windows::SystemMediaCommand;

        switch (request.command) {
        case SystemMediaCommand::play:
            if (playback_active_) {
                resume_playback();
            } else {
                start_playback(playback_track_frame_);
            }
            break;
        case SystemMediaCommand::pause:
            pause_playback();
            break;
        case SystemMediaCommand::stop:
            stop_playback();
            persist_playback_position();
            break;
        case SystemMediaCommand::previous:
            select_relative_track(-1);
            break;
        case SystemMediaCommand::next:
            select_relative_track(1);
            break;
        case SystemMediaCommand::seek:
            if (!seek_in_active_session(
                    selected_track_,
                    static_cast<core::SampleFrame>(
                        std::min<std::uint64_t>(
                            request.position_milliseconds,
                            86'400'000U) *
                        static_cast<std::uint64_t>(
                            core::kCdSampleFramesPerSecond) /
                        1'000U))) {
                start_playback(static_cast<core::SampleFrame>(
                    std::min<std::uint64_t>(
                        request.position_milliseconds,
                        86'400'000U) *
                    static_cast<std::uint64_t>(core::kCdSampleFramesPerSecond) /
                    1'000U), true);
            }
            break;
        }
    }

    [[nodiscard]] listenbrainz::TrackMetadata listen_metadata(
        const std::size_t track_index) const
    {
        if (track_index >= disc_.tracks.size()) {
            return {};
        }
        const auto& track = disc_.tracks[track_index];
        return {
            track.number,
            track.title,
            track.artist.empty() ? disc_.album_artist : track.artist,
            disc_.album_title,
            track.frame_count,
            track.recording_mbid,
            disc_.release_mbid,
            disc_.release_group_mbid,
            track.track_mbid,
            track.artist_mbids,
        };
    }

    void update_listenbrainz_tracker(
        const std::size_t track_index,
        const core::SampleFrame position_frames,
        const audio::PlaybackState playback_state)
    {
        if (playback_state != audio::PlaybackState::playing &&
            playback_state != audio::PlaybackState::draining) {
            return;
        }
        auto metadata = listen_metadata(track_index);
        if (!listenbrainz_tracker_.active()) {
            const core::SampleFrame initial_position =
                track_index == playback_start_track_
                ? playback_start_offset_frames_
                : 0;
            listenbrainz_tracker_.begin(
                metadata,
                initial_position,
                unix_time_now());
            listenbrainz_tracker_.update(
                std::move(metadata),
                position_frames,
                unix_time_now());
        } else {
            listenbrainz_tracker_.update(
                std::move(metadata),
                position_frames,
                unix_time_now());
        }
    }

    void sync_system_media(const bool force)
    {
        if (!system_media_controls_.available()) {
            return;
        }
        if (selected_track_ >= disc_.tracks.size() ||
            !disc_.tracks[selected_track_].is_audio) {
            system_media_controls_.clear();
            system_media_track_ = std::numeric_limits<std::size_t>::max();
            return;
        }

        const auto& track = disc_.tracks[selected_track_];
        if (force || system_media_track_ != selected_track_) {
            system_media_controls_.update_metadata(
                track.title,
                track.artist.empty() ? disc_.album_artist : track.artist,
                disc_.album_title);
            system_media_track_ = selected_track_;
        }

        const ULONGLONG now = GetTickCount64();
        if (force || now - last_system_timeline_update_ >= 5'000U) {
            system_media_controls_.update_timeline(
                static_cast<std::uint64_t>(
                    std::max<core::SampleFrame>(0, playback_track_frame_)) *
                    1'000U /
                    static_cast<std::uint64_t>(core::kCdSampleFramesPerSecond),
                static_cast<std::uint64_t>(
                    track.frame_count * 1'000 /
                    core::kCdSampleFramesPerSecond));
            last_system_timeline_update_ = now;
        }
    }

    void restore_playback_position()
    {
        current_disc_key_.clear();
        playback_track_frame_ = 0;
        playback_completed_ = false;
        if (!disc_.toc) {
            return;
        }
        current_disc_key_ = platform::windows::make_disc_settings_key(*disc_.toc);
        const auto saved = user_settings_.playback_positions.find(current_disc_key_);
        if (saved == user_settings_.playback_positions.end()) {
            return;
        }
        for (std::size_t index = 0; index < disc_.tracks.size(); ++index) {
            const auto& track = disc_.tracks[index];
            if (!track.is_audio || track.number != saved->second.track_number ||
                track.frame_count <= 0) {
                continue;
            }
            selected_track_ = index;
            const core::SampleFrame offset = std::clamp<core::SampleFrame>(
                saved->second.offset_frames,
                0,
                track.frame_count - 1);
            playback_track_frame_ = offset;
            last_persisted_track_number_ = track.number;
            last_persisted_frame_ = offset;
            return;
        }
    }

    void persist_playback_position()
    {
        if (playback_completed_) {
            if (!current_disc_key_.empty()) {
                user_settings_.playback_positions.erase(current_disc_key_);
                persist_user_settings();
            }
            return;
        }
        if (playback_active_) {
            update_playback_clock();
        }
        if (current_disc_key_.empty() || selected_track_ >= disc_.tracks.size()) {
            return;
        }
        const auto& track = disc_.tracks[selected_track_];
        if (!track.is_audio || track.frame_count <= 0) {
            return;
        }
        const core::SampleFrame offset = std::clamp<core::SampleFrame>(
            playback_track_frame_,
            0,
            track.frame_count - 1);
        user_settings_.playback_positions[current_disc_key_] = {
            track.number,
            offset,
        };
        last_persisted_track_number_ = track.number;
        last_persisted_frame_ = offset;
        last_position_save_tick_ = GetTickCount64();
        persist_user_settings();
    }

    void maybe_persist_playback_position()
    {
        if (!playback_active_ || selected_track_ >= disc_.tracks.size()) {
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (now - last_position_save_tick_ < 5'000U) {
            return;
        }
        const auto& track = disc_.tracks[selected_track_];
        const core::SampleFrame frame = playback_track_frame_;
        if (track.number == last_persisted_track_number_ &&
            std::abs(frame - last_persisted_frame_) <
                core::kCdSampleFramesPerSecond) {
            last_position_save_tick_ = now;
            return;
        }
        persist_playback_position();
    }

    void persist_user_settings()
    {
        user_settings_.volume = volume_;
        static_cast<void>(platform::windows::save_user_settings(user_settings_));
    }

    static LRESULT CALLBACK settings_edit_window_proc(
        const HWND edit,
        const UINT message,
        const WPARAM wparam,
        const LPARAM lparam)
    {
        auto* self = reinterpret_cast<MainWindow*>(
            GetWindowLongPtrW(edit, GWLP_USERDATA));
        if (self != nullptr && message == WM_KEYDOWN) {
            if (wparam == VK_RETURN) {
                PostMessageW(self->window_, kSettingsSaveMessage, 0, 0);
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                PostMessageW(self->window_, kSettingsCloseMessage, 0, 0);
                return 0;
            }
        }
        if (self != nullptr && message == WM_GETDLGCODE) {
            return DLGC_WANTALLKEYS;
        }
        return self != nullptr && self->settings_edit_original_proc_ != nullptr
            ? CallWindowProcW(
                  self->settings_edit_original_proc_,
                  edit,
                  message,
                  wparam,
                  lparam)
            : DefWindowProcW(edit, message, wparam, lparam);
    }

    void open_settings()
    {
        if (active_page_ == AppPage::settings) {
            if (settings_token_edit_ != nullptr) {
                SetFocus(settings_token_edit_);
            }
            return;
        }

        active_page_ = AppPage::settings;
        settings_save_failed_ = false;
        settings_saved_ = false;
        settings_input_required_ = false;
        if (settings_font_ == nullptr) {
            settings_font_ = CreateFontW(
                -MulDiv(14, static_cast<int>(GetDpiForWindow(window_)), 72),
                0,
                0,
                0,
                FW_NORMAL,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI");
        }
        if (settings_edit_brush_ == nullptr) {
            settings_edit_brush_ = CreateSolidBrush(RGB(29, 33, 42));
        }
        settings_token_edit_ = CreateWindowExW(
            0,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
            0,
            0,
            1,
            1,
            window_,
            reinterpret_cast<HMENU>(
                static_cast<std::intptr_t>(kSettingsTokenEditId)),
            instance_,
            nullptr);
        if (settings_token_edit_ != nullptr) {
            SendMessageW(
                settings_token_edit_,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(settings_font_),
                TRUE);
            SendMessageW(settings_token_edit_, EM_SETLIMITTEXT, 512, 0);
            SendMessageW(
                settings_token_edit_,
                EM_SETCUEBANNER,
                TRUE,
                reinterpret_cast<LPARAM>(L"粘贴 ListenBrainz User Token"));
            SetWindowLongPtrW(
                settings_token_edit_,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(this));
            settings_edit_original_proc_ = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(
                    settings_token_edit_,
                    GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(settings_edit_window_proc)));
            update_settings_edit_bounds();
            SetFocus(settings_token_edit_);
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void close_settings()
    {
        if (settings_token_edit_ != nullptr) {
            DestroyWindow(settings_token_edit_);
            settings_token_edit_ = nullptr;
            settings_edit_original_proc_ = nullptr;
        }
        if (active_page_ == AppPage::settings) {
            active_page_ = AppPage::player;
            settings_save_failed_ = false;
            settings_saved_ = false;
            settings_input_required_ = false;
            if (window_ != nullptr) {
                SetFocus(window_);
                InvalidateRect(window_, nullptr, FALSE);
            }
        }
    }

    void save_settings()
    {
        if (active_page_ != AppPage::settings || settings_token_edit_ == nullptr) {
            return;
        }
        const int length = GetWindowTextLengthW(settings_token_edit_);
        std::wstring token(static_cast<std::size_t>(length) + 1U, L'\0');
        const int written = GetWindowTextW(
            settings_token_edit_,
            token.data(),
            length + 1);
        token.resize(static_cast<std::size_t>(std::max(written, 0)));
        const auto first = token.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) {
            token.clear();
        } else {
            const auto last = token.find_last_not_of(L" \t\r\n");
            token = token.substr(first, last - first + 1U);
        }

        if (!platform::windows::is_listenbrainz_token_format_valid(token)) {
            settings_input_required_ = true;
            settings_save_failed_ = false;
            settings_saved_ = false;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }

        if (!platform::windows::save_listenbrainz_token(token)) {
            settings_save_failed_ = true;
            settings_input_required_ = false;
            settings_saved_ = false;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        listenbrainz_reporter_.reload_token();
        SetWindowTextW(settings_token_edit_, L"");
        settings_save_failed_ = false;
        settings_input_required_ = false;
        settings_saved_ = true;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void clear_settings_token()
    {
        if (!platform::windows::save_listenbrainz_token(L"")) {
            settings_save_failed_ = true;
            settings_input_required_ = false;
            settings_saved_ = false;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        listenbrainz_reporter_.reload_token();
        if (settings_token_edit_ != nullptr) {
            SetWindowTextW(settings_token_edit_, L"");
        }
        settings_save_failed_ = false;
        settings_input_required_ = false;
        settings_saved_ = true;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void handle_settings_click(const D2D1_POINT_2F point)
    {
        if (contains(layout_.settings_save, point)) {
            save_settings();
        } else if (contains(layout_.settings_clear, point)) {
            clear_settings_token();
        } else if (contains(layout_.settings_listenbrainz_toggle, point)) {
            user_settings_.listenbrainz_reporting_enabled =
                !user_settings_.listenbrainz_reporting_enabled;
            listenbrainz_reporter_.set_reporting_enabled(
                user_settings_.listenbrainz_reporting_enabled);
            persist_user_settings();
            InvalidateRect(window_, nullptr, FALSE);
        } else if (contains(layout_.settings_back, point) ||
                   contains(layout_.settings_button, point)) {
            close_settings();
        }
    }

    void update_settings_edit_bounds()
    {
        if (settings_token_edit_ == nullptr) {
            return;
        }
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0F;
        const auto to_pixel = [scale](const float value) {
            return static_cast<int>(std::lround(value * scale));
        };
        SetWindowPos(
            settings_token_edit_,
            HWND_TOP,
            to_pixel(layout_.settings_edit.left + 13.0F),
            to_pixel(layout_.settings_edit.top + 8.0F),
            to_pixel(layout_.settings_edit.right - layout_.settings_edit.left - 26.0F),
            to_pixel(layout_.settings_edit.bottom - layout_.settings_edit.top - 16.0F),
            SWP_NOACTIVATE);
    }

    void draw_toggle(const D2D1_RECT_F rectangle, const bool enabled)
    {
        const auto pill = D2D1::RoundedRect(rectangle, 14.0F, 14.0F);
        render_target_->FillRoundedRectangle(
            pill,
            enabled ? accent_brush_.Get() : elevated_brush_.Get());
        render_target_->DrawRoundedRectangle(pill, border_brush_.Get(), 1.0F);
        const float radius = 10.0F;
        const float center_x = enabled
            ? rectangle.right - radius - 4.0F
            : rectangle.left + radius + 4.0F;
        render_target_->FillEllipse(
            D2D1::Ellipse(
                D2D1::Point2F(
                    center_x,
                    (rectangle.top + rectangle.bottom) * 0.5F),
                radius,
                radius),
            enabled ? accent_text_brush_.Get() : secondary_brush_.Get());
    }

    void draw_settings_page()
    {
        draw_text(
            L"设置",
            heading_format_.Get(),
            D2D1::RectF(
                layout_.settings_page.left,
                84.0F,
                layout_.settings_back.left - 20.0F,
                116.0F),
            text_brush_.Get());
        draw_text(
            L"管理在线服务、播放和 Windows 集成",
            small_format_.Get(),
            D2D1::RectF(
                layout_.settings_page.left,
                116.0F,
                layout_.settings_back.left - 20.0F,
                140.0F),
            secondary_brush_.Get());

        const auto back = D2D1::RoundedRect(layout_.settings_back, 10.0F, 10.0F);
        render_target_->FillRoundedRectangle(back, surface_brush_.Get());
        render_target_->DrawRoundedRectangle(back, border_brush_.Get(), 1.0F);
        draw_text(
            L"←  返回播放器",
            button_format_.Get(),
            layout_.settings_back,
            text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);

        const auto listenbrainz_card = D2D1::RoundedRect(
            layout_.settings_listenbrainz_card,
            16.0F,
            16.0F);
        render_target_->FillRoundedRectangle(listenbrainz_card, surface_brush_.Get());
        render_target_->DrawRoundedRectangle(listenbrainz_card, border_brush_.Get(), 1.0F);
        draw_text(
            L"ListenBrainz",
            heading_format_.Get(),
            D2D1::RectF(
                listenbrainz_card.rect.left + 24.0F,
                listenbrainz_card.rect.top + 20.0F,
                listenbrainz_card.rect.right - 200.0F,
                listenbrainz_card.rect.top + 52.0F),
            text_brush_.Get());
        draw_text(
            L"将正在播放状态和达到阈值的播放记录同步到个人账户",
            small_format_.Get(),
            D2D1::RectF(
                listenbrainz_card.rect.left + 24.0F,
                listenbrainz_card.rect.top + 54.0F,
                listenbrainz_card.rect.right - 24.0F,
                listenbrainz_card.rect.top + 82.0F),
            secondary_brush_.Get());
        const auto listenbrainz_status = listenbrainz_reporter_.status();
        draw_text(
            platform::windows::to_string(listenbrainz_status.state),
            small_format_.Get(),
            D2D1::RectF(
                listenbrainz_card.rect.right - 180.0F,
                listenbrainz_card.rect.top + 22.0F,
                listenbrainz_card.rect.right - 92.0F,
                listenbrainz_card.rect.top + 50.0F),
            listenbrainz_status.state == platform::windows::ListenBrainzState::ready
                ? success_brush_.Get()
                : (listenbrainz_status.state ==
                           platform::windows::ListenBrainzState::unauthorized ||
                       listenbrainz_status.state ==
                           platform::windows::ListenBrainzState::error
                    ? error_brush_.Get()
                    : muted_brush_.Get()),
            DWRITE_TEXT_ALIGNMENT_TRAILING);
        draw_toggle(
            layout_.settings_listenbrainz_toggle,
            user_settings_.listenbrainz_reporting_enabled);
        draw_text(
            L"USER TOKEN",
            caption_format_.Get(),
            D2D1::RectF(
                layout_.settings_edit.left,
                layout_.settings_edit.top - 24.0F,
                layout_.settings_edit.right,
                layout_.settings_edit.top - 2.0F),
            muted_brush_.Get());

        const auto edit = D2D1::RoundedRect(layout_.settings_edit, 10.0F, 10.0F);
        render_target_->FillRoundedRectangle(edit, elevated_brush_.Get());
        render_target_->DrawRoundedRectangle(
            edit,
            settings_save_failed_ || settings_input_required_
                ? error_brush_.Get()
                : border_brush_.Get(),
            settings_save_failed_ || settings_input_required_ ? 2.0F : 1.0F);
        if (settings_save_failed_ || settings_input_required_ || settings_saved_) {
            draw_text(
                settings_saved_
                    ? L"设置已更新"
                    : (settings_input_required_
                        ? L"请输入完整的 User Token（不能包含空格或换行）；移除凭据请使用“清除”"
                        : L"保存失败，请检查 Windows 凭据管理器权限"),
                caption_format_.Get(),
                D2D1::RectF(
                    layout_.settings_edit.left,
                    layout_.settings_edit.bottom + 6.0F,
                    layout_.settings_listenbrainz_card.right - 24.0F,
                    layout_.settings_edit.bottom + 28.0F),
                settings_saved_ ? success_brush_.Get() : error_brush_.Get());
        } else {
            draw_text(
                listenbrainz_settings_detail(),
                caption_format_.Get(),
                D2D1::RectF(
                    layout_.settings_edit.left,
                    layout_.settings_edit.bottom + 6.0F,
                    layout_.settings_listenbrainz_card.right - 24.0F,
                    layout_.settings_edit.bottom + 28.0F),
                listenbrainz_status.failed_listens != 0 ||
                        listenbrainz_status.state ==
                            platform::windows::ListenBrainzState::unauthorized
                    ? error_brush_.Get()
                    : secondary_brush_.Get());
        }

        const auto clear = D2D1::RoundedRect(layout_.settings_clear, 10.0F, 10.0F);
        render_target_->FillRoundedRectangle(clear, elevated_brush_.Get());
        render_target_->DrawRoundedRectangle(clear, border_brush_.Get(), 1.0F);
        draw_text(
            L"清除",
            button_format_.Get(),
            layout_.settings_clear,
            text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);

        const auto save = D2D1::RoundedRect(layout_.settings_save, 10.0F, 10.0F);
        render_target_->FillRoundedRectangle(save, accent_brush_.Get());
        draw_text(
            L"保存",
            button_format_.Get(),
            layout_.settings_save,
            accent_text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);

    }

    void eject_disc()
    {
        if (!disc_.drive) {
            return;
        }
        persist_playback_position();
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
            sync_system_media(true);
        }
        InvalidateRect(window_, nullptr, FALSE);
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
    ComPtr<IWICImagingFactory> imaging_factory_;
    ComPtr<ID2D1HwndRenderTarget> render_target_;
    ComPtr<ID2D1Bitmap> cover_bitmap_;
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
    ComPtr<IDWriteTextFormat> button_format_;
    ComPtr<IDWriteTextFormat> track_format_;
    ComPtr<IDWriteTextFormat> track_duration_format_;
    ComPtr<IDWriteTextFormat> small_format_;
    ComPtr<IDWriteTextFormat> caption_format_;
    ComPtr<IDWriteTextFormat> icon_format_;
    DiscSnapshot disc_;
    std::jthread disc_worker_;
    std::jthread metadata_worker_;
    std::uint64_t disc_generation_{};
    bool disc_loading_{};
    Layout layout_{};
    std::vector<TrackHit> track_hits_;
    std::optional<std::size_t> hovered_track_;
    HoveredControl hovered_control_{HoveredControl::none};
    bool tracking_mouse_{};
    std::size_t selected_track_{};
    std::size_t scroll_row_{};
    int wheel_delta_remainder_{};
    bool scrollbar_dragging_{};
    float scrollbar_drag_offset_{};
    bool volume_dragging_{};
    platform::windows::UserSettings user_settings_;
    float volume_{1.0F};
    std::wstring current_disc_key_;
    unsigned int last_persisted_track_number_{};
    core::SampleFrame last_persisted_frame_{-1};
    ULONGLONG last_position_save_tick_{};
    AppPage active_page_{AppPage::player};
    bool settings_save_failed_{};
    bool settings_saved_{};
    bool settings_input_required_{};
    HWND settings_token_edit_{};
    WNDPROC settings_edit_original_proc_{};
    HFONT settings_font_{};
    HBRUSH settings_edit_brush_{};
    platform::windows::CddaPlaybackEngine playback_engine_;
    audio::PlaybackRecoveryCoordinator playback_recovery_;
    platform::windows::ListenBrainzReporter listenbrainz_reporter_;
    listenbrainz::PlaybackTracker listenbrainz_tracker_;
    platform::windows::SystemMediaControls system_media_controls_;
    std::size_t system_media_track_{std::numeric_limits<std::size_t>::max()};
    ULONGLONG last_system_timeline_update_{};
    std::jthread playback_worker_;
    std::mutex playback_result_mutex_;
    std::optional<platform::windows::CddaPlaybackResult> playback_result_;
    std::uint64_t playback_generation_{};
    bool playback_active_{};
    bool playback_paused_{};
    bool playback_completed_{};
    std::size_t playback_start_track_{};
    core::SampleFrame playback_start_offset_frames_{};
    core::SampleFrame playback_track_frame_{};
    bool playback_seek_pending_{};
    std::uint64_t playback_seek_sequence_{};
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
