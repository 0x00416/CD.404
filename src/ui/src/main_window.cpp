#include <windows.h>
#include <windowsx.h>
#include <uxtheme.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <oleacc.h>
#include <commctrl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cd404/audio/playback_recovery.hpp>
#include <cd404/core/cd_time.hpp>
#include <cd404/listenbrainz/playback_tracker.hpp>
#include <cd404/platform/windows/cdda_playback_engine.hpp>
#include <cd404/platform/windows/device_lifecycle.hpp>
#include <cd404/platform/windows/diagnostics.hpp>
#include <cd404/platform/windows/gnudb_client.hpp>
#include <cd404/platform/windows/listenbrainz_reporter.hpp>
#include <cd404/platform/windows/online_metadata.hpp>
#include <cd404/platform/windows/optical_drive.hpp>
#include <cd404/platform/windows/system_media_controls.hpp>
#include <cd404/platform/windows/user_settings.hpp>
#include <cd404/ui/main_window.hpp>
#include <cd404/ui/metadata_source_model.hpp>
#include <cd404/ui/playback_presenter.hpp>
#include <cd404/ui/settings_model.hpp>
#include <cd404/ui/theme.hpp>

#include "disc_snapshot.hpp"
#include "ui_drawing.hpp"
#include "ui_layout.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
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
constexpr UINT kMetadataEditSaveMessage = WM_APP + 7;
constexpr UINT kMetadataEditCloseMessage = WM_APP + 8;
constexpr UINT kCddbSubmissionReadyMessage = WM_APP + 9;
constexpr int kSettingsTokenEditId = 1'001;
constexpr int kSettingsCddbServerEditId = 1'002;
constexpr int kSettingsCddbEmailEditId = 1'003;
constexpr int kMetadataAlbumTitleEditId = 1'101;
constexpr int kMetadataAlbumArtistEditId = 1'102;
constexpr int kMetadataCategoryEditId = 1'103;
constexpr int kMetadataYearEditId = 1'104;
constexpr int kMetadataTrackTitleEditId = 1'105;
constexpr int kMetadataTrackArtistEditId = 1'106;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kAnimationIntervalMs = 50;
constexpr UINT_PTR kSettingsScrollSettleTimer = 2;
constexpr UINT kSettingsScrollSettleDelayMs = 150;
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmSystemBackdropType = 38;

constexpr float kDesignWidth = 1'100.0F;
constexpr float kDesignHeight = 760.0F;
constexpr float kMinimumWidth = 840.0F;
constexpr float kMinimumHeight = 600.0F;
constexpr float kSettingsContentTop = 140.0F;
constexpr float kSettingsContentBottom = 736.0F;

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

[[nodiscard]] COLORREF colorref(const std::uint32_t rgb) noexcept
{
    return RGB(
        static_cast<BYTE>((rgb >> 16U) & 0xffU),
        static_cast<BYTE>((rgb >> 8U) & 0xffU),
        static_cast<BYTE>(rgb & 0xffU));
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

using detail::DiscSnapshot;
using detail::OnlineMetadataSnapshot;
using detail::UiTrack;
using detail::load_disc_snapshot;
#if 0 // 合并期间保留的旧内联快照实现；待 CDDB 编辑器拆分时删除。
[[nodiscard]] std::wstring to_wstring(const std::u16string_view text)
{
    std::wstring result;
    result.reserve(text.size());
    std::ranges::transform(text, std::back_inserter(result), [](const char16_t character) {
        return static_cast<wchar_t>(character);
    });
    return result;
}

[[nodiscard]] std::string to_utf8(const std::wstring_view text)
{
    if (text.empty() ||
        text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    return WideCharToMultiByte(
               CP_UTF8,
               WC_ERR_INVALID_CHARS,
               text.data(),
               static_cast<int>(text.size()),
               result.data(),
               required,
               nullptr,
               nullptr) == required
        ? result
        : std::string{};
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
    std::wstring cddb_category{L"misc"};
    std::wstring cddb_year;
    unsigned int cddb_revision{};
    std::filesystem::path cover_art_path;
    std::wstring status;
    bool has_cd_text{};
    bool has_user_metadata{};
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
#endif

[[nodiscard]] std::string to_utf8(const std::wstring_view text)
{
    if (text.empty() ||
        text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    return WideCharToMultiByte(
               CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
               result.data(), required, nullptr, nullptr) == required
        ? result
        : std::string{};
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

using detail::Layout;
#if 0 // 合并期间保留的旧内联布局定义；有效布局位于 ui_layout.*。
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
    D2D1_RECT_F settings_cddb_card{};
    D2D1_RECT_F settings_cddb_server_edit{};
    D2D1_RECT_F settings_cddb_email_edit{};
    D2D1_RECT_F settings_cddb_save{};
    D2D1_RECT_F settings_cddb_toggle{};
    D2D1_RECT_F metadata_button{};
    D2D1_RECT_F metadata_page{};
    D2D1_RECT_F metadata_back{};
    D2D1_RECT_F metadata_album_title_edit{};
    D2D1_RECT_F metadata_album_artist_edit{};
    D2D1_RECT_F metadata_category_edit{};
    D2D1_RECT_F metadata_year_edit{};
    D2D1_RECT_F metadata_track_list{};
    D2D1_RECT_F metadata_track_title_edit{};
    D2D1_RECT_F metadata_track_artist_edit{};
    D2D1_RECT_F metadata_save{};
    D2D1_RECT_F metadata_test_submit{};
    D2D1_RECT_F metadata_submit{};
};

#endif

struct CddbSubmissionSnapshot final {
    platform::windows::CddbSubmissionResult result;
    platform::windows::CddbSubmissionMode mode{};
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
        theme_ = query_system_theme();
        diagnostics_.record(L"app", L"startup");
        listenbrainz_reporter_.set_reporting_enabled(
            user_settings_.listenbrainz_reporting_enabled);
    }

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    ~MainWindow()
    {
        close_metadata_edit();
        close_metadata_editor(false);
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
        INITCOMMONCONTROLSEX common_controls{};
        common_controls.dwSize = sizeof(common_controls);
        common_controls.dwICC = ICC_STANDARD_CLASSES;
        if (InitCommonControlsEx(&common_controls) == FALSE) {
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
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            FALSE,
            0,
            dpi));

        window_ = CreateWindowExW(
            0,
            kWindowClassName,
            kWindowTitle,
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
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
            update_accessible_name();
            return 0;
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_SETTINGCHANGE:
            refresh_theme();
            return 0;
        case WM_GETOBJECT:
            if (static_cast<long>(lparam) == OBJID_CLIENT) {
                IAccessible* accessible{};
                if (SUCCEEDED(CreateStdAccessibleObject(
                        window_,
                        OBJID_CLIENT,
                        IID_IAccessible,
                        reinterpret_cast<void**>(&accessible))) &&
                    accessible != nullptr) {
                    const LRESULT result = LresultFromObject(
                        IID_IAccessible,
                        wparam,
                        accessible);
                    accessible->Release();
                    return result;
                }
            }
            break;
        case WM_SETFOCUS:
            NotifyWinEvent(EVENT_OBJECT_FOCUS, window_, OBJID_CLIENT, CHILDID_SELF);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_ENTERSIZEMOVE:
            cancel_settings_scroll_session();
            window_resizing_ = true;
            hide_active_input_controls();
            return 0;
        case WM_EXITSIZEMOVE:
            window_resizing_ = false;
            finish_input_control_relayout();
            return 0;
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) {
                cancel_settings_scroll_session();
                hide_active_input_controls();
                return 0;
            }
            if (!window_resizing_) {
                cancel_settings_scroll_session();
                hide_active_input_controls();
            }
            if (render_target_) {
                const auto width = static_cast<UINT32>(LOWORD(lparam));
                const auto height = static_cast<UINT32>(HIWORD(lparam));
                static_cast<void>(render_target_->Resize(D2D1::SizeU(width, height)));
            }
            InvalidateRect(window_, nullptr, FALSE);
            if (!window_resizing_) {
                finish_input_control_relayout();
            }
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
            if (!window_resizing_) {
                hide_active_input_controls();
                finish_input_control_relayout();
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
                handle_settings_mouse_wheel(GET_WHEEL_DELTA_WPARAM(wparam));
                return 0;
            }
            if (active_page_ == AppPage::metadata_editor) {
                handle_metadata_mouse_wheel(GET_WHEEL_DELTA_WPARAM(wparam));
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
            if (active_page_ == AppPage::metadata_editor) {
                handle_metadata_click(D2D1::Point2F(
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
        case WM_COMMAND:
            if (LOWORD(wparam) == kSettingsTokenEditId &&
                HIWORD(wparam) == EN_SETFOCUS) {
                close_settings_dropdown();
            }
            break;
        case WM_CTLCOLOREDIT:
            if (reinterpret_cast<HWND>(lparam) == settings_token_edit_ ||
                reinterpret_cast<HWND>(lparam) == metadata_edit_ ||
                reinterpret_cast<HWND>(lparam) == settings_cddb_server_edit_ ||
                reinterpret_cast<HWND>(lparam) == settings_cddb_email_edit_ ||
                is_metadata_edit(reinterpret_cast<HWND>(lparam))) {
                const auto device_context = reinterpret_cast<HDC>(wparam);
                SetTextColor(device_context, colorref(theme_.text));
                SetBkColor(device_context, colorref(theme_.elevated));
                SetBkMode(device_context, OPAQUE);
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
            if (wparam == kSettingsScrollSettleTimer) {
                settle_settings_scroll_session();
                return 0;
            }
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
        case kMetadataEditSaveMessage:
            commit_metadata_edit();
            return 0;
        case kMetadataEditCloseMessage:
            close_metadata_edit();
            return 0;
        case kCddbSubmissionReadyMessage:
            receive_cddb_submission(std::unique_ptr<CddbSubmissionSnapshot>(
                reinterpret_cast<CddbSubmissionSnapshot*>(lparam)));
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kAnimationTimer);
            KillTimer(window_, kSettingsScrollSettleTimer);
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
        metadata,
    };

    enum class AppPage {
        player,
        settings,
        metadata_editor,
    };

    enum class SettingsDropdown {
        none,
        audio_engine,
        audio_endpoint,
    };

    struct SettingsDropdownHit final {
        D2D1_RECT_F rectangle{};
        std::size_t option_index{};
    };

    enum class MetadataEditField {
        none,
        album_title,
        album_artist,
        track_title,
        track_artist,
    };

    using ControlIcon = detail::ControlIcon;

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
        result = create_brush(color(theme_.background), background_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.surface), surface_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.elevated), elevated_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.border), border_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.text), text_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.secondary), secondary_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.muted), muted_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.accent), accent_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.accent_text), accent_text_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.success), success_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.warning), warning_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.error), error_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.accent, 0.14F), selection_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.text, 0.07F), hover_brush_);
        if (SUCCEEDED(result)) result = create_brush(color(theme_.disc, 0.72F), disc_brush_);
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
        const BOOL dark = theme_.dark && !theme_.high_contrast;
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
        const int backdrop = theme_.high_contrast ? 0 : 2;
        static_cast<void>(DwmSetWindowAttribute(
            window_,
            kDwmSystemBackdropType,
            &backdrop,
            sizeof(backdrop)));
    }

    void refresh_theme()
    {
        theme_ = query_system_theme();
        discard_device_resources();
        if (settings_edit_brush_ != nullptr) {
            DeleteObject(settings_edit_brush_);
        }
        settings_edit_brush_ = CreateSolidBrush(colorref(theme_.elevated));
        apply_window_materials();
        diagnostics_.record(
            L"theme",
            std::format(
                L"changed dark={} high-contrast={}",
                theme_.dark ? 1 : 0,
                theme_.high_contrast ? 1 : 0));
        InvalidateRect(window_, nullptr, FALSE);
#if 0 // 旧内联 calculate_layout；布局已合并进 detail::calculate_layout。
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
        result.settings_cddb_card = D2D1::RectF(
            margin,
            358.0F,
            size.width - margin,
            size.height - 24.0F);
        result.settings_cddb_toggle = D2D1::RectF(
            result.settings_cddb_card.right - 76.0F,
            result.settings_cddb_card.top + 22.0F,
            result.settings_cddb_card.right - 24.0F,
            result.settings_cddb_card.top + 50.0F);
        result.settings_cddb_server_edit = D2D1::RectF(
            result.settings_cddb_card.left + 24.0F,
            result.settings_cddb_card.top + 86.0F,
            result.settings_cddb_card.right - 24.0F,
            result.settings_cddb_card.top + 128.0F);
        result.settings_cddb_email_edit = D2D1::RectF(
            result.settings_cddb_card.left + 24.0F,
            result.settings_cddb_card.top + 150.0F,
            result.settings_cddb_card.right - 136.0F,
            result.settings_cddb_card.top + 192.0F);
        result.settings_cddb_save = D2D1::RectF(
            result.settings_cddb_card.right - 120.0F,
            result.settings_cddb_card.top + 150.0F,
            result.settings_cddb_card.right - 24.0F,
            result.settings_cddb_card.top + 192.0F);
        result.metadata_button = D2D1::RectF(
            result.track_list.left + 88.0F,
            82.0F,
            result.track_list.left + 184.0F,
            122.0F);
        result.metadata_page = D2D1::RectF(
            margin,
            84.0F,
            size.width - margin,
            size.height - 24.0F);
        result.metadata_back = D2D1::RectF(
            size.width - margin - 132.0F,
            88.0F,
            size.width - margin,
            128.0F);
        const float editor_top = 152.0F;
        const float editor_gap = 16.0F;
        const float editor_half = (size.width - margin * 2.0F - editor_gap) * 0.5F;
        result.metadata_album_title_edit = D2D1::RectF(
            margin, editor_top, margin + editor_half, editor_top + 42.0F);
        result.metadata_album_artist_edit = D2D1::RectF(
            margin + editor_half + editor_gap,
            editor_top,
            size.width - margin,
            editor_top + 42.0F);
        result.metadata_category_edit = D2D1::RectF(
            margin, editor_top + 66.0F, margin + editor_half, editor_top + 108.0F);
        result.metadata_year_edit = D2D1::RectF(
            margin + editor_half + editor_gap,
            editor_top + 66.0F,
            size.width - margin,
            editor_top + 108.0F);
        result.metadata_track_list = D2D1::RectF(
            margin,
            editor_top + 132.0F,
            size.width - margin,
            size.height - 150.0F);
        result.metadata_track_title_edit = D2D1::RectF(
            margin,
            size.height - 126.0F,
            margin + editor_half,
            size.height - 84.0F);
        result.metadata_track_artist_edit = D2D1::RectF(
            margin + editor_half + editor_gap,
            size.height - 126.0F,
            size.width - margin,
            size.height - 84.0F);
        result.metadata_save = D2D1::RectF(
            margin,
            size.height - 68.0F,
            margin + 118.0F,
            size.height - 28.0F);
        result.metadata_test_submit = D2D1::RectF(
            size.width - margin - 254.0F,
            size.height - 68.0F,
            size.width - margin - 126.0F,
            size.height - 28.0F);
        result.metadata_submit = D2D1::RectF(
            size.width - margin - 112.0F,
            size.height - 68.0F,
            size.width - margin,
            size.height - 28.0F);
        return result;
#endif
    }

    [[nodiscard]] static float settings_maximum_scroll(const float height) noexcept
    {
        return std::max(0.0F, kSettingsContentBottom - (height - 24.0F));
    }

    void paint()
    {
        PAINTSTRUCT paint_structure{};
        BeginPaint(window_, &paint_structure);
        if (SUCCEEDED(create_device_resources())) {
            render_target_->BeginDraw();
            render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
            render_target_->Clear(color(theme_.background));
            const auto size = render_target_->GetSize();
            settings_scroll_offset_ = std::clamp(
                settings_scroll_offset_,
                0.0F,
                settings_maximum_scroll(size.height));
            layout_ = detail::calculate_layout(
                size.width,
                size.height,
                settings_scroll_offset_);
            draw_header();
            if (active_page_ == AppPage::settings) {
                update_settings_control_bounds();
                draw_settings_page();
            } else if (active_page_ == AppPage::metadata_editor) {
                update_metadata_edit_bounds();
                update_metadata_editor_bounds();
                draw_metadata_editor_page();
            } else {
                update_metadata_edit_bounds();
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

    void hide_active_input_controls()
    {
        const HWND focused = GetFocus();
        if (focused != nullptr && IsChild(window_, focused) != FALSE) {
            resize_focus_ = focused;
        }
        const auto hide = [](const HWND edit) {
            if (edit != nullptr) {
                ShowWindow(edit, SW_HIDE);
            }
        };
        if (active_page_ == AppPage::settings) {
            settings_controls_ready_ = false;
            hide(settings_token_edit_);
            hide(settings_cddb_server_edit_);
            hide(settings_cddb_email_edit_);
        } else if (active_page_ == AppPage::metadata_editor) {
            hide(metadata_album_title_edit_);
            hide(metadata_album_artist_edit_);
            hide(metadata_category_edit_);
            hide(metadata_year_edit_);
            hide(metadata_track_title_edit_);
            hide(metadata_track_artist_edit_);
        } else {
            hide(metadata_edit_);
        }
    }

    void restore_active_input_controls()
    {
        const auto show = [](const HWND edit) {
            if (edit != nullptr) {
                ShowWindow(edit, SW_SHOWNOACTIVATE);
            }
        };
        if (active_page_ == AppPage::settings) {
            settings_controls_ready_ = true;
            update_settings_control_bounds();
        } else if (active_page_ == AppPage::metadata_editor) {
            update_metadata_edit_bounds();
            update_metadata_editor_bounds();
            show(metadata_album_title_edit_);
            show(metadata_album_artist_edit_);
            show(metadata_category_edit_);
            show(metadata_year_edit_);
            show(metadata_track_title_edit_);
            show(metadata_track_artist_edit_);
        } else {
            update_metadata_edit_bounds();
            show(metadata_edit_);
        }
        if (resize_focus_ != nullptr &&
            IsWindowVisible(resize_focus_) != FALSE) {
            SetFocus(resize_focus_);
        }
        resize_focus_ = nullptr;
    }

    void finish_input_control_relayout()
    {
        static_cast<void>(RedrawWindow(
            window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
        restore_active_input_controls();
    }

    void cancel_settings_scroll_session()
    {
        KillTimer(window_, kSettingsScrollSettleTimer);
        settings_scroll_active_ = false;
    }

    void begin_settings_scroll_session()
    {
        if (!settings_scroll_active_) {
            settings_scroll_active_ = true;
            hide_active_input_controls();
            static_cast<void>(RedrawWindow(
                window_, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
        }
        KillTimer(window_, kSettingsScrollSettleTimer);
        static_cast<void>(SetTimer(
            window_,
            kSettingsScrollSettleTimer,
            kSettingsScrollSettleDelayMs,
            nullptr));
    }

    void settle_settings_scroll_session()
    {
        KillTimer(window_, kSettingsScrollSettleTimer);
        if (!settings_scroll_active_) {
            return;
        }
        settings_scroll_active_ = false;
        if (!window_resizing_ && active_page_ == AppPage::settings) {
            finish_input_control_relayout();
        }
    }

    void scroll_settings_to(const float requested_offset)
    {
        const float next = std::clamp(
            requested_offset,
            0.0F,
            settings_maximum_scroll(layout_.height));
        if (std::abs(next - settings_scroll_offset_) < 0.5F) {
            return;
        }
        begin_settings_scroll_session();
        settings_scroll_offset_ = next;
        InvalidateRect(window_, nullptr, FALSE);
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
        const float source_width = layout_.cover.right - layout_.cover.left;
        std::size_t source_rows{};
        float measured_row_width{};
        for (const auto& source : disc_.metadata_sources) {
            const float pill_width = metadata_source_pill_width(
                source_width, source);
            const float next_width = measured_row_width == 0.0F
                ? pill_width
                : measured_row_width + 8.0F + pill_width;
            if (measured_row_width != 0.0F && next_width > source_width) {
                ++source_rows;
                measured_row_width = pill_width;
            } else {
                measured_row_width = next_width;
            }
        }
        if (measured_row_width != 0.0F) {
            ++source_rows;
        }
        const float album_info_height =
            60.0F + static_cast<float>(source_rows) * 28.0F;
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
        float pill_left = layout_.cover.left;
        float pill_top = text_top + 64.0F;
        for (const auto& source : disc_.metadata_sources) {
            const float pill_width = metadata_source_pill_width(
                source_width, source);
            if (pill_left != layout_.cover.left &&
                pill_left + pill_width > layout_.cover.right) {
                pill_left = layout_.cover.left;
                pill_top += 28.0F;
            }
            draw_metadata_source_pill(
                pill_left, pill_top, pill_width, source);
            pill_left += pill_width + 8.0F;
        }
        const float right_left = layout_.track_list.left;
        draw_text(
            L"曲目",
            heading_format_.Get(),
            D2D1::RectF(right_left, 84.0F, right_left + 130.0F, 122.0F),
            text_brush_.Get());
        const auto metadata_button = D2D1::RoundedRect(
            layout_.metadata_button,
            10.0F,
            10.0F);
        render_target_->FillRoundedRectangle(
            metadata_button,
            hovered_control_ == HoveredControl::metadata
                ? elevated_brush_.Get()
                : surface_brush_.Get());
        render_target_->DrawRoundedRectangle(metadata_button, border_brush_.Get(), 1.0F);
        draw_text(
            L"编辑元数据",
            caption_format_.Get(),
            layout_.metadata_button,
            secondary_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);
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
                               : muted_brush_.Get(),
                DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            draw_text(
                track.title,
                track_format_.Get(),
                D2D1::RectF(row.left + 56.0F, top, row.right - 88.0F, top + row_height),
                track.is_audio ? text_brush_.Get() : muted_brush_.Get(),
                DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            draw_text(
                track.is_audio ? format_duration(track.frame_count) : L"数据",
                track_duration_format_.Get(),
                D2D1::RectF(row.right - 76.0F, top, row.right - 12.0F, top + row_height),
                track.is_audio ? secondary_brush_.Get() : warning_brush_.Get(),
                DWRITE_TEXT_ALIGNMENT_TRAILING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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

    void draw_control_icon(
        const ControlIcon icon,
        const D2D1_POINT_2F center,
        ID2D1Brush* brush)
    {
        detail::draw_control_icon(
            factory_.Get(), render_target_.Get(), icon, center, brush);
    }

    [[nodiscard]] static float metadata_source_pill_width(
        const float maximum_width,
        const std::wstring& source)
    {
        return std::min(
            maximum_width,
            std::max(76.0F, 28.0F + static_cast<float>(source.size()) * 7.0F));
    }

    void draw_metadata_source_pill(
        const float left,
        const float top,
        const float width,
        const std::wstring& source)
    {
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
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    void draw_text(
        const std::wstring& text,
        IDWriteTextFormat* format,
        const D2D1_RECT_F rectangle,
        ID2D1Brush* brush,
        const DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
        const std::optional<DWRITE_PARAGRAPH_ALIGNMENT> paragraph_alignment =
            std::nullopt)
    {
        detail::draw_text(
            render_target_.Get(), text, format, rectangle, brush, alignment,
            paragraph_alignment);
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
        if (active_page_ == AppPage::metadata_editor) {
            close_metadata_editor(false);
        }
        ++disc_generation_;
        disc_ = std::move(*snapshot);
        load_cover_bitmap(disc_.cover_art_path);
        diagnostics_.record(
            L"disc",
            std::format(
                L"refresh complete optical={} toc={} tracks={}",
                disc_.has_optical_drive ? 1 : 0,
                disc_.toc ? 1 : 0,
                disc_.tracks.size()));
        preferred_metadata_release_id_.clear();
        selected_track_ = first_audio_track();
        restore_playback_position();
        apply_saved_metadata_override();
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

        diagnostics_.record(
            L"device",
            std::format(L"lifecycle event={}", static_cast<unsigned int>(event)));

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
        const std::wstring preferred_release_id = preferred_metadata_release_id_;
        const platform::windows::OnlineMetadataOptions metadata_options{
            user_settings_.cddb_enabled,
            user_settings_.cddb_server,
            user_settings_.cddb_email,
        };
        const std::uint64_t disc_generation = disc_generation_;
        metadata_worker_ = std::jthread([
            target_window,
            toc,
            album_title,
            album_artist,
            preferred_release_id,
            metadata_options,
            disc_generation] {
            auto snapshot = std::make_unique<OnlineMetadataSnapshot>();
            snapshot->disc_generation = disc_generation;
            snapshot->metadata = platform::windows::lookup_online_metadata(
                toc,
                album_title,
                album_artist,
                preferred_release_id,
                metadata_options);
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
        disc_.metadata_sources = make_metadata_source_labels(
            disc_.has_cd_text, false, metadata.sources);
        const auto merge_ui_field = [](
                                        std::wstring& value,
                                        platform::windows::MetadataSource& source,
                                        const platform::windows::SourcedMetadataValue& incoming) {
            platform::windows::SourcedMetadataValue destination{value, source};
            if (platform::windows::merge_metadata_value(
                    destination,
                    incoming.value,
                    incoming.source)) {
                value = std::move(destination.value);
                source = destination.source;
            }
        };
        merge_ui_field(
            disc_.album_title,
            disc_.album_title_source,
            metadata.editable.album_title);
        merge_ui_field(
            disc_.album_artist,
            disc_.album_artist_source,
            metadata.editable.album_artist);
        disc_.release_candidates = metadata.release_candidates;
        disc_.selected_release_id = metadata.selected_release_id;
        preferred_metadata_release_id_ = metadata.selected_release_id;
        disc_.cover_art_path = metadata.cover_art_path;
        disc_.cover_art_source = metadata.cover_art_source;
        disc_.release_mbid = metadata.release_mbid;
        disc_.release_group_mbid = metadata.release_group_mbid;
        disc_.reference_release_mbid = metadata.reference_release_mbid;
        disc_.reference_release_group_mbid =
            metadata.reference_release_group_mbid;
        if (!disc_.has_user_metadata) {
            if (!metadata.cddb_category.empty()) {
                disc_.cddb_category = metadata.cddb_category;
            }
            disc_.cddb_year = metadata.cddb_year;
            disc_.cddb_revision = metadata.cddb_revision;
        }
        load_cover_bitmap(disc_.cover_art_path);
        const std::size_t title_count = std::min(
            disc_.tracks.size(),
            metadata.track_titles.size());
        for (std::size_t index = 0; index < title_count; ++index) {
            if (index < metadata.editable.tracks.size()) {
                merge_ui_field(
                    disc_.tracks[index].title,
                    disc_.tracks[index].title_source,
                    metadata.editable.tracks[index].title);
            }
            if (disc_.tracks[index].title_source !=
                platform::windows::MetadataSource::unknown) {
                disc_.tracks[index].has_metadata_title = true;
            }
        }
        const std::size_t artist_count = std::min(
            disc_.tracks.size(),
            metadata.track_artists.size());
        for (std::size_t index = 0; index < artist_count; ++index) {
            if (index < metadata.editable.tracks.size()) {
                merge_ui_field(
                    disc_.tracks[index].artist,
                    disc_.tracks[index].artist_source,
                    metadata.editable.tracks[index].artist);
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
            update_settings_dropdown_hover(point);
            const bool changed = hovered_track_.has_value() ||
                hovered_control_ != HoveredControl::none;
            hovered_track_.reset();
            hovered_control_ = HoveredControl::none;
            if (changed) {
                InvalidateRect(window_, nullptr, FALSE);
            }
            return;
        }
        if (active_page_ == AppPage::metadata_editor) {
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
        if (contains(layout_.metadata_button, point)) return HoveredControl::metadata;
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
        case HoveredControl::metadata:
            open_metadata_editor();
            break;
        case HoveredControl::none:
            break;
        }
    }

    LRESULT handle_key_down(const WPARAM key)
    {
        if (key == VK_F1) {
            show_keyboard_help();
            return 0;
        }
        if (active_page_ == AppPage::settings) {
            if (handle_settings_dropdown_key(key)) {
                return 0;
            }
            if (key == VK_ESCAPE) {
                close_settings();
            } else if (key == VK_RETURN) {
                save_settings();
            } else if (key == 'D') {
                open_settings_dropdown(SettingsDropdown::audio_endpoint);
            } else if (key == 'E') {
                open_settings_dropdown(SettingsDropdown::audio_engine);
            } else if (key == 'X') {
                toggle_exclusive_output();
            } else if (key == 'F') {
                toggle_shared_fallback();
            } else if (key == 'G') {
                export_diagnostics();
            } else if (key == VK_PRIOR) {
                scroll_settings_to(settings_scroll_offset_ - 160.0F);
            } else if (key == VK_NEXT) {
                scroll_settings_to(settings_scroll_offset_ + 160.0F);
            } else if (key == VK_HOME) {
                scroll_settings_to(0.0F);
            } else if (key == VK_END) {
                scroll_settings_to(settings_maximum_scroll(layout_.height));
            }
            return 0;
        }
        if (active_page_ == AppPage::metadata_editor) {
            if (key == VK_ESCAPE) {
                close_metadata_editor();
            } else if (key == VK_UP) {
                select_metadata_track(-1);
            } else if (key == VK_DOWN) {
                select_metadata_track(1);
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
        case VK_F2: {
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            begin_metadata_edit(
                control
                    ? (shift ? MetadataEditField::album_artist
                             : MetadataEditField::album_title)
                    : (shift ? MetadataEditField::track_artist
                             : MetadataEditField::track_title));
            return 0;
        }
        case 'M':
            select_next_metadata_release();
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

    void show_keyboard_help() const
    {
        MessageBoxW(
            window_,
            L"播放器：空格/Enter 播放或暂停；↑/↓ 选择曲目；Ctrl+←/→ 切轨；"
            L"Home/End 首末曲；F2 组合键修订元数据；F5 刷新；Ctrl+E 弹出；Esc 停止。\n\n"
            L"设置：D 切换输出设备；X 切换独占；F 切换显式共享回退；"
            L"G 导出脱敏诊断；Enter 保存；Esc 返回。\n\n"
            L"应用跟随 Windows 浅色/深色和高对比度设置。",
            L"CD.404 键盘与辅助功能帮助",
            MB_OK | MB_ICONINFORMATION);
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

    void select_next_metadata_release()
    {
        if (disc_.release_candidates.size() < 2U || metadata_worker_.joinable()) {
            return;
        }
        const std::size_t current = platform::windows::select_metadata_candidate(
            disc_.release_candidates,
            disc_.selected_release_id);
        const std::size_t next = (current + 1U) % disc_.release_candidates.size();
        preferred_metadata_release_id_ =
            disc_.release_candidates[next].release_id;
        ui_message_ = std::format(
            L"正在切换 MusicBrainz 发行版：{}",
            disc_.release_candidates[next].album_title);
        ui_message_is_error_ = false;
        start_online_metadata_lookup();
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
        request.output.endpoint_id = user_settings_.audio_endpoint_id;
        request.output.mode = user_settings_.audio_exclusive_mode
            ? platform::windows::WasapiShareMode::exclusive
            : platform::windows::WasapiShareMode::shared;
        request.output.allow_shared_fallback =
            user_settings_.audio_allow_shared_fallback;
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

        diagnostics_.record(
            L"playback",
            std::format(
                L"complete state={} error={} system={} audio=0x{:08X} requested={} actual={} fallback={}",
                static_cast<unsigned int>(result->final_state),
                static_cast<unsigned int>(result->error),
                result->system_error,
                static_cast<unsigned int>(result->audio_status),
                static_cast<unsigned int>(result->output_open_result.requested_mode),
                static_cast<unsigned int>(result->output_open_result.actual_mode),
                result->output_open_result.fallback_attempted ? 1 : 0));

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
            if (result->output_open_result.fallback_attempted) {
                ui_message_ = L"独占模式失败（" +
                    platform::windows::describe_wasapi_status(
                        result->output_open_result.fallback_reason) +
                    L"），本次已明确回退到共享模式";
                ui_message_is_error_ = false;
            }
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
        ui_message_ = playback_error_message(result);
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
        update_accessible_name();
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

    void update_accessible_name()
    {
        std::wstring name = L"CD.404 播放器";
        if (selected_track_ < disc_.tracks.size()) {
            const auto& track = disc_.tracks[selected_track_];
            name += L" — " + track.title;
            if (!track.artist.empty()) {
                name += L" — " + track.artist;
            }
        } else if (!disc_.status.empty()) {
            name += L" — " + disc_.status;
        }
        if (name == accessible_name_) {
            return;
        }
        accessible_name_ = std::move(name);
        SetWindowTextW(window_, accessible_name_.c_str());
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, window_, OBJID_WINDOW, CHILDID_SELF);
    }

    void apply_saved_metadata_override()
    {
        if (current_disc_key_.empty()) {
            return;
        }
        const auto saved = user_settings_.metadata_overrides.find(current_disc_key_);
        if (saved == user_settings_.metadata_overrides.end() ||
            saved->second.track_titles.size() != disc_.tracks.size() ||
            (!saved->second.track_artists.empty() &&
             saved->second.track_artists.size() != disc_.tracks.size())) {
            return;
        }
        const auto& metadata = saved->second;
        disc_.album_title = metadata.album_title;
        disc_.album_artist = metadata.album_artist;
        disc_.cddb_category = metadata.category.empty() ? L"misc" : metadata.category;
        disc_.cddb_year = metadata.year;
        disc_.cddb_revision = metadata.revision;
        disc_.has_user_metadata = true;
        disc_.metadata_sources = make_metadata_source_labels(
            disc_.has_cd_text, true, {});
        for (std::size_t index = 0; index < disc_.tracks.size(); ++index) {
            disc_.tracks[index].title = metadata.track_titles[index];
            disc_.tracks[index].has_metadata_title = true;
            if (!metadata.track_artists.empty()) {
                disc_.tracks[index].artist = metadata.track_artists[index];
            }
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

    static LRESULT CALLBACK metadata_edit_window_proc(
        const HWND edit,
        const UINT message,
        const WPARAM wparam,
        const LPARAM lparam)
    {
        auto* self = reinterpret_cast<MainWindow*>(
            GetWindowLongPtrW(edit, GWLP_USERDATA));
        if (self != nullptr && message == WM_KEYDOWN) {
            if (wparam == VK_RETURN) {
                PostMessageW(self->window_, kMetadataEditSaveMessage, 0, 0);
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                PostMessageW(self->window_, kMetadataEditCloseMessage, 0, 0);
                return 0;
            }
        }
        if (self != nullptr && message == WM_GETDLGCODE) {
            return DLGC_WANTALLKEYS;
        }
        return self != nullptr && self->metadata_edit_original_proc_ != nullptr
            ? CallWindowProcW(
                  self->metadata_edit_original_proc_,
                  edit,
                  message,
                  wparam,
                  lparam)
            : DefWindowProcW(edit, message, wparam, lparam);
    }

    void begin_metadata_edit(const MetadataEditField field)
    {
        if (!disc_.toc || selected_track_ >= disc_.tracks.size()) {
            return;
        }
        close_metadata_edit();
        metadata_edit_field_ = field;
        const std::wstring* value{};
        platform::windows::MetadataSource source{
            platform::windows::MetadataSource::unknown};
        switch (field) {
        case MetadataEditField::album_title:
            value = &disc_.album_title;
            source = disc_.album_title_source;
            break;
        case MetadataEditField::album_artist:
            value = &disc_.album_artist;
            source = disc_.album_artist_source;
            break;
        case MetadataEditField::track_title:
            value = &disc_.tracks[selected_track_].title;
            source = disc_.tracks[selected_track_].title_source;
            break;
        case MetadataEditField::track_artist:
            value = &disc_.tracks[selected_track_].artist;
            source = disc_.tracks[selected_track_].artist_source;
            break;
        case MetadataEditField::none:
            return;
        }
        if (settings_font_ == nullptr) {
            settings_font_ = CreateFontW(
                -MulDiv(14, static_cast<int>(GetDpiForWindow(window_)), 72),
                0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
        if (settings_edit_brush_ == nullptr) {
            settings_edit_brush_ = CreateSolidBrush(colorref(theme_.elevated));
        }
        metadata_edit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            value->c_str(),
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 1, 1,
            window_,
            nullptr,
            instance_,
            nullptr);
        if (metadata_edit_ == nullptr) {
            metadata_edit_field_ = MetadataEditField::none;
            return;
        }
        static_cast<void>(SetWindowTheme(metadata_edit_, L" ", L" "));
        SendMessageW(
            metadata_edit_,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(settings_font_),
            TRUE);
        SendMessageW(metadata_edit_, EM_SETLIMITTEXT, 512, 0);
        SetWindowLongPtrW(
            metadata_edit_,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(this));
        metadata_edit_original_proc_ = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(
                metadata_edit_,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(metadata_edit_window_proc)));
        update_metadata_edit_bounds();
        static_cast<void>(RedrawWindow(
            window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
        ShowWindow(metadata_edit_, SW_SHOWNOACTIVATE);
        SendMessageW(metadata_edit_, EM_SETSEL, 0, -1);
        SetFocus(metadata_edit_);
        ui_message_ = std::format(
            L"编辑元数据 · 当前字段来源 {}：Enter 保存，Esc 取消",
            platform::windows::to_string(source));
        ui_message_is_error_ = false;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void update_metadata_edit_bounds()
    {
        if (metadata_edit_ == nullptr || window_ == nullptr) {
            return;
        }
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0F;
        const int left = static_cast<int>(std::lround(24.0F * scale));
        const int top = static_cast<int>(std::lround(62.0F * scale));
        RECT client{};
        GetClientRect(window_, &client);
        SetWindowPos(
            metadata_edit_,
            HWND_TOP,
            left,
            top,
            std::max(static_cast<int>(client.right) - left * 2, 160),
            static_cast<int>(std::lround(38.0F * scale)),
            SWP_NOACTIVATE);
    }

    void close_metadata_edit()
    {
        if (metadata_edit_ != nullptr) {
            DestroyWindow(metadata_edit_);
            metadata_edit_ = nullptr;
            metadata_edit_original_proc_ = nullptr;
        }
        metadata_edit_field_ = MetadataEditField::none;
        if (window_ != nullptr) {
            SetFocus(window_);
        }
    }

    void commit_metadata_edit()
    {
        if (metadata_edit_ == nullptr || current_disc_key_.empty() ||
            selected_track_ >= disc_.tracks.size()) {
            close_metadata_edit();
            return;
        }
        const int length = GetWindowTextLengthW(metadata_edit_);
        std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
        const int written = GetWindowTextW(metadata_edit_, value.data(), length + 1);
        value.resize(static_cast<std::size_t>(std::max(written, 0)));

        switch (metadata_edit_field_) {
        case MetadataEditField::album_title:
            disc_.album_title = value;
            disc_.album_title_source = platform::windows::MetadataSource::user;
            break;
        case MetadataEditField::album_artist:
            disc_.album_artist = value;
            disc_.album_artist_source = platform::windows::MetadataSource::user;
            break;
        case MetadataEditField::track_title:
            disc_.tracks[selected_track_].title = value;
            disc_.tracks[selected_track_].title_source =
                platform::windows::MetadataSource::user;
            disc_.tracks[selected_track_].has_metadata_title = true;
            break;
        case MetadataEditField::track_artist:
            disc_.tracks[selected_track_].artist = value;
            disc_.tracks[selected_track_].artist_source =
                platform::windows::MetadataSource::user;
            break;
        case MetadataEditField::none:
            close_metadata_edit();
            return;
        }

        platform::windows::MetadataCacheEntry entry;
        entry.disc_key = current_disc_key_;
        entry.selected_release_id = disc_.selected_release_id;
        entry.updated_unix_seconds = unix_time_now();
        entry.metadata.album_title = {
            disc_.album_title,
            disc_.album_title_source};
        entry.metadata.album_artist = {
            disc_.album_artist,
            disc_.album_artist_source};
        for (const auto& track : disc_.tracks) {
            entry.metadata.tracks.push_back({
                {track.title, track.title_source},
                {track.artist, track.artist_source},
            });
        }
        const bool saved = platform::windows::save_metadata_cache(entry);
        close_metadata_edit();
        sync_system_media(true);
        ui_message_ = saved
            ? L"元数据修订已保存（F2/Shift/Ctrl 可编辑其他字段）"
            : L"元数据修订未能写入本地缓存";
        ui_message_is_error_ = !saved;
        InvalidateRect(window_, nullptr, FALSE);
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
        if (self != nullptr && edit == self->settings_token_edit_ &&
            self->settings_token_mask_active_) {
            const bool replaces_saved_token =
                message == WM_PASTE || message == WM_CUT ||
                (message == WM_KEYDOWN &&
                 (wparam == VK_BACK || wparam == VK_DELETE)) ||
                (message == WM_CHAR && wparam >= 0x20U);
            if (replaces_saved_token) {
                self->settings_token_mask_active_ = false;
                SetWindowTextW(edit, L"");
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
        cancel_settings_scroll_session();
        settings_scroll_offset_ = 0.0F;
        settings_controls_ready_ = false;
        settings_save_failed_ = false;
        settings_saved_ = false;
        settings_input_required_ = false;
        audio_endpoints_ = platform::windows::enumerate_wasapi_render_endpoints(
            &audio_endpoint_status_);
        audio_endpoints_.insert(
            audio_endpoints_.begin(),
            platform::windows::WasapiEndpoint{L"", L"系统默认设备", true});
        selected_audio_endpoint_ = find_audio_endpoint_index(
            audio_endpoints_, user_settings_.audio_endpoint_id);
        cddb_settings_error_.clear();
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
            settings_edit_brush_ = CreateSolidBrush(colorref(theme_.elevated));
        }
        settings_token_edit_ = CreateWindowExW(
            0,
            L"EDIT",
            L"",
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
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
            static_cast<void>(SetWindowTheme(settings_token_edit_, L" ", L" "));
            SendMessageW(
                settings_token_edit_,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(settings_font_),
                TRUE);
            SendMessageW(settings_token_edit_, EM_SETLIMITTEXT, 512, 0);
            SetWindowLongPtrW(
                settings_token_edit_,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(this));
            settings_edit_original_proc_ = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(
                    settings_token_edit_,
                    GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(settings_edit_window_proc)));
            refresh_settings_token_status();
        }
        const auto create_plain_edit = [this](
            const int identifier,
            const wchar_t* initial_text,
            const WPARAM limit) {
            HWND edit = CreateWindowExW(
                0,
                L"EDIT",
                initial_text,
                WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                0,
                0,
                1,
                1,
                window_,
                reinterpret_cast<HMENU>(static_cast<std::intptr_t>(identifier)),
                instance_,
                nullptr);
            if (edit != nullptr) {
                static_cast<void>(SetWindowTheme(edit, L" ", L" "));
                SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(settings_font_), TRUE);
                SendMessageW(edit, EM_SETLIMITTEXT, limit, 0);
                SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                const auto original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                    edit,
                    GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(settings_edit_window_proc)));
                if (settings_edit_original_proc_ == nullptr) {
                    settings_edit_original_proc_ = original;
                }
            }
            return edit;
        };
        settings_cddb_server_edit_ = create_plain_edit(
            kSettingsCddbServerEditId,
            user_settings_.cddb_server.c_str(),
            512);
        settings_cddb_email_edit_ = create_plain_edit(
            kSettingsCddbEmailEditId,
            user_settings_.cddb_email.c_str(),
            320);
        update_settings_control_bounds();
        static_cast<void>(RedrawWindow(
            window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
        settings_controls_ready_ = true;
        update_settings_control_bounds();
        if (settings_token_edit_ != nullptr) {
            SetFocus(settings_token_edit_);
        }
    }

    void refresh_settings_token_status()
    {
        if (settings_token_edit_ == nullptr) {
            return;
        }
        settings_token_character_count_ =
            platform::windows::listenbrainz_token_character_count();
        settings_token_mask_active_ = settings_token_character_count_ != 0U;
        const std::wstring masked_value(
            settings_token_character_count_,
            L'x');
        SetWindowTextW(settings_token_edit_, masked_value.c_str());
        SendMessageW(settings_token_edit_, EM_SETSEL, 0, 0);
        static_cast<void>(RedrawWindow(
            settings_token_edit_,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW));
    }

    void close_settings()
    {
        cancel_settings_scroll_session();
        close_settings_dropdown();
        settings_controls_ready_ = false;
        resize_focus_ = nullptr;
        if (settings_token_edit_ != nullptr) {
            DestroyWindow(settings_token_edit_);
            settings_token_edit_ = nullptr;
        }
        if (settings_cddb_server_edit_ != nullptr) {
            DestroyWindow(settings_cddb_server_edit_);
            settings_cddb_server_edit_ = nullptr;
        }
        if (settings_cddb_email_edit_ != nullptr) {
            DestroyWindow(settings_cddb_email_edit_);
            settings_cddb_email_edit_ = nullptr;
        }
        settings_edit_original_proc_ = nullptr;
        if (active_page_ == AppPage::settings) {
            active_page_ = AppPage::player;
            settings_save_failed_ = false;
            settings_saved_ = false;
            settings_input_required_ = false;
            cddb_settings_error_.clear();
            if (window_ != nullptr) {
                SetFocus(window_);
                InvalidateRect(window_, nullptr, FALSE);
            }
        }
    }

    void save_settings()
    {
        if (active_page_ != AppPage::settings || settings_token_edit_ == nullptr ||
            settings_cddb_server_edit_ == nullptr ||
            settings_cddb_email_edit_ == nullptr) {
            return;
        }
        const auto edit_text = [](const HWND edit) {
            const int length = GetWindowTextLengthW(edit);
            std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
            const int written = GetWindowTextW(edit, value.data(), length + 1);
            value.resize(static_cast<std::size_t>(std::max(written, 0)));
            const auto first = value.find_first_not_of(L" \t\r\n");
            if (first == std::wstring::npos) {
                return std::wstring{};
            }
            const auto last = value.find_last_not_of(L" \t\r\n");
            return value.substr(first, last - first + 1U);
        };
        const std::wstring token = settings_token_mask_active_
            ? std::wstring{}
            : edit_text(settings_token_edit_);
        const std::wstring server = edit_text(settings_cddb_server_edit_);
        const std::wstring email = edit_text(settings_cddb_email_edit_);

        if (!token.empty() &&
            !platform::windows::is_listenbrainz_token_format_valid(token)) {
            settings_input_required_ = true;
            settings_save_failed_ = false;
            settings_saved_ = false;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (!platform::windows::parse_cddb_server(server)) {
            cddb_settings_error_ = L"CDDB 服务器地址无效";
            settings_saved_ = false;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (!email.empty() && !platform::windows::is_valid_cddb_email(email)) {
            cddb_settings_error_ = L"请输入有效的 CDDB 身份邮箱";
            settings_saved_ = false;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }

        if (!token.empty() &&
            !platform::windows::save_listenbrainz_token(token)) {
            settings_save_failed_ = true;
            settings_input_required_ = false;
            settings_saved_ = false;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (!token.empty()) {
            listenbrainz_reporter_.reload_token();
            SetWindowTextW(settings_token_edit_, L"");
            refresh_settings_token_status();
        }
        user_settings_.cddb_server = server;
        user_settings_.cddb_email = email;
        persist_user_settings();
        cddb_settings_error_.clear();
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
            refresh_settings_token_status();
        }
        settings_save_failed_ = false;
        settings_input_required_ = false;
        settings_saved_ = true;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void handle_settings_click(const D2D1_POINT_2F point)
    {
        const ScrollbarGeometry scroll = settings_scrollbar_geometry();
        if (scroll.visible && contains(scroll.track, point)) {
            const float maximum = settings_maximum_scroll(layout_.height);
            const float travel = std::max(
                0.0F,
                scroll.track.bottom - scroll.track.top -
                    (scroll.thumb.bottom - scroll.thumb.top));
            const float thumb_top = std::clamp(
                point.y - scroll.track.top -
                    (scroll.thumb.bottom - scroll.thumb.top) * 0.5F,
                0.0F,
                travel);
            const float next = travel > 0.0F
                ? maximum * thumb_top / travel
                : 0.0F;
            scroll_settings_to(next);
            return;
        }
        if (point.y < kSettingsContentTop) {
            if (contains(layout_.settings_diagnostics_export, point)) {
                export_diagnostics();
            } else if (contains(layout_.settings_back, point) ||
                       contains(layout_.settings_button, point)) {
                close_settings();
            }
            return;
        }
        for (const auto& hit : settings_dropdown_hits_) {
            if (contains(hit.rectangle, point)) {
                apply_settings_dropdown_option(hit.option_index);
                return;
            }
        }
        if (contains(layout_.settings_audio_engine, point)) {
            if (settings_dropdown_ == SettingsDropdown::audio_engine) {
                close_settings_dropdown();
            } else {
                open_settings_dropdown(SettingsDropdown::audio_engine);
            }
            return;
        }
        if (contains(layout_.settings_audio_endpoint, point)) {
            if (settings_dropdown_ == SettingsDropdown::audio_endpoint) {
                close_settings_dropdown();
            } else {
                open_settings_dropdown(SettingsDropdown::audio_endpoint);
            }
            return;
        }
        close_settings_dropdown();
        if (contains(layout_.settings_diagnostics_export, point)) {
            export_diagnostics();
        } else if (contains(layout_.settings_save, point)) {
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
        } else if (contains(layout_.settings_queue_clear, point)) {
            settings_saved_ = listenbrainz_reporter_.clear_pending();
            settings_save_failed_ = !settings_saved_;
            settings_input_required_ = false;
            InvalidateRect(window_, nullptr, FALSE);
        } else if (contains(
                       layout_.settings_audio_exclusive_toggle,
                       point)) {
            toggle_exclusive_output();
        } else if (contains(layout_.settings_audio_fallback_toggle, point)) {
            toggle_shared_fallback();
        } else if (contains(layout_.settings_cddb_save, point)) {
            save_settings();
        } else if (contains(layout_.settings_cddb_toggle, point)) {
            user_settings_.cddb_enabled = !user_settings_.cddb_enabled;
            cddb_settings_error_.clear();
            persist_user_settings();
            InvalidateRect(window_, nullptr, FALSE);
        } else if (contains(layout_.settings_back, point) ||
                   contains(layout_.settings_button, point)) {
            close_settings();
        }
    }

    void export_diagnostics()
    {
        wchar_t path[MAX_PATH]{L'C', L'D', L'.', L'4', L'0', L'4', L'-',
            L'd', L'i', L'a', L'g', L'n', L'o', L's', L't', L'i', L'c', L's',
            L'.', L't', L'x', L't', L'\0'};
        constexpr wchar_t filter[] =
            L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.lpstrDefExt = L"txt";
        dialog.lpstrTitle = L"导出已脱敏的 CD.404 诊断";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
            OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (GetSaveFileNameW(&dialog) == FALSE) {
            return;
        }
        diagnostics_.record(L"diagnostics", L"export requested");
        const bool exported = diagnostics_.export_to(path);
        ui_message_ = exported
            ? L"已导出脱敏诊断（不含 Token、路径、端点 ID 或媒体元数据）"
            : L"诊断导出失败，请检查目标文件权限";
        ui_message_is_error_ = !exported;
        InvalidateRect(window_, nullptr, FALSE);
    }

    [[nodiscard]] std::size_t settings_dropdown_option_count(
        const SettingsDropdown dropdown) const noexcept
    {
        switch (dropdown) {
        case SettingsDropdown::audio_engine:
            return audio_output_engine_count();
        case SettingsDropdown::audio_endpoint:
            return audio_endpoints_.size();
        case SettingsDropdown::none:
            return 0U;
        }
        return 0U;
    }

    [[nodiscard]] std::wstring settings_dropdown_option_label(
        const SettingsDropdown dropdown,
        const std::size_t index) const
    {
        if (dropdown == SettingsDropdown::audio_engine) {
            const auto engine = audio_output_engine_at(index);
            return engine
                ? std::wstring(audio_output_engine_label(*engine))
                : std::wstring{};
        }
        if (dropdown == SettingsDropdown::audio_endpoint &&
            index < audio_endpoints_.size()) {
            return audio_endpoints_[index].name;
        }
        return {};
    }

    [[nodiscard]] std::size_t current_settings_dropdown_option(
        const SettingsDropdown dropdown) const noexcept
    {
        if (dropdown == SettingsDropdown::audio_endpoint) {
            return selected_audio_endpoint_;
        }
        return 0U;
    }

    [[nodiscard]] D2D1_RECT_F settings_dropdown_anchor(
        const SettingsDropdown dropdown) const noexcept
    {
        return dropdown == SettingsDropdown::audio_engine
            ? layout_.settings_audio_engine
            : layout_.settings_audio_endpoint;
    }

    void open_settings_dropdown(const SettingsDropdown dropdown)
    {
        const std::size_t count = settings_dropdown_option_count(dropdown);
        if (dropdown == SettingsDropdown::none || count == 0U) {
            return;
        }
        settings_dropdown_ = dropdown;
        settings_dropdown_highlight_ = std::min(
            current_settings_dropdown_option(dropdown), count - 1U);
        constexpr std::size_t visible_options = 4U;
        settings_dropdown_scroll_ = settings_dropdown_highlight_ >= visible_options
            ? settings_dropdown_highlight_ - visible_options + 1U
            : 0U;
        settings_dropdown_hits_.clear();
        if (window_ != nullptr) {
            SetFocus(window_);
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void close_settings_dropdown()
    {
        if (settings_dropdown_ == SettingsDropdown::none) {
            return;
        }
        settings_dropdown_ = SettingsDropdown::none;
        settings_dropdown_scroll_ = 0U;
        settings_dropdown_highlight_ = 0U;
        settings_dropdown_hits_.clear();
        if (window_ != nullptr) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void apply_audio_engine_selection(const std::size_t selected)
    {
        const auto engine = audio_output_engine_at(selected);
        if (!engine) {
            return;
        }
        user_settings_.audio_output_engine = *engine;
        persist_user_settings();
    }

    void apply_audio_endpoint_selection(const std::size_t selected)
    {
        if (!ui::select_audio_endpoint(
                audio_endpoints_,
                selected,
                selected_audio_endpoint_,
                user_settings_)) {
            return;
        }
        diagnostics_.record(
            L"audio",
            std::format(
                L"output endpoint selection changed default={} index={} count={}",
                user_settings_.audio_endpoint_id.empty() ? 1 : 0,
                selected_audio_endpoint_,
                audio_endpoints_.size()));
        persist_user_settings();
    }

    void apply_settings_dropdown_option(const std::size_t selected)
    {
        if (settings_dropdown_ == SettingsDropdown::audio_engine) {
            apply_audio_engine_selection(selected);
        } else if (settings_dropdown_ == SettingsDropdown::audio_endpoint) {
            apply_audio_endpoint_selection(selected);
        }
        close_settings_dropdown();
    }

    [[nodiscard]] bool handle_settings_dropdown_key(const WPARAM key)
    {
        if (settings_dropdown_ == SettingsDropdown::none) {
            return false;
        }
        const std::size_t count = settings_dropdown_option_count(
            settings_dropdown_);
        if (count == 0U) {
            close_settings_dropdown();
            return true;
        }
        if (key == VK_ESCAPE) {
            close_settings_dropdown();
            return true;
        }
        if (key == VK_RETURN || key == VK_SPACE) {
            apply_settings_dropdown_option(settings_dropdown_highlight_);
            return true;
        }
        if (key == VK_UP && settings_dropdown_highlight_ > 0U) {
            --settings_dropdown_highlight_;
        } else if (key == VK_DOWN && settings_dropdown_highlight_ + 1U < count) {
            ++settings_dropdown_highlight_;
        } else if (key == VK_HOME) {
            settings_dropdown_highlight_ = 0U;
        } else if (key == VK_END) {
            settings_dropdown_highlight_ = count - 1U;
        } else {
            return true;
        }
        constexpr std::size_t visible_options = 4U;
        if (settings_dropdown_highlight_ < settings_dropdown_scroll_) {
            settings_dropdown_scroll_ = settings_dropdown_highlight_;
        } else if (settings_dropdown_highlight_ >=
                   settings_dropdown_scroll_ + visible_options) {
            settings_dropdown_scroll_ =
                settings_dropdown_highlight_ - visible_options + 1U;
        }
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    void handle_settings_dropdown_wheel(const short delta)
    {
        if (settings_dropdown_ == SettingsDropdown::none) {
            return;
        }
        constexpr std::size_t visible_options = 4U;
        const std::size_t count = settings_dropdown_option_count(
            settings_dropdown_);
        const std::size_t maximum_scroll = count > visible_options
            ? count - visible_options
            : 0U;
        if (delta > 0 && settings_dropdown_scroll_ > 0U) {
            --settings_dropdown_scroll_;
        } else if (delta < 0 && settings_dropdown_scroll_ < maximum_scroll) {
            ++settings_dropdown_scroll_;
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void handle_settings_mouse_wheel(const short delta)
    {
        if (settings_dropdown_ != SettingsDropdown::none) {
            handle_settings_dropdown_wheel(delta);
            return;
        }
        const float next =
            settings_scroll_offset_ -
                static_cast<float>(delta) * 64.0F /
                    static_cast<float>(WHEEL_DELTA);
        scroll_settings_to(next);
    }

    void update_settings_dropdown_hover(const D2D1_POINT_2F point)
    {
        for (const auto& hit : settings_dropdown_hits_) {
            if (contains(hit.rectangle, point) &&
                settings_dropdown_highlight_ != hit.option_index) {
                settings_dropdown_highlight_ = hit.option_index;
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
        }
    }

    void toggle_exclusive_output()
    {
        ui::toggle_exclusive_output(user_settings_);
        persist_user_settings();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void toggle_shared_fallback()
    {
        if (!ui::toggle_shared_fallback(user_settings_)) {
            return;
        }
        persist_user_settings();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void draw_settings_dropdown_field(
        const D2D1_RECT_F rectangle,
        const std::wstring& label,
        const bool open)
    {
        const auto field = D2D1::RoundedRect(rectangle, 10.0F, 10.0F);
        render_target_->FillRoundedRectangle(field, elevated_brush_.Get());
        render_target_->DrawRoundedRectangle(
            field, open ? accent_brush_.Get() : border_brush_.Get(),
            open ? 2.0F : 1.0F);
        draw_text(
            label,
            body_format_.Get(),
            D2D1::RectF(
                rectangle.left + 14.0F,
                rectangle.top,
                rectangle.right - 42.0F,
                rectangle.bottom),
            text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const float center_x = rectangle.right - 20.0F;
        const float center_y = (rectangle.top + rectangle.bottom) * 0.5F;
        const float direction = open ? -1.0F : 1.0F;
        render_target_->DrawLine(
            D2D1::Point2F(center_x - 5.0F, center_y - 2.0F * direction),
            D2D1::Point2F(center_x, center_y + 3.0F * direction),
            secondary_brush_.Get(),
            1.5F);
        render_target_->DrawLine(
            D2D1::Point2F(center_x, center_y + 3.0F * direction),
            D2D1::Point2F(center_x + 5.0F, center_y - 2.0F * direction),
            secondary_brush_.Get(),
            1.5F);
    }

    void draw_settings_dropdown_popup()
    {
        settings_dropdown_hits_.clear();
        if (settings_dropdown_ == SettingsDropdown::none) {
            return;
        }
        constexpr std::size_t maximum_visible = 4U;
        constexpr float item_height = 32.0F;
        const std::size_t count = settings_dropdown_option_count(
            settings_dropdown_);
        const std::size_t first = std::min(settings_dropdown_scroll_, count);
        const std::size_t visible = std::min(maximum_visible, count - first);
        if (visible == 0U) {
            return;
        }
        const D2D1_RECT_F anchor = settings_dropdown_anchor(settings_dropdown_);
        const D2D1_RECT_F popup_rect = D2D1::RectF(
            anchor.left,
            anchor.bottom + 6.0F,
            anchor.right,
            anchor.bottom + 14.0F +
                static_cast<float>(visible) * item_height);
        const auto popup = D2D1::RoundedRect(popup_rect, 10.0F, 10.0F);
        render_target_->FillRoundedRectangle(popup, surface_brush_.Get());
        render_target_->DrawRoundedRectangle(popup, border_brush_.Get(), 1.0F);

        const std::size_t selected = current_settings_dropdown_option(
            settings_dropdown_);
        for (std::size_t local = 0; local < visible; ++local) {
            const std::size_t index = first + local;
            const float top = popup_rect.top + 4.0F +
                static_cast<float>(local) * item_height;
            const D2D1_RECT_F item_rect = D2D1::RectF(
                popup_rect.left + 4.0F,
                top,
                popup_rect.right - 4.0F,
                top + item_height);
            if (index == settings_dropdown_highlight_) {
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(item_rect, 7.0F, 7.0F),
                    selection_brush_.Get());
            }
            draw_text(
                settings_dropdown_option_label(settings_dropdown_, index),
                small_format_.Get(),
                D2D1::RectF(
                    item_rect.left + 10.0F,
                    item_rect.top,
                    item_rect.right - 32.0F,
                    item_rect.bottom),
                index == settings_dropdown_highlight_
                    ? text_brush_.Get()
                    : secondary_brush_.Get(),
                DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (index == selected) {
                draw_text(
                    L"✓",
                    caption_format_.Get(),
                    D2D1::RectF(
                        item_rect.right - 28.0F,
                        item_rect.top,
                        item_rect.right - 8.0F,
                        item_rect.bottom),
                    accent_brush_.Get(),
                    DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            settings_dropdown_hits_.push_back({item_rect, index});
        }
    }

    void update_settings_control_bounds()
    {
        if (settings_token_edit_ == nullptr) {
            return;
        }
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0F;
        const auto to_pixel = [scale](const float value) {
            return static_cast<int>(std::lround(value * scale));
        };
        const auto position_edit = [&](const HWND edit, const D2D1_RECT_F rectangle) {
            if (edit == nullptr) {
                return;
            }
            const bool visible = settings_controls_ready_ &&
                rectangle.top >= kSettingsContentTop &&
                rectangle.bottom <= layout_.height - 24.0F;
            const int left = to_pixel(rectangle.left + 13.0F);
            const int top = to_pixel(rectangle.top + 8.0F);
            const int width = to_pixel(rectangle.right - rectangle.left - 26.0F);
            const int height = to_pixel(rectangle.bottom - rectangle.top - 16.0F);
            RECT current{};
            GetWindowRect(edit, &current);
            MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&current), 2);
            const bool bounds_changed =
                current.left != left || current.top != top ||
                current.right - current.left != width ||
                current.bottom - current.top != height;
            if (bounds_changed) {
                SetWindowPos(
                    edit,
                    nullptr,
                    left,
                    top,
                    width,
                    height,
                    SWP_NOACTIVATE | SWP_NOZORDER);
            }
            const bool visibility_changed =
                (IsWindowVisible(edit) != FALSE) != visible;
            if (visibility_changed) {
                ShowWindow(edit, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            }
            if (visible && (bounds_changed || visibility_changed)) {
                static_cast<void>(RedrawWindow(
                    edit,
                    nullptr,
                    nullptr,
                    RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW));
            }
        };
        position_edit(settings_token_edit_, layout_.settings_edit);
        position_edit(settings_cddb_server_edit_, layout_.settings_cddb_server_edit);
        position_edit(settings_cddb_email_edit_, layout_.settings_cddb_email_edit);
    }

    void draw_toggle(const D2D1_RECT_F rectangle, const bool enabled)
    {
        detail::draw_toggle(
            render_target_.Get(), rectangle, enabled,
            accent_brush_.Get(), elevated_brush_.Get(), border_brush_.Get(),
            accent_text_brush_.Get(), secondary_brush_.Get());
    }

    [[nodiscard]] ScrollbarGeometry settings_scrollbar_geometry() const noexcept
    {
        ScrollbarGeometry geometry;
        const float maximum = settings_maximum_scroll(layout_.height);
        if (maximum <= 0.0F) {
            return geometry;
        }
        geometry.visible = true;
        geometry.track = D2D1::RectF(
            layout_.settings_page.right + 8.0F,
            148.0F,
            layout_.settings_page.right + 12.0F,
            std::max(149.0F, layout_.height - 24.0F));
        const float track_height = geometry.track.bottom - geometry.track.top;
        constexpr float content_height = kSettingsContentBottom - 148.0F;
        const float thumb_height = std::clamp(
            track_height * track_height / content_height,
            48.0F,
            track_height);
        const float travel = std::max(0.0F, track_height - thumb_height);
        const float thumb_top = geometry.track.top +
            travel * settings_scroll_offset_ / maximum;
        geometry.thumb = D2D1::RectF(
            geometry.track.left - 2.0F,
            thumb_top,
            geometry.track.right + 2.0F,
            thumb_top + thumb_height);
        return geometry;
    }

    void draw_settings_scrollbar()
    {
        const ScrollbarGeometry geometry = settings_scrollbar_geometry();
        if (!geometry.visible) {
            return;
        }
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(geometry.track, 2.0F, 2.0F),
            border_brush_.Get());
        render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(geometry.thumb, 4.0F, 4.0F),
            muted_brush_.Get());
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
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const auto diagnostic_export = D2D1::RoundedRect(
            layout_.settings_diagnostics_export, 10.0F, 10.0F);
        render_target_->FillRoundedRectangle(diagnostic_export, surface_brush_.Get());
        render_target_->DrawRoundedRectangle(
            diagnostic_export, border_brush_.Get(), 1.0F);
        draw_text(
            L"导出脱敏诊断 (G)",
            button_format_.Get(),
            layout_.settings_diagnostics_export,
            text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        render_target_->PushAxisAlignedClip(
            D2D1::RectF(
                0.0F,
                kSettingsContentTop,
                layout_.width,
                layout_.height),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const auto audio_card = D2D1::RoundedRect(
            layout_.settings_audio_card,
            16.0F,
            16.0F);
        render_target_->FillRoundedRectangle(audio_card, surface_brush_.Get());
        render_target_->DrawRoundedRectangle(audio_card, border_brush_.Get(), 1.0F);
        draw_text(
            L"音频输出",
            heading_format_.Get(),
            D2D1::RectF(
                audio_card.rect.left + 24.0F,
                audio_card.rect.top + 18.0F,
                audio_card.rect.right - 24.0F,
                audio_card.rect.top + 46.0F),
            text_brush_.Get());
        draw_text(
            L"音频引擎",
            caption_format_.Get(),
            D2D1::RectF(
                layout_.settings_audio_engine.left,
                audio_card.rect.top + 54.0F,
                layout_.settings_audio_engine.right,
                audio_card.rect.top + 74.0F),
            muted_brush_.Get());
        draw_text(
            L"输出设备",
            caption_format_.Get(),
            D2D1::RectF(
                layout_.settings_audio_endpoint.left,
                audio_card.rect.top + 54.0F,
                layout_.settings_audio_endpoint.right,
                audio_card.rect.top + 74.0F),
            muted_brush_.Get());
        draw_settings_dropdown_field(
            layout_.settings_audio_engine,
            std::wstring(audio_output_engine_label(
                user_settings_.audio_output_engine)),
            settings_dropdown_ == SettingsDropdown::audio_engine);
        const std::wstring endpoint_name = audio_endpoints_.empty()
            ? L"系统默认设备"
            : audio_endpoints_[std::min(
                  selected_audio_endpoint_,
                  audio_endpoints_.size() - 1U)].name;
        draw_settings_dropdown_field(
            layout_.settings_audio_endpoint,
            endpoint_name,
            settings_dropdown_ == SettingsDropdown::audio_endpoint);
        draw_text(
            user_settings_.audio_exclusive_mode ? L"独占模式 (X)" : L"共享模式 (X)",
            small_format_.Get(),
            D2D1::RectF(
                audio_card.rect.right - 220.0F,
                audio_card.rect.top + 60.0F,
                audio_card.rect.right - 88.0F,
                audio_card.rect.top + 84.0F),
            secondary_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_TRAILING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        draw_toggle(
            layout_.settings_audio_exclusive_toggle,
            user_settings_.audio_exclusive_mode);
        draw_text(
            L"失败后回退共享 (F)",
            small_format_.Get(),
            D2D1::RectF(
                audio_card.rect.right - 230.0F,
                audio_card.rect.top + 102.0F,
                audio_card.rect.right - 88.0F,
                audio_card.rect.top + 126.0F),
            user_settings_.audio_exclusive_mode
                ? secondary_brush_.Get()
                : muted_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_TRAILING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        draw_toggle(
            layout_.settings_audio_fallback_toggle,
            user_settings_.audio_exclusive_mode &&
                user_settings_.audio_allow_shared_fallback);
        draw_text(
            audio_endpoint_status_ < 0
                ? platform::windows::describe_wasapi_status(audio_endpoint_status_)
                : L"44.1 kHz / 16 位 / 双声道；设备选择将在下一播放会话生效",
            caption_format_.Get(),
            D2D1::RectF(
                audio_card.rect.left + 24.0F,
                audio_card.rect.bottom - 28.0F,
                audio_card.rect.right - 24.0F,
                audio_card.rect.bottom - 6.0F),
            audio_endpoint_status_ < 0 ? error_brush_.Get() : muted_brush_.Get());

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
                listenbrainz_card.rect.top + 18.0F,
                listenbrainz_card.rect.right - 200.0F,
                listenbrainz_card.rect.top + 46.0F),
            text_brush_.Get());
        draw_text(
            L"将正在播放状态和达到阈值的播放记录同步到个人账户",
            small_format_.Get(),
            D2D1::RectF(
                listenbrainz_card.rect.left + 24.0F,
                listenbrainz_card.rect.top + 52.0F,
                listenbrainz_card.rect.right - 24.0F,
                listenbrainz_card.rect.top + 76.0F),
            secondary_brush_.Get());
        const auto listenbrainz_status = listenbrainz_reporter_.status();
        draw_text(
            platform::windows::to_string(listenbrainz_status.state),
            small_format_.Get(),
            D2D1::RectF(
                listenbrainz_card.rect.right - 180.0F,
                listenbrainz_card.rect.top + 18.0F,
                listenbrainz_card.rect.right - 92.0F,
                listenbrainz_card.rect.top + 46.0F),
            listenbrainz_status.state == platform::windows::ListenBrainzState::ready
                ? success_brush_.Get()
                : (listenbrainz_status.state ==
                           platform::windows::ListenBrainzState::unauthorized ||
                       listenbrainz_status.state ==
                           platform::windows::ListenBrainzState::error
                    ? error_brush_.Get()
                    : muted_brush_.Get()),
            DWRITE_TEXT_ALIGNMENT_TRAILING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        draw_toggle(
            layout_.settings_listenbrainz_toggle,
            user_settings_.listenbrainz_reporting_enabled);
        const auto queue_clear = D2D1::RoundedRect(
            layout_.settings_queue_clear,
            9.0F,
            9.0F);
        render_target_->FillRoundedRectangle(queue_clear, elevated_brush_.Get());
        render_target_->DrawRoundedRectangle(queue_clear, border_brush_.Get(), 1.0F);
        draw_text(
            L"清理当前账户待同步队列",
            small_format_.Get(),
            layout_.settings_queue_clear,
            text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const auto save = D2D1::RoundedRect(layout_.settings_save, 10.0F, 10.0F);
        render_target_->FillRoundedRectangle(save, accent_brush_.Get());
        draw_text(
            L"保存",
            button_format_.Get(),
            layout_.settings_save,
            accent_text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        draw_settings_dropdown_popup();

        const auto cddb_card = D2D1::RoundedRect(
            layout_.settings_cddb_card,
            16.0F,
            16.0F);
        render_target_->FillRoundedRectangle(cddb_card, surface_brush_.Get());
        render_target_->DrawRoundedRectangle(cddb_card, border_brush_.Get(), 1.0F);
        draw_text(
            L"CDDB / freedb",
            heading_format_.Get(),
            D2D1::RectF(
                cddb_card.rect.left + 24.0F,
                cddb_card.rect.top + 18.0F,
                cddb_card.rect.right - 120.0F,
                cddb_card.rect.top + 46.0F),
            text_brush_.Get());
        draw_text(
            cddb_settings_error_.empty()
                ? L"默认使用 GnuDB HTTPS；邮箱留空时使用项目开发者身份"
                : cddb_settings_error_,
            small_format_.Get(),
            D2D1::RectF(
                cddb_card.rect.left + 24.0F,
                cddb_card.rect.top + 52.0F,
                cddb_card.rect.right - 96.0F,
                cddb_card.rect.top + 76.0F),
            cddb_settings_error_.empty()
                ? secondary_brush_.Get()
                : error_brush_.Get());
        draw_toggle(layout_.settings_cddb_toggle, user_settings_.cddb_enabled);
        draw_text(
            L"服务器",
            caption_format_.Get(),
            D2D1::RectF(
                layout_.settings_cddb_server_edit.left,
                layout_.settings_cddb_server_edit.top - 18.0F,
                layout_.settings_cddb_server_edit.right,
                layout_.settings_cddb_server_edit.top - 4.0F),
            muted_brush_.Get());
        draw_text(
            L"身份邮箱（查询可选，提交时必填）",
            caption_format_.Get(),
            D2D1::RectF(
                layout_.settings_cddb_email_edit.left,
                layout_.settings_cddb_email_edit.top - 18.0F,
                layout_.settings_cddb_email_edit.right,
                layout_.settings_cddb_email_edit.top - 4.0F),
            muted_brush_.Get());
        for (const D2D1_RECT_F rectangle : {
                 layout_.settings_cddb_server_edit,
                 layout_.settings_cddb_email_edit,
             }) {
            const auto field = D2D1::RoundedRect(rectangle, 10.0F, 10.0F);
            render_target_->FillRoundedRectangle(field, elevated_brush_.Get());
            render_target_->DrawRoundedRectangle(
                field,
                cddb_settings_error_.empty() ? border_brush_.Get() : error_brush_.Get(),
                cddb_settings_error_.empty() ? 1.0F : 2.0F);
        }
        const auto cddb_save = D2D1::RoundedRect(
            layout_.settings_cddb_save,
            10.0F,
            10.0F);
        render_target_->FillRoundedRectangle(cddb_save, accent_brush_.Get());
        draw_text(
            L"保存",
            button_format_.Get(),
            layout_.settings_cddb_save,
            accent_text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);
        render_target_->PopAxisAlignedClip();
        draw_settings_scrollbar();
    }

    [[nodiscard]] bool is_metadata_edit(const HWND window) const noexcept
    {
        return window != nullptr &&
            (window == metadata_album_title_edit_ ||
             window == metadata_album_artist_edit_ ||
             window == metadata_category_edit_ ||
             window == metadata_year_edit_ ||
             window == metadata_track_title_edit_ ||
             window == metadata_track_artist_edit_);
    }

    [[nodiscard]] static std::wstring edit_text(const HWND edit)
    {
        if (edit == nullptr) {
            return {};
        }
        const int length = GetWindowTextLengthW(edit);
        std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
        const int written = GetWindowTextW(edit, value.data(), length + 1);
        value.resize(static_cast<std::size_t>(std::max(written, 0)));
        const auto first = value.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) {
            return {};
        }
        const auto last = value.find_last_not_of(L" \t\r\n");
        return value.substr(first, last - first + 1U);
    }

    static LRESULT CALLBACK metadata_editor_window_proc(
        const HWND edit,
        const UINT message,
        const WPARAM wparam,
        const LPARAM lparam)
    {
        auto* self = reinterpret_cast<MainWindow*>(
            GetWindowLongPtrW(edit, GWLP_USERDATA));
        if (self != nullptr && message == WM_KEYDOWN && wparam == VK_ESCAPE) {
            PostMessageW(self->window_, WM_KEYDOWN, VK_ESCAPE, 0);
            return 0;
        }
        return self != nullptr && self->metadata_editor_edit_original_proc_ != nullptr
            ? CallWindowProcW(
                  self->metadata_editor_edit_original_proc_,
                  edit,
                  message,
                  wparam,
                  lparam)
            : DefWindowProcW(edit, message, wparam, lparam);
    }

    [[nodiscard]] HWND create_metadata_edit(
        const int identifier,
        const std::wstring& value,
        const WPARAM limit)
    {
        HWND edit = CreateWindowExW(
            0,
            L"EDIT",
            value.c_str(),
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
            0,
            0,
            1,
            1,
            window_,
            reinterpret_cast<HMENU>(static_cast<std::intptr_t>(identifier)),
            instance_,
            nullptr);
        if (edit == nullptr) {
            return nullptr;
        }
        static_cast<void>(SetWindowTheme(edit, L" ", L" "));
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(settings_font_), TRUE);
        SendMessageW(edit, EM_SETLIMITTEXT, limit, 0);
        SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        const auto original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            edit,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(metadata_editor_window_proc)));
        if (metadata_editor_edit_original_proc_ == nullptr) {
            metadata_editor_edit_original_proc_ = original;
        }
        return edit;
    }

    [[nodiscard]] bool metadata_editor_changed() const noexcept
    {
        const auto& left = metadata_editor_working_;
        const auto& right = metadata_editor_original_;
        return left.album_title != right.album_title ||
            left.album_artist != right.album_artist ||
            left.category != right.category || left.year != right.year ||
            left.track_titles != right.track_titles ||
            left.track_artists != right.track_artists;
    }

    void commit_metadata_editor_fields()
    {
        if (active_page_ != AppPage::metadata_editor) {
            return;
        }
        metadata_editor_working_.album_title = edit_text(metadata_album_title_edit_);
        metadata_editor_working_.album_artist = edit_text(metadata_album_artist_edit_);
        metadata_editor_working_.category = edit_text(metadata_category_edit_);
        metadata_editor_working_.year = edit_text(metadata_year_edit_);
        if (metadata_editor_track_ < metadata_editor_working_.track_titles.size()) {
            metadata_editor_working_.track_titles[metadata_editor_track_] =
                edit_text(metadata_track_title_edit_);
            metadata_editor_working_.track_artists[metadata_editor_track_] =
                edit_text(metadata_track_artist_edit_);
        }
    }

    void load_metadata_track_fields()
    {
        if (metadata_editor_track_ >= metadata_editor_working_.track_titles.size()) {
            return;
        }
        SetWindowTextW(
            metadata_track_title_edit_,
            metadata_editor_working_.track_titles[metadata_editor_track_].c_str());
        SetWindowTextW(
            metadata_track_artist_edit_,
            metadata_editor_working_.track_artists[metadata_editor_track_].c_str());
    }

    void open_metadata_editor()
    {
        if (!disc_.toc || disc_.tracks.empty()) {
            return;
        }
        if (active_page_ == AppPage::metadata_editor) {
            SetFocus(metadata_album_title_edit_);
            return;
        }
        active_page_ = AppPage::metadata_editor;
        if (settings_font_ == nullptr) {
            settings_font_ = CreateFontW(
                -MulDiv(14, static_cast<int>(GetDpiForWindow(window_)), 72),
                0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
        if (settings_edit_brush_ == nullptr) {
            settings_edit_brush_ = CreateSolidBrush(colorref(theme_.elevated));
        }
        metadata_editor_working_ = {
            disc_.album_title,
            disc_.album_artist,
            disc_.cddb_category.empty() ? L"misc" : disc_.cddb_category,
            disc_.cddb_year,
            {},
            {},
            disc_.cddb_revision,
        };
        if (!disc_.has_user_metadata &&
            std::find(
                disc_.metadata_sources.begin(),
                disc_.metadata_sources.end(),
                L"CDDB/freedb") != disc_.metadata_sources.end()) {
            ++metadata_editor_working_.revision;
        }
        metadata_editor_working_.track_titles.reserve(disc_.tracks.size());
        metadata_editor_working_.track_artists.reserve(disc_.tracks.size());
        for (const auto& track : disc_.tracks) {
            metadata_editor_working_.track_titles.push_back(track.title);
            metadata_editor_working_.track_artists.push_back(
                track.artist.empty() ? disc_.album_artist : track.artist);
        }
        metadata_editor_original_ = metadata_editor_working_;
        metadata_editor_has_saved_edits_ = disc_.has_user_metadata;
        metadata_editor_track_ = std::min(selected_track_, disc_.tracks.size() - 1U);
        metadata_editor_scroll_row_ = 0;
        metadata_editor_feedback_.clear();
        metadata_editor_feedback_error_ = false;

        metadata_album_title_edit_ = create_metadata_edit(
            kMetadataAlbumTitleEditId,
            metadata_editor_working_.album_title,
            512);
        metadata_album_artist_edit_ = create_metadata_edit(
            kMetadataAlbumArtistEditId,
            metadata_editor_working_.album_artist,
            512);
        metadata_category_edit_ = create_metadata_edit(
            kMetadataCategoryEditId,
            metadata_editor_working_.category,
            32);
        metadata_year_edit_ = create_metadata_edit(
            kMetadataYearEditId,
            metadata_editor_working_.year,
            16);
        metadata_track_title_edit_ = create_metadata_edit(
            kMetadataTrackTitleEditId,
            metadata_editor_working_.track_titles[metadata_editor_track_],
            512);
        metadata_track_artist_edit_ = create_metadata_edit(
            kMetadataTrackArtistEditId,
            metadata_editor_working_.track_artists[metadata_editor_track_],
            512);
        update_metadata_editor_bounds();
        static_cast<void>(RedrawWindow(
            window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
        for (const HWND edit : {
                 metadata_album_title_edit_,
                 metadata_album_artist_edit_,
                 metadata_category_edit_,
                 metadata_year_edit_,
                 metadata_track_title_edit_,
                 metadata_track_artist_edit_,
             }) {
            if (edit != nullptr) {
                ShowWindow(edit, SW_SHOWNOACTIVATE);
            }
        }
        SetFocus(metadata_album_title_edit_);
    }

    void close_metadata_editor(const bool confirm = true)
    {
        if (active_page_ == AppPage::metadata_editor) {
            commit_metadata_editor_fields();
            if (confirm && metadata_editor_changed() &&
                MessageBoxW(
                    window_,
                    L"尚未保存的元数据修改将被放弃。",
                    L"CD.404",
                    MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
                return;
            }
        }
        for (HWND* edit : {
                 &metadata_album_title_edit_,
                 &metadata_album_artist_edit_,
                 &metadata_category_edit_,
                 &metadata_year_edit_,
                 &metadata_track_title_edit_,
                 &metadata_track_artist_edit_,
             }) {
            if (*edit != nullptr) {
                DestroyWindow(*edit);
                *edit = nullptr;
            }
        }
        metadata_editor_edit_original_proc_ = nullptr;
        if (active_page_ == AppPage::metadata_editor) {
            active_page_ = AppPage::player;
            SetFocus(window_);
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void update_metadata_editor_bounds()
    {
        const float scale = static_cast<float>(GetDpiForWindow(window_)) / 96.0F;
        const auto to_pixel = [scale](const float value) {
            return static_cast<int>(std::lround(value * scale));
        };
        const auto position = [&](const HWND edit, const D2D1_RECT_F rectangle) {
            if (edit == nullptr) {
                return;
            }
            SetWindowPos(
                edit,
                HWND_TOP,
                to_pixel(rectangle.left + 13.0F),
                to_pixel(rectangle.top + 8.0F),
                to_pixel(rectangle.right - rectangle.left - 26.0F),
                to_pixel(rectangle.bottom - rectangle.top - 16.0F),
                SWP_NOACTIVATE);
        };
        position(metadata_album_title_edit_, layout_.metadata_album_title_edit);
        position(metadata_album_artist_edit_, layout_.metadata_album_artist_edit);
        position(metadata_category_edit_, layout_.metadata_category_edit);
        position(metadata_year_edit_, layout_.metadata_year_edit);
        position(metadata_track_title_edit_, layout_.metadata_track_title_edit);
        position(metadata_track_artist_edit_, layout_.metadata_track_artist_edit);
    }

    [[nodiscard]] disc::GnudbSubmissionMetadataUtf8 metadata_submission() const
    {
        disc::GnudbSubmissionMetadataUtf8 result;
        result.album_title = to_utf8(metadata_editor_working_.album_title);
        result.album_artist = to_utf8(metadata_editor_working_.album_artist);
        result.category = to_utf8(metadata_editor_working_.category);
        result.year = to_utf8(metadata_editor_working_.year);
        result.revision = metadata_editor_working_.revision;
        result.user_edited = metadata_editor_has_saved_edits_ || metadata_editor_changed();
        result.track_titles.reserve(metadata_editor_working_.track_titles.size());
        result.track_artists.reserve(metadata_editor_working_.track_artists.size());
        for (const auto& title : metadata_editor_working_.track_titles) {
            result.track_titles.push_back(to_utf8(title));
        }
        for (const auto& artist : metadata_editor_working_.track_artists) {
            result.track_artists.push_back(to_utf8(artist));
        }
        return result;
    }

    [[nodiscard]] bool save_metadata_locally()
    {
        commit_metadata_editor_fields();
        if (!disc_.toc || current_disc_key_.empty()) {
            return false;
        }
        if (metadata_editor_working_.album_title.empty() ||
            metadata_editor_working_.album_artist.empty() ||
            metadata_editor_working_.track_titles.size() != disc_.tracks.size() ||
            std::ranges::any_of(
                metadata_editor_working_.track_titles,
                [](const std::wstring& title) { return title.empty(); }) ||
            !disc::is_valid_gnudb_category(
                to_utf8(metadata_editor_working_.category))) {
            metadata_editor_feedback_ =
                L"请填写专辑名、艺术家、有效分类和所有曲名";
            metadata_editor_feedback_error_ = true;
            InvalidateRect(window_, nullptr, FALSE);
            return false;
        }
        user_settings_.metadata_overrides[current_disc_key_] = metadata_editor_working_;
        if (!platform::windows::save_user_settings(user_settings_)) {
            metadata_editor_feedback_ = L"无法保存本地元数据";
            metadata_editor_feedback_error_ = true;
            InvalidateRect(window_, nullptr, FALSE);
            return false;
        }
        metadata_editor_has_saved_edits_ = true;
        metadata_editor_original_ = metadata_editor_working_;
        apply_saved_metadata_override();
        metadata_editor_feedback_ = L"已保存到本机，不会被在线元数据覆盖";
        metadata_editor_feedback_error_ = false;
        sync_system_media(true);
        if (listenbrainz_tracker_.active() && selected_track_ < disc_.tracks.size()) {
            listenbrainz_tracker_.update(
                listen_metadata(selected_track_),
                playback_track_frame_,
                unix_time_now());
        }
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    void start_cddb_submission(const platform::windows::CddbSubmissionMode mode)
    {
        if (cddb_submission_worker_.joinable() || !save_metadata_locally()) {
            return;
        }
        if (!user_settings_.cddb_enabled) {
            metadata_editor_feedback_ = L"CDDB/freedb 已在设置中关闭";
            metadata_editor_feedback_error_ = true;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (!platform::windows::is_valid_cddb_email(user_settings_.cddb_email)) {
            metadata_editor_feedback_ = L"请先在设置中填写有效的 CDDB 提交邮箱";
            metadata_editor_feedback_error_ = true;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (mode == platform::windows::CddbSubmissionMode::submit &&
            MessageBoxW(
                window_,
                L"这会将当前整张光盘的编辑结果和提交邮箱发送到配置的公共 CDDB/freedb 服务器。是否继续？",
                L"正式上传 CDDB 元数据",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
        const platform::windows::CddbClientConfiguration configuration{
            user_settings_.cddb_server,
            user_settings_.cddb_email,
        };
        const auto submission = metadata_submission();
        disc::GnudbSubmissionError submission_error{};
        if (!disc::make_gnudb_submission_entry(
                *disc_.toc,
                submission,
                submission_error)) {
            metadata_editor_feedback_ =
                L"上传要求替换所有“音轨 01 / Track 01”占位曲名";
            metadata_editor_feedback_error_ = true;
            InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        const disc::Toc toc = *disc_.toc;
        const HWND target_window = window_;
        metadata_editor_feedback_ = mode == platform::windows::CddbSubmissionMode::test
            ? L"正在请服务器校验…"
            : L"正在上传…";
        metadata_editor_feedback_error_ = false;
        cddb_submission_worker_ = std::jthread([
            target_window,
            configuration,
            submission,
            toc,
            mode] {
            auto snapshot = std::make_unique<CddbSubmissionSnapshot>();
            snapshot->mode = mode;
            snapshot->result = platform::windows::submit_gnudb(
                configuration,
                submission,
                toc,
                mode);
            auto* raw = snapshot.release();
            if (PostMessageW(
                    target_window,
                    kCddbSubmissionReadyMessage,
                    0,
                    reinterpret_cast<LPARAM>(raw)) == FALSE) {
                delete raw;
            }
        });
        InvalidateRect(window_, nullptr, FALSE);
    }

    void receive_cddb_submission(std::unique_ptr<CddbSubmissionSnapshot> snapshot)
    {
        if (cddb_submission_worker_.joinable()) {
            cddb_submission_worker_.join();
        }
        std::wstring response = snapshot->result.message;
        std::replace(response.begin(), response.end(), L'\r', L' ');
        std::replace(response.begin(), response.end(), L'\n', L' ');
        if (response.size() > 160U) {
            response.resize(160U);
        }
        if (snapshot->result.accepted) {
            metadata_editor_feedback_ =
                snapshot->mode == platform::windows::CddbSubmissionMode::test
                ? L"服务器校验通过，未写入公共数据库"
                : L"服务器已接收正式提交";
            if (snapshot->mode == platform::windows::CddbSubmissionMode::submit) {
                ++metadata_editor_working_.revision;
                metadata_editor_original_.revision = metadata_editor_working_.revision;
                if (!current_disc_key_.empty()) {
                    user_settings_.metadata_overrides[current_disc_key_].revision =
                        metadata_editor_working_.revision;
                    persist_user_settings();
                }
            }
            metadata_editor_feedback_error_ = false;
        } else {
            metadata_editor_feedback_ = response.empty()
                ? std::format(
                      L"CDDB 请求失败（HTTP {}，系统 {}）",
                      snapshot->result.http_status,
                      snapshot->result.system_error)
                : response;
            metadata_editor_feedback_error_ = true;
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void select_metadata_track(const int direction)
    {
        commit_metadata_editor_fields();
        if (metadata_editor_working_.track_titles.empty()) {
            return;
        }
        const auto next = std::clamp<std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(metadata_editor_track_) + direction,
            0,
            static_cast<std::ptrdiff_t>(metadata_editor_working_.track_titles.size() - 1U));
        metadata_editor_track_ = static_cast<std::size_t>(next);
        load_metadata_track_fields();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void handle_metadata_mouse_wheel(const short delta)
    {
        const std::size_t visible = static_cast<std::size_t>(std::max(
            1.0F,
            std::floor((layout_.metadata_track_list.bottom -
                        layout_.metadata_track_list.top - 28.0F) / 36.0F)));
        const std::size_t maximum = disc_.tracks.size() > visible
            ? disc_.tracks.size() - visible
            : 0U;
        if (delta > 0) {
            metadata_editor_scroll_row_ = metadata_editor_scroll_row_ > 2U
                ? metadata_editor_scroll_row_ - 3U
                : 0U;
        } else {
            metadata_editor_scroll_row_ = std::min(
                maximum,
                metadata_editor_scroll_row_ + 3U);
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void handle_metadata_click(const D2D1_POINT_2F point)
    {
        if (contains(layout_.metadata_back, point)) {
            close_metadata_editor();
            return;
        }
        if (contains(layout_.metadata_save, point)) {
            static_cast<void>(save_metadata_locally());
            return;
        }
        if (contains(layout_.metadata_test_submit, point)) {
            start_cddb_submission(platform::windows::CddbSubmissionMode::test);
            return;
        }
        if (contains(layout_.metadata_submit, point)) {
            start_cddb_submission(platform::windows::CddbSubmissionMode::submit);
            return;
        }
        for (const auto& hit : metadata_track_hits_) {
            if (contains(hit.rectangle, point)) {
                commit_metadata_editor_fields();
                metadata_editor_track_ = hit.track_index;
                load_metadata_track_fields();
                InvalidateRect(window_, nullptr, FALSE);
                return;
            }
        }
    }

    void draw_metadata_editor_page()
    {
        draw_text(
            L"编辑元数据",
            heading_format_.Get(),
            D2D1::RectF(
                layout_.metadata_page.left,
                84.0F,
                layout_.metadata_back.left - 20.0F,
                116.0F),
            text_brush_.Get());
        draw_text(
            L"本地修改优先级最高；上传始终需要用户明确操作",
            small_format_.Get(),
            D2D1::RectF(
                layout_.metadata_page.left,
                116.0F,
                layout_.metadata_back.left - 20.0F,
                140.0F),
            secondary_brush_.Get());
        const auto back = D2D1::RoundedRect(layout_.metadata_back, 10.0F, 10.0F);
        render_target_->FillRoundedRectangle(back, surface_brush_.Get());
        render_target_->DrawRoundedRectangle(back, border_brush_.Get(), 1.0F);
        draw_text(
            L"←  返回播放器",
            button_format_.Get(),
            layout_.metadata_back,
            text_brush_.Get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);

        const auto draw_field = [&](const D2D1_RECT_F rectangle, const wchar_t* label) {
            draw_text(
                label,
                caption_format_.Get(),
                D2D1::RectF(
                    rectangle.left,
                    rectangle.top - 18.0F,
                    rectangle.right,
                    rectangle.top),
                muted_brush_.Get());
            const auto field = D2D1::RoundedRect(rectangle, 10.0F, 10.0F);
            render_target_->FillRoundedRectangle(field, elevated_brush_.Get());
            render_target_->DrawRoundedRectangle(field, border_brush_.Get(), 1.0F);
        };
        draw_field(layout_.metadata_album_title_edit, L"专辑名");
        draw_field(layout_.metadata_album_artist_edit, L"专辑艺术家");
        draw_field(
            layout_.metadata_category_edit,
            L"CDDB 分类（misc / rock / classical / soundtrack 等）");
        draw_field(layout_.metadata_year_edit, L"年份");
        draw_field(
            layout_.metadata_track_title_edit,
            std::format(L"{:02} 曲名", metadata_editor_track_ + 1U).c_str());
        draw_field(layout_.metadata_track_artist_edit, L"曲目艺术家");

        const auto list = D2D1::RoundedRect(layout_.metadata_track_list, 12.0F, 12.0F);
        render_target_->FillRoundedRectangle(list, surface_brush_.Get());
        render_target_->DrawRoundedRectangle(list, border_brush_.Get(), 1.0F);
        draw_text(
            L"#",
            caption_format_.Get(),
            D2D1::RectF(
                list.rect.left + 12.0F,
                list.rect.top + 5.0F,
                list.rect.left + 50.0F,
                list.rect.top + 27.0F),
            muted_brush_.Get());
        draw_text(
            L"曲名 / 艺术家",
            caption_format_.Get(),
            D2D1::RectF(
                list.rect.left + 54.0F,
                list.rect.top + 5.0F,
                list.rect.right - 16.0F,
                list.rect.top + 27.0F),
            muted_brush_.Get());
        const float row_height = 36.0F;
        const float rows_top = list.rect.top + 28.0F;
        const std::size_t visible = static_cast<std::size_t>(std::max(
            1.0F,
            std::floor((list.rect.bottom - rows_top) / row_height)));
        const std::size_t maximum = disc_.tracks.size() > visible
            ? disc_.tracks.size() - visible
            : 0U;
        metadata_editor_scroll_row_ = std::min(metadata_editor_scroll_row_, maximum);
        metadata_track_hits_.clear();
        render_target_->PushAxisAlignedClip(
            D2D1::RectF(list.rect.left, rows_top, list.rect.right, list.rect.bottom),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const std::size_t end = std::min(
            disc_.tracks.size(), metadata_editor_scroll_row_ + visible);
        for (std::size_t index = metadata_editor_scroll_row_; index < end; ++index) {
            const float top = rows_top +
                static_cast<float>(index - metadata_editor_scroll_row_) * row_height;
            const D2D1_RECT_F row = D2D1::RectF(
                list.rect.left + 4.0F,
                top,
                list.rect.right - 4.0F,
                top + row_height);
            if (index == metadata_editor_track_) {
                render_target_->FillRoundedRectangle(
                    D2D1::RoundedRect(row, 8.0F, 8.0F),
                    selection_brush_.Get());
            }
            metadata_track_hits_.push_back({row, index});
            draw_text(
                std::format(L"{:02}", disc_.tracks[index].number),
                caption_format_.Get(),
                D2D1::RectF(row.left + 8.0F, row.top + 8.0F, row.left + 48.0F, row.bottom),
                muted_brush_.Get());
            std::wstring summary = metadata_editor_working_.track_titles[index];
            const auto& artist = metadata_editor_working_.track_artists[index];
            if (!artist.empty()) {
                summary += L"  ·  " + artist;
            }
            draw_text(
                summary,
                small_format_.Get(),
                D2D1::RectF(row.left + 54.0F, row.top + 6.0F, row.right - 12.0F, row.bottom),
                index == metadata_editor_track_ ? text_brush_.Get() : secondary_brush_.Get());
        }
        render_target_->PopAxisAlignedClip();
        if (maximum != 0U) {
            const float track_top = rows_top + 4.0F;
            const float track_bottom = list.rect.bottom - 4.0F;
            const float thumb_height = std::max(
                24.0F,
                (track_bottom - track_top) * static_cast<float>(visible) /
                    static_cast<float>(disc_.tracks.size()));
            const float thumb_top = track_top +
                (track_bottom - track_top - thumb_height) *
                    static_cast<float>(metadata_editor_scroll_row_) /
                    static_cast<float>(maximum);
            render_target_->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(list.rect.right - 8.0F, thumb_top,
                                list.rect.right - 4.0F, thumb_top + thumb_height),
                    2.0F,
                    2.0F),
                muted_brush_.Get());
        }

        const auto button = [&](
            const D2D1_RECT_F rectangle,
            const wchar_t* label,
            const bool primary,
            const bool enabled) {
            const auto rounded = D2D1::RoundedRect(rectangle, 10.0F, 10.0F);
            render_target_->FillRoundedRectangle(
                rounded,
                primary && enabled ? accent_brush_.Get() : elevated_brush_.Get());
            render_target_->DrawRoundedRectangle(rounded, border_brush_.Get(), 1.0F);
            draw_text(
                label,
                button_format_.Get(),
                rectangle,
                enabled
                    ? (primary ? accent_text_brush_.Get() : text_brush_.Get())
                    : muted_brush_.Get(),
                DWRITE_TEXT_ALIGNMENT_CENTER);
        };
        const bool submission_idle = !cddb_submission_worker_.joinable();
        button(layout_.metadata_save, L"保存到本机", false, true);
        button(
            layout_.metadata_test_submit,
            L"测试上传",
            false,
            submission_idle && user_settings_.cddb_enabled);
        button(
            layout_.metadata_submit,
            L"正式上传",
            true,
            submission_idle && user_settings_.cddb_enabled);
        if (!metadata_editor_feedback_.empty()) {
            draw_text(
                metadata_editor_feedback_,
                caption_format_.Get(),
                D2D1::RectF(
                    layout_.metadata_save.right + 16.0F,
                    layout_.metadata_save.top + 4.0F,
                    layout_.metadata_test_submit.left - 16.0F,
                    layout_.metadata_save.bottom),
                metadata_editor_feedback_error_
                    ? error_brush_.Get()
                    : success_brush_.Get(),
                DWRITE_TEXT_ALIGNMENT_CENTER);
        }
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
    HWND resize_focus_{};
    bool window_resizing_{};
    ThemePalette theme_{};
    std::wstring accessible_name_;
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
    platform::windows::DiagnosticLog diagnostics_;
    std::vector<platform::windows::WasapiEndpoint> audio_endpoints_;
    std::size_t selected_audio_endpoint_{};
    std::int32_t audio_endpoint_status_{};
    float volume_{1.0F};
    std::wstring current_disc_key_;
    std::wstring preferred_metadata_release_id_;
    unsigned int last_persisted_track_number_{};
    core::SampleFrame last_persisted_frame_{-1};
    ULONGLONG last_position_save_tick_{};
    AppPage active_page_{AppPage::player};
    bool settings_save_failed_{};
    bool settings_saved_{};
    bool settings_input_required_{};
    bool settings_controls_ready_{};
    bool settings_scroll_active_{};
    float settings_scroll_offset_{};
    std::size_t settings_token_character_count_{};
    bool settings_token_mask_active_{};
    HWND settings_token_edit_{};
    SettingsDropdown settings_dropdown_{SettingsDropdown::none};
    std::size_t settings_dropdown_scroll_{};
    std::size_t settings_dropdown_highlight_{};
    std::vector<SettingsDropdownHit> settings_dropdown_hits_;
    HWND settings_cddb_server_edit_{};
    HWND settings_cddb_email_edit_{};
    std::wstring cddb_settings_error_;
    WNDPROC settings_edit_original_proc_{};
    HWND metadata_album_title_edit_{};
    HWND metadata_album_artist_edit_{};
    HWND metadata_category_edit_{};
    HWND metadata_year_edit_{};
    HWND metadata_track_title_edit_{};
    HWND metadata_track_artist_edit_{};
    WNDPROC metadata_editor_edit_original_proc_{};
    platform::windows::SavedDiscMetadata metadata_editor_working_;
    platform::windows::SavedDiscMetadata metadata_editor_original_;
    std::size_t metadata_editor_track_{};
    std::size_t metadata_editor_scroll_row_{};
    std::vector<TrackHit> metadata_track_hits_;
    bool metadata_editor_has_saved_edits_{};
    std::wstring metadata_editor_feedback_;
    bool metadata_editor_feedback_error_{};
    std::jthread cddb_submission_worker_;
    HFONT settings_font_{};
    HBRUSH settings_edit_brush_{};
    HWND metadata_edit_{};
    WNDPROC metadata_edit_original_proc_{};
    MetadataEditField metadata_edit_field_{MetadataEditField::none};
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
