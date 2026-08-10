#include "wizard.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <string>
#include <utility>

namespace cd404::installer {
namespace {

constexpr wchar_t kWindowClass[] = L"CD404InstallerWizard";
constexpr COLORREF kAccent = RGB(102, 126, 234);
constexpr int kBack = 1101;
constexpr int kNext = 1102;
constexpr int kCancel = 1103;
constexpr int kDesktop = 1104;
constexpr int kLaunch = 1105;
constexpr int kBrowse = 1106;
constexpr int kPurge = 1107;

enum class Mode { install, uninstall };

struct Fonts {
    HFONT title{};
    HFONT heading{};
    HFONT body{};
    HFONT caption{};
    HFONT button{};

    void reset()
    {
        for (HFONT font : {title, heading, body, caption, button}) {
            if (font != nullptr) {
                DeleteObject(font);
            }
        }
        title = nullptr;
        heading = nullptr;
        body = nullptr;
        caption = nullptr;
        button = nullptr;
    }

    ~Fonts() { reset(); }
};

struct Wizard {
    HINSTANCE instance{};
    HWND window{};
    HWND product{};
    HWND version{};
    HWND heading{};
    HWND body{};
    HWND path_label{};
    HWND path{};
    HWND browse{};
    HWND space{};
    HWND desktop{};
    HWND progress{};
    HWND status{};
    HWND launch{};
    HWND purge{};
    HWND back{};
    HWND next{};
    HWND cancel{};
    Fonts fonts;
    HBRUSH canvas_brush{};
    HBRUSH surface_brush{};
    COLORREF canvas{};
    COLORREF surface{};
    COLORREF ink{};
    COLORREF muted{};
    bool dark{};
    bool desktop_selected{};
    bool launch_selected{};
    bool purge_selected{};
    Mode mode{Mode::install};
    int page{};
    int exit_code{};
    UINT dpi{96U};
    InstallWizardConfig install_config;
    UninstallWizardConfig uninstall_config;

    ~Wizard()
    {
        if (canvas_brush != nullptr) DeleteObject(canvas_brush);
        if (surface_brush != nullptr) DeleteObject(surface_brush);
    }
};

[[nodiscard]] bool system_uses_dark_mode()
{
    std::array<wchar_t, 16> override_value{};
    const DWORD override_length = GetEnvironmentVariableW(
        L"CD404_INSTALLER_THEME", override_value.data(),
        static_cast<DWORD>(override_value.size()));
    if (override_length != 0U && override_length < override_value.size()) {
        if (_wcsicmp(override_value.data(), L"dark") == 0) return true;
        if (_wcsicmp(override_value.data(), L"light") == 0) return false;
    }
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast),
            &contrast, 0) != FALSE &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0U) {
        const COLORREF window = GetSysColor(COLOR_WINDOW);
        return GetRValue(window) + GetGValue(window) + GetBValue(window) < 384;
    }
    DWORD light{1U};
    DWORD bytes{sizeof(light)};
    static_cast<void>(RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes));
    return light == 0U;
}

void apply_theme(Wizard& state)
{
    state.dark = system_uses_dark_mode();
    state.canvas = state.dark ? RGB(24, 27, 34) : RGB(247, 248, 250);
    state.surface = state.dark ? RGB(31, 35, 43) : RGB(255, 255, 255);
    state.ink = state.dark ? RGB(240, 242, 247) : RGB(24, 28, 36);
    state.muted = state.dark ? RGB(168, 176, 191) : RGB(92, 101, 117);
    if (state.canvas_brush != nullptr) DeleteObject(state.canvas_brush);
    if (state.surface_brush != nullptr) DeleteObject(state.surface_brush);
    state.canvas_brush = CreateSolidBrush(state.canvas);
    state.surface_brush = CreateSolidBrush(state.surface);
    if (state.window != nullptr) {
        const BOOL enabled = state.dark ? TRUE : FALSE;
        if (FAILED(DwmSetWindowAttribute(state.window, 20, &enabled, sizeof(enabled)))) {
            static_cast<void>(DwmSetWindowAttribute(
                state.window, 19, &enabled, sizeof(enabled)));
        }
        const wchar_t* theme = state.dark ? L"DarkMode_Explorer" : L"Explorer";
        for (HWND control : {state.product, state.version, state.heading, state.body,
                 state.path_label, state.path, state.browse, state.space,
                 state.desktop, state.progress, state.status, state.launch,
                 state.purge, state.back, state.next, state.cancel}) {
            if (control != nullptr) SetWindowTheme(control, theme, nullptr);
        }
        SendMessageW(state.progress, PBM_SETBKCOLOR, 0, state.surface);
        SendMessageW(state.progress, PBM_SETBARCOLOR, 0, kAccent);
        InvalidateRect(state.window, nullptr, TRUE);
    }
}

[[nodiscard]] int px(const Wizard& state, const int value)
{
    return MulDiv(value, static_cast<int>(state.dpi), 96);
}

[[nodiscard]] HFONT make_font(
    const Wizard& state,
    const int points,
    const int weight)
{
    return CreateFontW(
        -MulDiv(points, static_cast<int>(state.dpi), 72), 0, 0, 0, weight,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

void rebuild_fonts(Wizard& state)
{
    state.fonts.reset();
    state.fonts.title = make_font(state, 18, FW_BOLD);
    state.fonts.heading = make_font(state, 18, FW_SEMIBOLD);
    state.fonts.body = make_font(state, 11, FW_NORMAL);
    state.fonts.caption = make_font(state, 9, FW_NORMAL);
    state.fonts.button = make_font(state, 10, FW_SEMIBOLD);
    SendMessageW(state.product, WM_SETFONT,
        reinterpret_cast<WPARAM>(state.fonts.title), TRUE);
    SendMessageW(state.heading, WM_SETFONT,
        reinterpret_cast<WPARAM>(state.fonts.heading), TRUE);
    for (HWND control : {state.version, state.body, state.path_label, state.path,
             state.space, state.desktop, state.status, state.launch, state.purge}) {
        SendMessageW(control, WM_SETFONT,
            reinterpret_cast<WPARAM>(state.fonts.body), TRUE);
    }
    for (HWND control : {state.back, state.next, state.cancel, state.browse}) {
        SendMessageW(control, WM_SETFONT,
            reinterpret_cast<WPARAM>(state.fonts.button), TRUE);
    }
}

void move(HWND control, const Wizard& state, int x, int y, int width, int height)
{
    MoveWindow(control, px(state, x), px(state, y), px(state, width),
        px(state, height), TRUE);
}

void layout(Wizard& state)
{
    RECT client{};
    GetClientRect(state.window, &client);
    const int width = MulDiv(client.right, 96, static_cast<int>(state.dpi));
    const int height = MulDiv(client.bottom, 96, static_cast<int>(state.dpi));
    const int left = 48;
    const int content_width = std::max(280, width - 96);
    move(state.product, state, left, 24, 120, 34);
    move(state.version, state, left + 126, 28, content_width - 126, 28);
    move(state.heading, state, left, 72, content_width, 42);
    move(state.body, state, left, 116, content_width, 52);
    move(state.path_label, state, left, 176, content_width, 24);
    move(state.path, state, left, 202, content_width - 98, 34);
    move(state.browse, state, left + content_width - 90, 202, 90, 34);
    move(state.space, state, left, 244, content_width, 24);
    move(state.desktop, state, left, 276, content_width, 30);
    move(state.progress, state, left, 190, content_width, 14);
    move(state.status, state, left, 216, content_width, 54);
    move(state.launch, state, left, 220, content_width, 30);
    move(state.purge, state, left, 205, content_width, 70);
    const int button_y = height - 53;
    move(state.cancel, state, width - 126, button_y, 92, 34);
    move(state.next, state, width - 226, button_y, 92, 34);
    move(state.back, state, width - 326, button_y, 92, 34);
}

void show(HWND control, const bool visible)
{
    ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

[[nodiscard]] std::wstring format_size(const std::uint64_t bytes)
{
    constexpr std::uint64_t mebibyte = 1'048'576U;
    const auto tenths = (bytes * 10U + mebibyte - 1U) / mebibyte;
    return std::format(L"需要约 {}.{} MB 可用空间", tenths / 10U, tenths % 10U);
}

void set_page(Wizard& state, const int page)
{
    state.page = page;
    const bool install = state.mode == Mode::install;
    const bool welcome = page == 0;
    const bool options = install && page == 1;
    const bool progress = (install && page == 2) || (!install && page == 1);
    const bool finish = (install && page == 3) || (!install && page == 2);

    show(state.path_label, options);
    show(state.path, options);
    show(state.browse, options);
    show(state.space, options);
    show(state.desktop, options);
    show(state.progress, progress);
    show(state.status, progress);
    show(state.launch, install && finish);
    show(state.purge, !install && welcome);
    show(state.back, options);
    show(state.next, !progress);
    show(state.cancel, !finish);

    EnableWindow(state.back, TRUE);
    EnableWindow(state.next, TRUE);
    EnableWindow(state.cancel, TRUE);

    if (install) {
        if (welcome) {
            SetWindowTextW(state.heading,
                state.install_config.updating ? L"更新 CD.404" : L"安装 CD.404");
            SetWindowTextW(state.body,
                state.install_config.updating
                    ? L"保留现有数据。请先关闭 CD.404。"
                    : L"轻量 Windows 音频 CD 播放器。");
            SetWindowTextW(state.next, L"下一步");
        } else if (options) {
            SetWindowTextW(state.heading, L"安装选项");
            SetWindowTextW(state.body,
                L"选择安装位置和快捷方式。"
            );
            SetWindowTextW(state.next,
                state.install_config.updating ? L"更新" : L"安装");
        } else if (progress) {
            SetWindowTextW(state.heading,
                state.install_config.updating ? L"正在更新 CD.404" : L"正在安装 CD.404");
            SetWindowTextW(state.body, L"正在写入程序文件。");
        } else {
            SetWindowTextW(state.heading,
                state.install_config.updating ? L"更新完成" : L"安装完成");
            SetWindowTextW(state.body,
                L"CD.404 已安装。"
            );
            SetWindowTextW(state.next, L"完成");
            SetWindowTextW(state.launch, L"立即启动 CD.404");
            state.launch_selected = true;
            InvalidateRect(state.launch, nullptr, TRUE);
        }
    } else {
        if (welcome) {
            SetWindowTextW(state.heading, L"卸载 CD.404");
            SetWindowTextW(state.body,
                L"删除程序文件和快捷方式。"
            );
            SetWindowTextW(state.next, L"卸载");
        } else if (progress) {
            SetWindowTextW(state.heading, L"正在卸载 CD.404");
            SetWindowTextW(state.body, L"正在删除程序文件。");
        } else {
            SetWindowTextW(state.heading, L"卸载完成");
            SetWindowTextW(state.body,
                L"CD.404 已卸载。"
            );
            SetWindowTextW(state.next, L"完成");
        }
    }
    InvalidateRect(state.window, nullptr, TRUE);
}

void set_progress(Wizard& state, const int value, const std::wstring_view text)
{
    SendMessageW(state.progress, PBM_SETPOS,
        static_cast<WPARAM>(std::clamp(value, 0, 100)), 0);
    const std::wstring copy(text);
    SetWindowTextW(state.status, copy.c_str());
    UpdateWindow(state.window);
}

void show_failure(Wizard& state, const DWORD error)
{
    wchar_t* message{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring detail = error == ERROR_BUSY
        ? L"CD.404 未能自动退出。请手动关闭后重试。"
        : (length != 0U && message != nullptr
            ? std::wstring(message, length)
            : std::format(L"Windows 错误 {}", error));
    if (message != nullptr) LocalFree(message);
    MessageBoxW(state.window, detail.c_str(), L"CD.404 安装程序",
        MB_OK | MB_ICONERROR);
}

void run_action(Wizard& state)
{
    const bool installing = state.mode == Mode::install;
    set_page(state, installing ? 2 : 1);
    set_progress(state, 0, installing ? L"正在准备安装…" : L"正在准备卸载…");
    DWORD error{};
    const ProgressCallback progress = [&state](int value, std::wstring_view text) {
        set_progress(state, value, text);
    };
    std::wstring destination(32'768U, L'\0');
    const int destination_length = installing
        ? GetWindowTextW(state.path, destination.data(),
              static_cast<int>(destination.size()))
        : 0;
    destination.resize(static_cast<std::size_t>(std::max(destination_length, 0)));
    const bool success = installing
        ? state.install_config.install(
              destination,
              state.desktop_selected,
              progress, error)
        : state.uninstall_config.uninstall(
              state.purge_selected,
              progress, error);
    if (!success) {
        state.exit_code = 1;
        show_failure(state, error);
        set_page(state, installing ? 1 : 0);
        return;
    }
    set_progress(state, 100, installing ? L"安装完成" : L"卸载完成");
    set_page(state, installing ? 3 : 2);
}

void browse_for_destination(Wizard& state)
{
    IFileDialog* dialog{};
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options{};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        static_cast<void>(dialog->SetOptions(
            options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT));
    }
    dialog->SetTitle(L"选择 CD.404 安装位置");
    if (SUCCEEDED(dialog->Show(state.window))) {
        IShellItem* item{};
        if (SUCCEEDED(dialog->GetResult(&item))) {
            wchar_t* path{};
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) &&
                path != nullptr) {
                SetWindowTextW(state.path, path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

LRESULT on_command(Wizard& state, const int identifier)
{
    if (identifier == kDesktop || identifier == kLaunch || identifier == kPurge) {
        const HWND control = GetDlgItem(state.window, identifier);
        bool* selected = identifier == kDesktop ? &state.desktop_selected :
            (identifier == kLaunch ? &state.launch_selected : &state.purge_selected);
        *selected = !*selected;
        InvalidateRect(control, nullptr, TRUE);
    } else if (identifier == kBack && state.mode == Mode::install && state.page == 1) {
        set_page(state, 0);
    } else if (identifier == kBrowse) {
        browse_for_destination(state);
    } else if (identifier == kCancel) {
        if (MessageBoxW(state.window, L"确定要退出安装向导吗？",
                L"CD.404 安装程序", MB_YESNO | MB_ICONQUESTION |
                    MB_DEFBUTTON2) == IDYES) {
            DestroyWindow(state.window);
        }
    } else if (identifier == kNext) {
        if (state.mode == Mode::install && state.page == 0) {
            set_page(state, 1);
        } else if ((state.mode == Mode::install && state.page == 1) ||
                   (state.mode == Mode::uninstall && state.page == 0)) {
            if (state.mode == Mode::uninstall &&
                state.purge_selected &&
                MessageBoxW(state.window,
                    L"所有设置、缓存、播放记录和凭据都会永久删除。继续吗？",
                    L"彻底删除 CD.404 数据",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return 0;
            }
            run_action(state);
        } else {
            if (state.mode == Mode::install && state.page == 3 &&
                state.launch_selected &&
                state.install_config.launch) {
                std::wstring destination(32'768U, L'\0');
                const int length = GetWindowTextW(state.path, destination.data(),
                    static_cast<int>(destination.size()));
                destination.resize(static_cast<std::size_t>(std::max(length, 0)));
                state.install_config.launch(destination);
            }
            DestroyWindow(state.window);
        }
    }
    return 0;
}

void draw_checkbox(Wizard& state, const DRAWITEMSTRUCT& item)
{
    const HDC context = item.hDC;
    FillRect(context, &item.rcItem, state.canvas_brush);
    const int side = px(state, 16);
    const int top = item.rcItem.top + (item.rcItem.bottom - item.rcItem.top - side) / 2;
    RECT box{item.rcItem.left, top, item.rcItem.left + side, top + side};
    const bool checked = item.CtlID == kDesktop ? state.desktop_selected :
        (item.CtlID == kLaunch ? state.launch_selected : state.purge_selected);
    HBRUSH box_brush = CreateSolidBrush(
        checked ? kAccent : state.surface);
    FillRect(context, &box, box_brush);
    DeleteObject(box_brush);
    HPEN border = CreatePen(PS_SOLID, 1, state.muted);
    const HGDIOBJ old_pen = SelectObject(context, border);
    const HGDIOBJ old_brush = SelectObject(context, GetStockObject(NULL_BRUSH));
    Rectangle(context, box.left, box.top, box.right, box.bottom);
    SelectObject(context, old_brush);
    SelectObject(context, old_pen);
    DeleteObject(border);
    if (checked) {
        HPEN check = CreatePen(PS_SOLID, std::max(1, px(state, 2)), RGB(255, 255, 255));
        const HGDIOBJ previous = SelectObject(context, check);
        MoveToEx(context, box.left + px(state, 3), box.top + px(state, 8), nullptr);
        LineTo(context, box.left + px(state, 7), box.top + px(state, 12));
        LineTo(context, box.left + px(state, 14), box.top + px(state, 4));
        SelectObject(context, previous);
        DeleteObject(check);
    }
    std::wstring text(512U, L'\0');
    const int length = GetWindowTextW(
        item.hwndItem, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>(std::max(length, 0)));
    RECT text_rect = item.rcItem;
    text_rect.left += side + px(state, 10);
    SetBkMode(context, TRANSPARENT);
    SetTextColor(context, state.ink);
    SelectObject(context, state.fonts.body);
    DrawTextW(context, text.c_str(), static_cast<int>(text.size()), &text_rect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if ((item.itemState & ODS_FOCUS) != 0U) {
        DrawFocusRect(context, &text_rect);
    }
}

LRESULT CALLBACK window_procedure(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<Wizard*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<Wizard*>(creation->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_COMMAND:
        return on_command(*state, LOWORD(wparam));
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (item != nullptr && (item->CtlID == kDesktop ||
                item->CtlID == kLaunch || item->CtlID == kPurge)) {
            draw_checkbox(*state, *item);
            return TRUE;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    case WM_CTLCOLORSTATIC: {
        const HDC context = reinterpret_cast<HDC>(wparam);
        const HWND control = reinterpret_cast<HWND>(lparam);
        SetBkMode(context, TRANSPARENT);
        SetTextColor(context,
            control == state->version ? state->muted : state->ink);
        return reinterpret_cast<LRESULT>(state->canvas_brush);
    }
    case WM_CTLCOLOREDIT: {
        const HDC context = reinterpret_cast<HDC>(wparam);
        SetTextColor(context, state->ink);
        SetBkColor(context, state->surface);
        return reinterpret_cast<LRESULT>(state->surface_brush);
    }
    case WM_CTLCOLORBTN: {
        const HDC context = reinterpret_cast<HDC>(wparam);
        SetTextColor(context, state->ink);
        SetBkColor(context, state->canvas);
        return reinterpret_cast<LRESULT>(state->canvas_brush);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC context = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(context, &client, state->canvas_brush);
        RECT footer{0, client.bottom - px(*state, 70),
            client.right, client.bottom};
        FillRect(context, &footer, state->surface_brush);
        HPEN separator = CreatePen(PS_SOLID, 1,
            state->dark ? RGB(54, 59, 70) : RGB(225, 228, 234));
        const HGDIOBJ old_pen = SelectObject(context, separator);
        MoveToEx(context, footer.left, footer.top, nullptr);
        LineTo(context, footer.right, footer.top);
        SelectObject(context, old_pen);
        DeleteObject(separator);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DPICHANGED: {
        state->dpi = HIWORD(wparam);
        rebuild_fonts(*state);
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        RECT desired{0, 0,
            MulDiv(720, static_cast<int>(state->dpi), 96),
            MulDiv(430, static_cast<int>(state->dpi), 96)};
        AdjustWindowRectExForDpi(&desired,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            FALSE, 0, state->dpi);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            desired.right - desired.left,
            desired.bottom - desired.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        layout(*state);
        return 0;
    }
    case WM_SIZE:
        layout(*state);
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        apply_theme(*state);
        return 0;
    case WM_CLOSE:
        if (state->page == (state->mode == Mode::install ? 2 : 1)) return 0;
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(state->exit_code);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

HWND create_control(
    Wizard& state,
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    int identifier = 0,
    DWORD extended_style = 0)
{
    return CreateWindowExW(extended_style, class_name, text,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
        state.instance, nullptr);
}

bool create_controls(Wizard& state)
{
    state.product = create_control(state, L"STATIC", L"CD.404", SS_LEFT);
    const std::wstring version =
        state.mode == Mode::install ? state.install_config.version : L"卸载程序";
    state.version = create_control(state, L"STATIC", version.c_str(), SS_LEFT);
    state.heading = create_control(state, L"STATIC", L"", SS_LEFT);
    state.body = create_control(state, L"STATIC", L"", SS_LEFT);
    state.path_label = create_control(state, L"STATIC", L"安装位置", SS_LEFT);
    state.path = create_control(state, L"EDIT",
        state.install_config.destination.c_str(), ES_LEFT |
            ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 0, WS_EX_CLIENTEDGE);
    state.browse = create_control(state, L"BUTTON", L"浏览…",
        BS_PUSHBUTTON | WS_TABSTOP, kBrowse);
    const std::wstring required = format_size(state.install_config.required_bytes);
    state.space = create_control(state, L"STATIC", required.c_str(), SS_LEFT);
    state.desktop = create_control(state, L"BUTTON", L"创建桌面快捷方式",
        BS_OWNERDRAW | WS_TABSTOP, kDesktop);
    state.progress = create_control(state, PROGRESS_CLASSW, L"",
        PBS_SMOOTH, 0);
    SendMessageW(state.progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(state.progress, PBM_SETSTATE, PBST_NORMAL, 0);
    SendMessageW(state.progress, PBM_SETBARCOLOR, 0, kAccent);
    state.status = create_control(state, L"STATIC", L"", SS_LEFT);
    state.launch = create_control(state, L"BUTTON", L"立即启动 CD.404",
        BS_OWNERDRAW | WS_TABSTOP, kLaunch);
    state.purge = create_control(state, L"BUTTON",
        L"同时删除设置、缓存、播放记录和 ListenBrainz 凭据",
        BS_OWNERDRAW | WS_TABSTOP, kPurge);
    state.back = create_control(state, L"BUTTON", L"上一步",
        BS_PUSHBUTTON | WS_TABSTOP, kBack);
    state.next = create_control(state, L"BUTTON", L"下一步",
        BS_DEFPUSHBUTTON | WS_TABSTOP, kNext);
    state.cancel = create_control(state, L"BUTTON", L"取消",
        BS_PUSHBUTTON | WS_TABSTOP, kCancel);
    return state.product && state.version && state.heading && state.body &&
        state.path_label && state.path && state.browse && state.space && state.desktop &&
        state.progress && state.status && state.launch && state.purge && state.back &&
        state.next && state.cancel;
}

int run(Wizard& state)
{
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = state.instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hIconSm = window_class.hIcon;
    window_class.lpszClassName = kWindowClass;
    RegisterClassExW(&window_class);
    apply_theme(state);

    const UINT initial_dpi = GetDpiForSystem();
    RECT bounds{0, 0,
        MulDiv(720, static_cast<int>(initial_dpi), 96),
        MulDiv(430, static_cast<int>(initial_dpi), 96)};
    AdjustWindowRectExForDpi(&bounds,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        FALSE, 0, initial_dpi);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    const int available_width = static_cast<int>(work_area.right - work_area.left) - width;
    const int available_height = static_cast<int>(work_area.bottom - work_area.top) - height;
    const int x = static_cast<int>(work_area.left) + std::max(0, available_width / 2);
    const int y = static_cast<int>(work_area.top) + std::max(0, available_height / 2);
    state.window = CreateWindowExW(0, kWindowClass,
        state.mode == Mode::install ? L"安装 CD.404" : L"卸载 CD.404",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, width, height, nullptr, nullptr, state.instance, &state);
    if (state.window == nullptr) return 1;
    state.dpi = GetDpiForWindow(state.window);
    RECT desired{0, 0,
        MulDiv(720, static_cast<int>(state.dpi), 96),
        MulDiv(430, static_cast<int>(state.dpi), 96)};
    AdjustWindowRectExForDpi(&desired,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        FALSE, 0, state.dpi);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(state.window, MONITOR_DEFAULTTONEAREST), &monitor);
    const int desired_width = desired.right - desired.left;
    const int desired_height = desired.bottom - desired.top;
    const int desired_x = monitor.rcWork.left + std::max(0,
        (static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) - desired_width) / 2);
    const int desired_y = monitor.rcWork.top + std::max(0,
        (static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) - desired_height) / 2);
    SetWindowPos(state.window, nullptr, desired_x, desired_y,
        desired_width, desired_height, SWP_NOACTIVATE | SWP_NOZORDER);
    if (!create_controls(state)) return 1;
    apply_theme(state);
    rebuild_fonts(state);
    layout(state);
    set_page(state, 0);
    ShowWindow(state.window, SW_SHOW);
    UpdateWindow(state.window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(state.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return state.exit_code;
}

} // namespace

int run_install_wizard(HINSTANCE instance, InstallWizardConfig config)
{
    Wizard state;
    state.instance = instance;
    state.mode = Mode::install;
    state.install_config = std::move(config);
    return run(state);
}

int run_uninstall_wizard(HINSTANCE instance, UninstallWizardConfig config)
{
    Wizard state;
    state.instance = instance;
    state.mode = Mode::uninstall;
    state.uninstall_config = std::move(config);
    return run(state);
}

} // namespace cd404::installer
