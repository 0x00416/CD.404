#include <windows.h>

#include <commctrl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wincred.h>
#include <wrl/client.h>

#include "resource.h"
#include "wizard.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <cwchar>

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kProductName[] = L"CD.404";
constexpr wchar_t kPublisher[] = L"CD.404 contributors";
constexpr wchar_t kApplicationFile[] = L"CD.404.exe";
constexpr wchar_t kUninstallerFile[] = L"Uninstall.exe";
constexpr wchar_t kInstallMarkerFile[] = L".cd404-install";
constexpr char kInstallMarker[] = "CD.404 native installer v1\n";
constexpr wchar_t kCredentialTarget[] = L"CD.404/ListenBrainz";
constexpr wchar_t kApplicationWindowClass[] = L"CD404.MainWindow";
constexpr wchar_t kUninstallRegistryKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CD.404";
constexpr wchar_t kAutoPlayHandlerName[] = L"CD404PlayCDAudioOnArrival";
constexpr wchar_t kAutoPlayProgId[] = L"CD404.AudioCD";
constexpr wchar_t kAutoPlayEventName[] = L"PlayCDAudioOnArrival";
constexpr wchar_t kAudioCdVerb[] = L"CD404.play";
constexpr wchar_t kExplorerAutoPlayRegistryKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\AutoplayHandlers";
constexpr wchar_t kClassesRegistryKey[] = L"Software\\Classes";

struct AutoPlayRegistryPaths final {
    std::wstring autoplay_root{kExplorerAutoPlayRegistryKey};
    std::wstring classes_root{kClassesRegistryKey};
    std::wstring install_state_root{L"Software\\CD404\\Installer"};
};

struct ComGuard final {
    HRESULT result{CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};
    ~ComGuard()
    {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

[[nodiscard]] std::filesystem::path module_path()
{
    std::wstring buffer(32'768U, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return buffer;
}

[[nodiscard]] std::filesystem::path known_folder(const KNOWNFOLDERID& id)
{
    wchar_t* value{};
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value)) ||
        value == nullptr) {
        return {};
    }
    const std::filesystem::path result(value);
    CoTaskMemFree(value);
    return result;
}

[[nodiscard]] std::filesystem::path default_install_directory()
{
    const auto local = known_folder(FOLDERID_LocalAppData);
    return local.empty() ? std::filesystem::path{} :
        local / L"Programs" / kProductName;
}

[[nodiscard]] std::filesystem::path start_menu_shortcut()
{
    const auto programs = known_folder(FOLDERID_Programs);
    return programs.empty() ? std::filesystem::path{} :
        programs / L"CD.404.lnk";
}

[[nodiscard]] std::filesystem::path desktop_shortcut()
{
    const auto desktop = known_folder(FOLDERID_Desktop);
    return desktop.empty() ? std::filesystem::path{} :
        desktop / L"CD.404.lnk";
}

[[nodiscard]] std::wstring windows_error(const DWORD code)
{
    wchar_t* message{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    std::wstring result = length != 0U && message != nullptr
        ? std::wstring(message, length)
        : std::format(L"Windows 错误 {}", code);
    if (message != nullptr) {
        LocalFree(message);
    }
    while (!result.empty() &&
           (result.back() == L'\r' || result.back() == L'\n' ||
            result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

void show_message(
    const std::wstring_view instruction,
    const std::wstring_view content,
    const PCWSTR icon)
{
    const std::wstring instruction_copy(instruction);
    const std::wstring content_copy(content);
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_SIZE_TO_CONTENT;
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    config.pszWindowTitle = kProductName;
    config.pszMainInstruction = instruction_copy.c_str();
    config.pszContent = content_copy.c_str();
    config.pszMainIcon = icon;
    static_cast<void>(TaskDialogIndirect(&config, nullptr, nullptr, nullptr));
}

[[nodiscard]] std::optional<std::span<const std::byte>> resource_bytes(
    const int identifier)
{
    const HRSRC resource = FindResourceW(
        nullptr, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    if (resource == nullptr) {
        return std::nullopt;
    }
    const HGLOBAL loaded = LoadResource(nullptr, resource);
    const DWORD size = SizeofResource(nullptr, resource);
    const void* data = loaded == nullptr ? nullptr : LockResource(loaded);
    return data != nullptr && size != 0U
        ? std::optional(std::span(
              static_cast<const std::byte*>(data),
              static_cast<std::size_t>(size)))
        : std::nullopt;
}

[[nodiscard]] int verify_embedded_payload()
{
    const auto application = resource_bytes(IDR_CD404_APPLICATION);
    const auto privacy = resource_bytes(IDR_CD404_PRIVACY);
    const auto notices = resource_bytes(IDR_CD404_NOTICES);
    if (!application || application->size() < 2U) {
        return 11;
    }
    if (!privacy) {
        return 12;
    }
    if (!notices) {
        return 13;
    }
    const auto first = std::to_integer<unsigned char>((*application)[0]);
    const auto second = std::to_integer<unsigned char>((*application)[1]);
    if (first != static_cast<unsigned char>('M') ||
        second != static_cast<unsigned char>('Z')) {
        return 14;
    }
    const auto destination = default_install_directory();
    if (destination.empty()) {
        return 15;
    }
    if (destination.filename() != kProductName) {
        return 16;
    }
    return destination.parent_path().filename() == L"Programs" ? 0 : 17;
}

[[nodiscard]] bool write_all(
    const HANDLE file,
    const std::span<const std::byte> bytes,
    DWORD& error)
{
    std::size_t offset{};
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset,
            std::numeric_limits<DWORD>::max()));
        DWORD written{};
        if (WriteFile(file, bytes.data() + offset, requested, &written, nullptr) == FALSE ||
            written == 0U) {
            error = GetLastError();
            return false;
        }
        offset += written;
    }
    return true;
}

[[nodiscard]] bool replace_with_bytes(
    const std::filesystem::path& destination,
    const std::span<const std::byte> bytes,
    DWORD& error)
{
    const std::filesystem::path temporary = destination.wstring() + L".installing";
    static_cast<void>(DeleteFileW(temporary.c_str()));
    const HANDLE file = CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    const bool written = write_all(file, bytes, error) && FlushFileBuffers(file) != FALSE;
    if (!written && error == ERROR_SUCCESS) {
        error = GetLastError();
    }
    CloseHandle(file);
    if (!written || MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        if (written) {
            error = GetLastError();
        }
        static_cast<void>(DeleteFileW(temporary.c_str()));
        return false;
    }
    return true;
}

[[nodiscard]] bool replace_with_file(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    DWORD& error)
{
    const std::filesystem::path temporary = destination.wstring() + L".installing";
    static_cast<void>(DeleteFileW(temporary.c_str()));
    if (CopyFileW(source.c_str(), temporary.c_str(), FALSE) == FALSE ||
        MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        error = GetLastError();
        static_cast<void>(DeleteFileW(temporary.c_str()));
        return false;
    }
    return true;
}

[[nodiscard]] bool create_shortcut(
    const std::filesystem::path& shortcut,
    const std::filesystem::path& application,
    DWORD& error)
{
    ComPtr<IShellLinkW> link;
    HRESULT result = CoCreateInstance(
        CLSID_ShellLink,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(link.ReleaseAndGetAddressOf()));
    if (FAILED(result)) {
        error = static_cast<DWORD>(result);
        return false;
    }
    result = link->SetPath(application.c_str());
    if (SUCCEEDED(result)) {
        result = link->SetWorkingDirectory(application.parent_path().c_str());
    }
    if (SUCCEEDED(result)) {
        result = link->SetDescription(L"轻量 Windows 音频 CD 播放器");
    }
    if (SUCCEEDED(result)) {
        result = link->SetIconLocation(application.c_str(), 0);
    }
    ComPtr<IPersistFile> persistence;
    if (SUCCEEDED(result)) {
        result = link.As(&persistence);
    }
    if (SUCCEEDED(result)) {
        result = persistence->Save(shortcut.c_str(), TRUE);
    }
    if (FAILED(result)) {
        error = static_cast<DWORD>(result);
        return false;
    }
    return true;
}

[[nodiscard]] bool set_registry_string(
    const HKEY key,
    const wchar_t* name,
    const std::wstring_view value)
{
    return RegSetValueExW(
               key,
               name,
               0,
               REG_SZ,
               reinterpret_cast<const BYTE*>(value.data()),
               static_cast<DWORD>((value.size() + 1U) * sizeof(wchar_t))) ==
        ERROR_SUCCESS;
}

[[nodiscard]] bool create_registry_key(
    const std::wstring& path,
    HKEY& key,
    DWORD& error)
{
    const LSTATUS status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        path.c_str(),
        0,
        nullptr,
        0,
        KEY_SET_VALUE | KEY_QUERY_VALUE,
        nullptr,
        &key,
        nullptr);
    if (status != ERROR_SUCCESS) {
        error = static_cast<DWORD>(status);
        return false;
    }
    return true;
}

void delete_registry_value_if_equal(
    const std::wstring& path,
    const wchar_t* const name,
    const std::wstring_view expected)
{
    std::wstring value(512U, L'\0');
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    const LSTATUS queried = RegGetValueW(
        HKEY_CURRENT_USER,
        path.c_str(),
        name,
        RRF_RT_REG_SZ,
        nullptr,
        value.data(),
        &bytes);
    if (queried != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        return;
    }
    value.resize((bytes / sizeof(wchar_t)) - 1U);
    if (CompareStringOrdinal(
            value.c_str(), -1,
            expected.data(), static_cast<int>(expected.size()),
            TRUE) == CSTR_EQUAL) {
        static_cast<void>(RegDeleteKeyValueW(
            HKEY_CURRENT_USER, path.c_str(), name));
    }
}

void delete_registry_key_if_empty(const std::wstring& path)
{
    HKEY key{};
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            path.c_str(),
            0,
            KEY_QUERY_VALUE,
            &key) != ERROR_SUCCESS) {
        return;
    }
    DWORD subkeys{};
    DWORD values{};
    const bool empty = RegQueryInfoKeyW(
        key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
        &values, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS &&
        subkeys == 0U && values == 0U;
    RegCloseKey(key);
    if (empty) {
        static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()));
    }
}

[[nodiscard]] std::optional<std::wstring> read_registry_string(
    const std::wstring& path,
    const wchar_t* const name)
{
    std::wstring value(2'048U, L'\0');
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            path.c_str(),
            name,
            RRF_RT_REG_SZ,
            nullptr,
            value.data(),
            &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return std::nullopt;
    }
    value.resize((bytes / sizeof(wchar_t)) - 1U);
    return value;
}

[[nodiscard]] bool set_registry_dword(
    const HKEY key,
    const wchar_t* const name,
    const DWORD value)
{
    return RegSetValueExW(
               key,
               name,
               0,
               REG_DWORD,
               reinterpret_cast<const BYTE*>(&value),
               sizeof(value)) == ERROR_SUCCESS;
}

[[nodiscard]] bool capture_audio_cd_shell_default(
    const AutoPlayRegistryPaths& paths,
    DWORD& error)
{
    const std::wstring state = paths.install_state_root + L"\\AudioCDShell";
    DWORD captured{};
    DWORD bytes = sizeof(captured);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            state.c_str(),
            L"Captured",
            RRF_RT_REG_DWORD,
            nullptr,
            &captured,
            &bytes) == ERROR_SUCCESS &&
        captured == 1U) {
        return true;
    }

    HKEY key{};
    if (!create_registry_key(state, key, error)) {
        return false;
    }
    const std::wstring shell = paths.classes_root + L"\\AudioCD\\shell";
    const auto previous = read_registry_string(shell, nullptr);
    bool success = set_registry_dword(key, L"Captured", 1U) &&
        set_registry_dword(key, L"HadPreviousDefault", previous ? 1U : 0U);
    if (success && previous) {
        success = set_registry_string(key, L"PreviousDefault", *previous);
    }
    RegCloseKey(key);
    if (!success) {
        error = ERROR_WRITE_FAULT;
    }
    return success;
}

void restore_audio_cd_shell_default(const AutoPlayRegistryPaths& paths)
{
    const std::wstring shell = paths.classes_root + L"\\AudioCD\\shell";
    const auto current = read_registry_string(shell, nullptr);
    if (current && CompareStringOrdinal(
            current->c_str(), -1, kAudioCdVerb, -1, TRUE) == CSTR_EQUAL) {
        const std::wstring state = paths.install_state_root + L"\\AudioCDShell";
        DWORD had_previous{};
        DWORD bytes = sizeof(had_previous);
        const bool previous_present = RegGetValueW(
            HKEY_CURRENT_USER,
            state.c_str(),
            L"HadPreviousDefault",
            RRF_RT_REG_DWORD,
            nullptr,
            &had_previous,
            &bytes) == ERROR_SUCCESS &&
            had_previous == 1U;
        HKEY key{};
        DWORD ignored{};
        if (create_registry_key(shell, key, ignored)) {
            if (previous_present) {
                const auto previous = read_registry_string(
                    state, L"PreviousDefault");
                if (previous) {
                    static_cast<void>(set_registry_string(key, nullptr, *previous));
                }
            } else {
                static_cast<void>(RegDeleteValueW(key, nullptr));
            }
            RegCloseKey(key);
        }
    }

    const std::wstring verb = shell + L"\\" + kAudioCdVerb;
    static_cast<void>(RegDeleteTreeW(HKEY_CURRENT_USER, verb.c_str()));
    static_cast<void>(RegDeleteTreeW(
        HKEY_CURRENT_USER,
        (paths.install_state_root + L"\\AudioCDShell").c_str()));
    delete_registry_key_if_empty(shell);
    delete_registry_key_if_empty(paths.classes_root + L"\\AudioCD");
    delete_registry_key_if_empty(paths.install_state_root);
}

void remove_autoplay_registration(
    const AutoPlayRegistryPaths& paths = {})
{
    restore_audio_cd_shell_default(paths);
    const std::wstring event = paths.autoplay_root +
        L"\\EventHandlers\\" + kAutoPlayEventName;
    static_cast<void>(RegDeleteKeyValueW(
        HKEY_CURRENT_USER, event.c_str(), kAutoPlayHandlerName));
    delete_registry_key_if_empty(event);

    const std::wstring handler = paths.autoplay_root +
        L"\\Handlers\\" + kAutoPlayHandlerName;
    static_cast<void>(RegDeleteTreeW(HKEY_CURRENT_USER, handler.c_str()));

    const std::wstring prog_id = paths.classes_root + L"\\" + kAutoPlayProgId;
    static_cast<void>(RegDeleteTreeW(HKEY_CURRENT_USER, prog_id.c_str()));

    // Never choose CD.404 as the default during installation. On uninstall,
    // remove only a previous user/default selection that still points to the
    // handler being removed, otherwise Windows would retain a dead selection.
    delete_registry_value_if_equal(
        paths.autoplay_root + L"\\UserChosenExecuteHandlers",
        kAutoPlayEventName,
        kAutoPlayHandlerName);
    delete_registry_value_if_equal(
        paths.autoplay_root + L"\\EventHandlersDefaultSelection",
        kAutoPlayEventName,
        kAutoPlayHandlerName);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

[[nodiscard]] bool write_autoplay_registration(
    const std::filesystem::path& directory,
    DWORD& error,
    const AutoPlayRegistryPaths& paths = {})
{
    const std::filesystem::path application = directory / kApplicationFile;
    const std::wstring icon = application.wstring() + L",0";
    const std::wstring command =
        L"\"" + application.wstring() + L"\" /autoplay \"%L\"";

    HKEY key{};
    const std::wstring prog_id = paths.classes_root + L"\\" + kAutoPlayProgId;
    if (!create_registry_key(prog_id, key, error)) {
        return false;
    }
    bool success = set_registry_string(key, nullptr, L"CD.404 音频 CD");
    RegCloseKey(key);

    const std::wstring icon_key = prog_id + L"\\DefaultIcon";
    if (success && create_registry_key(icon_key, key, error)) {
        success = set_registry_string(key, nullptr, icon);
        RegCloseKey(key);
    } else if (success) {
        success = false;
    }

    const std::wstring shell_key = prog_id + L"\\shell";
    if (success && create_registry_key(shell_key, key, error)) {
        success = set_registry_string(key, nullptr, L"play");
        RegCloseKey(key);
    } else if (success) {
        success = false;
    }

    const std::wstring verb_key = shell_key + L"\\play";
    if (success && create_registry_key(verb_key, key, error)) {
        success = set_registry_string(key, nullptr, L"使用 CD.404 播放");
        RegCloseKey(key);
    } else if (success) {
        success = false;
    }

    const std::wstring command_key = verb_key + L"\\command";
    if (success && create_registry_key(command_key, key, error)) {
        success = set_registry_string(key, nullptr, command);
        RegCloseKey(key);
    } else if (success) {
        success = false;
    }

    if (success) {
        success = capture_audio_cd_shell_default(paths, error);
    }
    const std::wstring audio_cd_shell =
        paths.classes_root + L"\\AudioCD\\shell";
    const std::wstring audio_cd_verb = audio_cd_shell + L"\\" + kAudioCdVerb;
    if (success && create_registry_key(audio_cd_verb, key, error)) {
        success = set_registry_string(key, nullptr, L"使用 CD.404 播放");
        RegCloseKey(key);
    } else if (success) {
        success = false;
    }
    const std::wstring audio_cd_command = audio_cd_verb + L"\\command";
    if (success && create_registry_key(audio_cd_command, key, error)) {
        success = set_registry_string(key, nullptr, command);
        RegCloseKey(key);
    } else if (success) {
        success = false;
    }
    if (success && create_registry_key(audio_cd_shell, key, error)) {
        success = set_registry_string(key, nullptr, kAudioCdVerb);
        RegCloseKey(key);
    } else if (success) {
        success = false;
    }

    const std::wstring handler = paths.autoplay_root +
        L"\\Handlers\\" + kAutoPlayHandlerName;
    if (success && create_registry_key(handler, key, error)) {
        success =
            set_registry_string(key, L"Action", L"播放音频 CD") &&
            set_registry_string(key, L"DefaultIcon", icon) &&
            set_registry_string(key, L"InvokeProgID", kAutoPlayProgId) &&
            set_registry_string(key, L"InvokeVerb", L"play") &&
            set_registry_string(key, L"Provider", kProductName);
        RegCloseKey(key);
    } else if (success) {
        success = false;
    }

    const std::wstring event = paths.autoplay_root +
        L"\\EventHandlers\\" + kAutoPlayEventName;
    if (success && create_registry_key(event, key, error)) {
        success = set_registry_string(key, kAutoPlayHandlerName, L"");
        RegCloseKey(key);
    } else if (success) {
        success = false;
    }

    if (!success) {
        if (error == ERROR_SUCCESS) {
            error = ERROR_WRITE_FAULT;
        }
        remove_autoplay_registration(paths);
        return false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

[[nodiscard]] bool registry_string_equals(
    const std::wstring& path,
    const wchar_t* const name,
    const std::wstring_view expected)
{
    std::wstring value(2'048U, L'\0');
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        path.c_str(),
        name,
        RRF_RT_REG_SZ,
        nullptr,
        value.data(),
        &bytes);
    if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        return false;
    }
    value.resize((bytes / sizeof(wchar_t)) - 1U);
    return value == expected;
}

[[nodiscard]] bool registry_key_missing(const std::wstring& path)
{
    HKEY key{};
    const LSTATUS status = RegOpenKeyExW(
        HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key);
    if (status == ERROR_SUCCESS) {
        RegCloseKey(key);
        return false;
    }
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] int verify_autoplay_registration()
{
    const std::wstring test_root = std::format(
        L"Software\\CD404\\InstallerTests\\{}-{}",
        GetCurrentProcessId(),
        GetTickCount64());
    const AutoPlayRegistryPaths paths{
        test_root + L"\\AutoplayHandlers",
        test_root + L"\\Classes",
        test_root + L"\\InstallerState",
    };
    struct Cleanup final {
        std::wstring root;
        ~Cleanup()
        {
            static_cast<void>(RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str()));
            static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, root.c_str()));
            delete_registry_key_if_empty(L"Software\\CD404\\InstallerTests");
            delete_registry_key_if_empty(L"Software\\CD404");
        }
    } cleanup{test_root};

    DWORD error{};
    HKEY key{};
    if (!create_registry_key(paths.autoplay_root, key, error)) {
        return 20;
    }
    RegCloseKey(key);
    if (!create_registry_key(paths.classes_root, key, error)) {
        return 20;
    }
    RegCloseKey(key);
    const std::wstring user_choice = paths.autoplay_root +
        L"\\UserChosenExecuteHandlers";
    if (!create_registry_key(user_choice, key, error)) {
        return 21;
    }
    const bool default_seeded = set_registry_string(
        key, kAutoPlayEventName, L"ExistingPlayerHandler");
    RegCloseKey(key);
    if (!default_seeded) {
        return 22;
    }

    const std::wstring audio_cd_shell =
        paths.classes_root + L"\\AudioCD\\shell";
    if (!create_registry_key(audio_cd_shell, key, error)) {
        return 22;
    }
    const bool audio_default_seeded = set_registry_string(
        key, nullptr, L"ExistingAudioCdVerb");
    RegCloseKey(key);
    if (!audio_default_seeded) {
        return 22;
    }

    const std::filesystem::path directory = L"C:\\CD.404 AutoPlay Test";
    if (!write_autoplay_registration(directory, error, paths)) {
        return 23;
    }
    const std::wstring handler = paths.autoplay_root +
        L"\\Handlers\\" + kAutoPlayHandlerName;
    const std::wstring event = paths.autoplay_root +
        L"\\EventHandlers\\" + kAutoPlayEventName;
    const std::wstring prog_id = paths.classes_root + L"\\" + kAutoPlayProgId;
    const std::wstring command = prog_id + L"\\shell\\play\\command";
    const std::wstring audio_cd_verb =
        audio_cd_shell + L"\\" + kAudioCdVerb;
    const std::wstring audio_cd_command = audio_cd_verb + L"\\command";
    const std::wstring expected_command =
        L"\"C:\\CD.404 AutoPlay Test\\CD.404.exe\" /autoplay \"%L\"";
    if (!registry_string_equals(handler, L"InvokeProgID", kAutoPlayProgId) ||
        !registry_string_equals(handler, L"InvokeVerb", L"play") ||
        !registry_string_equals(event, kAutoPlayHandlerName, L"") ||
        !registry_string_equals(command, nullptr, expected_command) ||
        !registry_string_equals(audio_cd_shell, nullptr, kAudioCdVerb) ||
        !registry_string_equals(audio_cd_command, nullptr, expected_command) ||
        !registry_string_equals(
            user_choice, kAutoPlayEventName, L"ExistingPlayerHandler")) {
        return 24;
    }

    if (!create_registry_key(user_choice, key, error)) {
        return 25;
    }
    const bool seeded =
        set_registry_string(key, kAutoPlayEventName, kAutoPlayHandlerName) &&
        set_registry_string(key, L"UnrelatedEvent", L"OtherHandler");
    RegCloseKey(key);
    if (!seeded) {
        return 26;
    }

    remove_autoplay_registration(paths);
    if (!registry_key_missing(handler) || !registry_key_missing(prog_id) ||
        !registry_key_missing(audio_cd_verb) ||
        !registry_string_equals(
            audio_cd_shell, nullptr, L"ExistingAudioCdVerb") ||
        registry_string_equals(event, kAutoPlayHandlerName, L"") ||
        registry_string_equals(
            user_choice, kAutoPlayEventName, kAutoPlayHandlerName) ||
        !registry_string_equals(
            user_choice, L"UnrelatedEvent", L"OtherHandler")) {
        return 27;
    }

    if (!write_autoplay_registration(directory, error, paths) ||
        !create_registry_key(audio_cd_shell, key, error)) {
        return 28;
    }
    const bool user_changed = set_registry_string(
        key, nullptr, L"UserChangedAfterInstall");
    RegCloseKey(key);
    if (!user_changed) {
        return 29;
    }
    remove_autoplay_registration(paths);
    if (!registry_string_equals(
            audio_cd_shell, nullptr, L"UserChangedAfterInstall") ||
        !registry_key_missing(audio_cd_verb)) {
        return 30;
    }
    return 0;
}

[[nodiscard]] bool write_uninstall_registration(
    const std::filesystem::path& directory,
    const std::uint64_t installed_bytes,
    DWORD& error)
{
    HKEY key{};
    const LSTATUS created = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kUninstallRegistryKey,
        0,
        nullptr,
        0,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (created != ERROR_SUCCESS) {
        error = static_cast<DWORD>(created);
        return false;
    }
    const std::filesystem::path app = directory / kApplicationFile;
    const std::filesystem::path uninstaller = directory / kUninstallerFile;
    const std::wstring uninstall = L"\"" + uninstaller.wstring() + L"\" /uninstall";
    const std::wstring quiet = uninstall + L" /silent";
    const std::wstring icon = L"\"" + app.wstring() + L"\",0";
    const DWORD estimated_kib = static_cast<DWORD>(std::min<std::uint64_t>(
        (installed_bytes + 1'023U) / 1'024U,
        std::numeric_limits<DWORD>::max()));
    const DWORD one = 1U;
    const bool success =
        set_registry_string(key, L"DisplayName", kProductName) &&
        set_registry_string(key, L"DisplayVersion", CD404_VERSION_WIDE) &&
        set_registry_string(key, L"Publisher", kPublisher) &&
        set_registry_string(key, L"DisplayIcon", icon) &&
        set_registry_string(key, L"InstallLocation", directory.wstring()) &&
        set_registry_string(key, L"UninstallString", uninstall) &&
        set_registry_string(key, L"QuietUninstallString", quiet) &&
        RegSetValueExW(key, L"NoModify", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&one), sizeof(one)) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"NoRepair", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&one), sizeof(one)) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&estimated_kib), sizeof(estimated_kib)) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!success) {
        error = ERROR_WRITE_FAULT;
    }
    return success;
}

[[nodiscard]] std::filesystem::path registered_install_directory()
{
    std::wstring value(32'768U, L'\0');
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        kUninstallRegistryKey,
        L"InstallLocation",
        RRF_RT_REG_SZ,
        nullptr,
        value.data(),
        &bytes);
    if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        return {};
    }
    value.resize((bytes / sizeof(wchar_t)) - 1U);
    return std::filesystem::path(value).lexically_normal();
}

[[nodiscard]] std::optional<std::filesystem::path> normalized_destination(
    const std::wstring_view requested,
    DWORD& error)
{
    if (requested.empty()) {
        error = ERROR_PATH_NOT_FOUND;
        return std::nullopt;
    }
    std::error_code filesystem_error;
    auto result = std::filesystem::absolute(
        std::filesystem::path(requested), filesystem_error).lexically_normal();
    if (filesystem_error || result.empty() || result == result.root_path() ||
        result.parent_path().empty()) {
        error = ERROR_INVALID_NAME;
        return std::nullopt;
    }
    const auto local = known_folder(FOLDERID_LocalAppData);
    if (!local.empty() && result == (local / kProductName).lexically_normal()) {
        error = ERROR_INVALID_NAME;
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] bool close_running_application(
    const cd404::installer::ProgressCallback& progress,
    DWORD& error)
{
    HWND running = FindWindowW(kApplicationWindowClass, nullptr);
    while (running != nullptr) {
        if (progress) progress(9, L"正在关闭 CD.404…");
        DWORD process_id{};
        GetWindowThreadProcessId(running, &process_id);
        if (PostMessageW(running, WM_CLOSE, 0, 0) == FALSE) {
            error = GetLastError();
            return false;
        }
        const HANDLE process = process_id == 0U ? nullptr :
            OpenProcess(SYNCHRONIZE, FALSE, process_id);
        if (process != nullptr) {
            const DWORD waited = WaitForSingleObject(process, 15'000U);
            CloseHandle(process);
            if (waited != WAIT_OBJECT_0) {
                error = ERROR_BUSY;
                return false;
            }
        } else {
            const ULONGLONG deadline = GetTickCount64() + 15'000U;
            while (IsWindow(running) != FALSE && GetTickCount64() < deadline) {
                Sleep(100U);
            }
            if (IsWindow(running) != FALSE) {
                error = ERROR_BUSY;
                return false;
            }
        }
        running = FindWindowW(kApplicationWindowClass, nullptr);
    }
    return true;
}

[[nodiscard]] bool install_application(
    const std::filesystem::path& requested_directory,
    const bool desktop,
    const cd404::installer::ProgressCallback& progress,
    DWORD& error)
{
    progress(5, L"正在验证安装包…");
    const auto normalized = normalized_destination(
        requested_directory.wstring(), error);
    if (!normalized) {
        return false;
    }
    const auto& directory = *normalized;
    const auto self = module_path();
    const auto application = resource_bytes(IDR_CD404_APPLICATION);
    const auto privacy = resource_bytes(IDR_CD404_PRIVACY);
    const auto notices = resource_bytes(IDR_CD404_NOTICES);
    if (directory.empty() || self.empty() || !application || !privacy || !notices) {
        error = ERROR_RESOURCE_DATA_NOT_FOUND;
        return false;
    }
    if (!close_running_application(progress, error)) {
        return false;
    }
    progress(12, L"正在创建安装目录…");
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory / L"docs", filesystem_error);
    if (filesystem_error) {
        error = static_cast<DWORD>(filesystem_error.value());
        return false;
    }
    progress(22, L"正在安装 CD.404…");
    if (!replace_with_bytes(directory / kApplicationFile, *application, error)) {
        return false;
    }
    progress(52, L"正在安装隐私与许可说明…");
    if (!replace_with_bytes(directory / L"docs" / L"PRIVACY.md", *privacy, error) ||
        !replace_with_bytes(directory / L"THIRD_PARTY_NOTICES.md", *notices, error)) {
        return false;
    }
    progress(64, L"正在安装卸载程序…");
    if (!replace_with_file(self, directory / kUninstallerFile, error)) {
        return false;
    }
    progress(76, L"正在创建开始菜单快捷方式…");
    const auto menu = start_menu_shortcut();
    if (menu.empty() || !create_shortcut(menu, directory / kApplicationFile, error)) {
        return false;
    }
    progress(86, desktop ? L"正在创建桌面快捷方式…" : L"正在应用快捷方式设置…");
    const auto desktop_link = desktop_shortcut();
    if (!desktop_link.empty()) {
        if (desktop) {
            if (!create_shortcut(
                    desktop_link, directory / kApplicationFile, error)) {
                return false;
            }
        } else {
            std::filesystem::remove(desktop_link, filesystem_error);
            filesystem_error.clear();
        }
    }
    progress(91, L"正在写入安装标记…");
    const auto marker = std::as_bytes(std::span(
        kInstallMarker, std::size(kInstallMarker) - 1U));
    if (!replace_with_bytes(directory / kInstallMarkerFile, marker, error)) {
        return false;
    }
    progress(93, L"正在注册音频 CD 自动播放选项…");
    if (!write_autoplay_registration(directory, error)) {
        return false;
    }
    progress(96, L"正在登记 Windows 卸载信息…");
    const bool registered = write_uninstall_registration(
        directory,
        application->size() + privacy->size() + notices->size() +
            std::filesystem::file_size(self, filesystem_error),
        error);
    if (registered) {
        progress(98, L"正在完成安装…");
    }
    return registered;
}

void remove_shortcuts_and_registration()
{
    remove_autoplay_registration();
    std::error_code error;
    const auto menu = start_menu_shortcut();
    const auto desktop = desktop_shortcut();
    if (!menu.empty()) {
        std::filesystem::remove(menu, error);
        error.clear();
    }
    if (!desktop.empty()) {
        std::filesystem::remove(desktop, error);
    }
    static_cast<void>(RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallRegistryKey));
}

[[nodiscard]] bool has_valid_install_marker(
    const std::filesystem::path& directory)
{
    if (directory.empty() || directory == directory.root_path()) {
        return false;
    }
    const HANDLE file = CreateFileW(
        (directory / kInstallMarkerFile).c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::array<char, std::size(kInstallMarker) - 1U> buffer{};
    DWORD read{};
    const bool valid = ReadFile(file, buffer.data(),
                           static_cast<DWORD>(buffer.size()), &read, nullptr) != FALSE &&
        read == buffer.size() &&
        std::memcmp(buffer.data(), kInstallMarker, buffer.size()) == 0 &&
        GetFileSize(file, nullptr) == buffer.size();
    CloseHandle(file);
    return valid;
}

[[nodiscard]] bool remove_installed_files(
    const std::filesystem::path& directory,
    DWORD& error)
{
    if (directory.empty() || directory == directory.root_path() ||
        !has_valid_install_marker(directory)) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    std::error_code filesystem_error;
    for (const auto& relative : {
             std::filesystem::path(kApplicationFile),
             std::filesystem::path(kUninstallerFile),
             std::filesystem::path(L"docs") / L"PRIVACY.md",
             std::filesystem::path(L"THIRD_PARTY_NOTICES.md"),
             std::filesystem::path(kInstallMarkerFile)}) {
        const auto target = directory / relative;
        bool removed{};
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (DeleteFileW(target.c_str()) != FALSE) {
                removed = true;
                break;
            }
            const DWORD current_error = GetLastError();
            if (current_error == ERROR_FILE_NOT_FOUND ||
                current_error == ERROR_PATH_NOT_FOUND) {
                removed = true;
                break;
            }
            if (current_error != ERROR_ACCESS_DENIED &&
                current_error != ERROR_SHARING_VIOLATION &&
                current_error != ERROR_LOCK_VIOLATION) {
                error = current_error;
                return false;
            }
            error = current_error;
            Sleep(100U);
        }
        if (!removed) {
            return false;
        }
    }
    std::filesystem::remove(directory / L"docs", filesystem_error);
    filesystem_error.clear();
    std::filesystem::remove(directory, filesystem_error);
    filesystem_error.clear();
    return true;
}

[[nodiscard]] bool same_directory(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    const auto left_text = left.lexically_normal().wstring();
    const auto right_text = right.lexically_normal().wstring();
    return CompareStringOrdinal(left_text.c_str(), -1, right_text.c_str(), -1,
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool install_with_migration(
    const std::filesystem::path& requested_directory,
    const std::filesystem::path& previous_directory,
    const bool desktop,
    const cd404::installer::ProgressCallback& progress,
    DWORD& error)
{
    if (!install_application(requested_directory, desktop, progress, error)) {
        return false;
    }
    const auto normalized = normalized_destination(
        requested_directory.wstring(), error);
    if (!normalized) {
        return false;
    }
    if (has_valid_install_marker(previous_directory) &&
        !same_directory(previous_directory, *normalized)) {
        progress(98, L"正在移除旧安装位置…");
        if (!remove_installed_files(previous_directory, error)) {
            return false;
        }
    }
    progress(100, L"安装完成");
    return true;
}

[[nodiscard]] bool purge_user_data(DWORD& error)
{
    const auto local = known_folder(FOLDERID_LocalAppData);
    if (local.empty()) {
        error = ERROR_PATH_NOT_FOUND;
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::remove_all(local / kProductName, filesystem_error);
    if (filesystem_error) {
        error = static_cast<DWORD>(filesystem_error.value());
        return false;
    }
    if (CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0) == FALSE) {
        const DWORD credential_error = GetLastError();
        if (credential_error != ERROR_NOT_FOUND) {
            error = credential_error;
            return false;
        }
    }
    return true;
}

void mark_self_for_deletion()
{
    const auto self = module_path();
    const HANDLE delete_on_exit = CreateFileW(
        self.c_str(),
        DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr);
    if (delete_on_exit != INVALID_HANDLE_VALUE) {
        // Intentionally keep the handle open. Windows closes it after the image
        // section is unmapped during process teardown.
    }
    const HANDLE file = CreateFileW(
        self.c_str(),
        DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
#ifdef FILE_DISPOSITION_FLAG_DELETE
        FILE_DISPOSITION_INFO_EX disposition{};
        disposition.Flags = FILE_DISPOSITION_FLAG_DELETE |
            FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
            FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
        static_cast<void>(SetFileInformationByHandle(
            file, FileDispositionInfoEx, &disposition, sizeof(disposition)));
#else
        static_cast<void>(MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT));
#endif
        CloseHandle(file);
    }

    std::wstring system_directory(MAX_PATH, L'\0');
    const UINT system_length = GetSystemDirectoryW(
        system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (system_length == 0U || system_length >= system_directory.size()) {
        return;
    }
    system_directory.resize(system_length);
    const auto command_processor =
        std::filesystem::path(system_directory) / L"cmd.exe";
    std::wstring command_line = std::format(
        L"\"{}\" /d /q /c \"for /l %i in (1,1,20) do "
        L"@(del /f /q \"\"{}\"\" >nul 2>&1 & if not exist \"\"{}\"\" "
        L"exit /b 0 & ping -n 2 127.0.0.1 >nul)\"",
        command_processor.wstring(), self.wstring(), self.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(command_processor.c_str(), command_line.data(), nullptr,
            nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
            system_directory.c_str(), &startup, &process) != FALSE) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

[[nodiscard]] int cleanup_after_uninstaller(
    const DWORD parent_process_id,
    const std::filesystem::path& directory,
    const bool purge,
    const cd404::installer::ProgressCallback& progress = {})
{
    if (progress) progress(8, L"正在等待卸载程序就绪…");
    if (parent_process_id != 0U) {
        const HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_process_id);
        if (parent != nullptr) {
            static_cast<void>(WaitForSingleObject(parent, 30'000U));
            CloseHandle(parent);
        }
    }
    DWORD error{};
    if (!close_running_application(progress, error)) {
        return static_cast<int>(error == ERROR_SUCCESS ? ERROR_BUSY : error);
    }
    if (progress) progress(25, L"正在验证安装目录…");
    if (!has_valid_install_marker(directory)) {
        return ERROR_INVALID_DATA;
    }
    if (purge) {
        if (progress) progress(40, L"正在删除设置、缓存、播放记录和凭据…");
        if (!purge_user_data(error)) {
            return static_cast<int>(error);
        }
    }
    if (progress) progress(58, L"正在删除应用程序文件…");
    if (!remove_installed_files(directory, error)) {
        return static_cast<int>(error == ERROR_SUCCESS ? ERROR_DELETE_PENDING : error);
    }
    if (progress) progress(82, L"正在移除自动播放注册…");
    remove_shortcuts_and_registration();
    if (progress) progress(94, L"正在完成卸载…");
    if (progress) progress(100, L"卸载完成");
    return 0;
}

[[nodiscard]] bool launch_cleanup_helper(
    DWORD& error,
    const bool interactive,
    const bool purge,
    const std::filesystem::path& directory)
{
    const auto self = module_path();
    const auto temporary_root = known_folder(FOLDERID_LocalAppData) / L"Temp";
    if (self.empty() || temporary_root.empty()) {
        error = ERROR_PATH_NOT_FOUND;
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(temporary_root, filesystem_error);
    const auto helper = temporary_root /
        std::format(L"CD404-Uninstall-{}-{}.exe", GetCurrentProcessId(), GetTickCount64());
    if (CopyFileW(self.c_str(), helper.c_str(), TRUE) == FALSE) {
        error = GetLastError();
        return false;
    }
    const std::wstring purge_switch = purge ? L" /purge" : L"";
    const std::wstring parameters = interactive
        ? std::format(L"/uninstall-ui {} \"{}\"{}",
              GetCurrentProcessId(), directory.wstring(), purge_switch)
        : std::format(L"/cleanup {} \"{}\"{}",
              GetCurrentProcessId(), directory.wstring(), purge_switch);
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.lpFile = helper.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = interactive ? SW_SHOWNORMAL : SW_HIDE;
    if (ShellExecuteExW(&execute) == FALSE) {
        error = GetLastError();
        static_cast<void>(DeleteFileW(helper.c_str()));
        return false;
    }
    if (execute.hProcess != nullptr) {
        CloseHandle(execute.hProcess);
    }
    return true;
}

[[nodiscard]] bool has_argument(
    const std::span<wchar_t*> arguments,
    const std::wstring_view expected)
{
    return std::ranges::any_of(arguments, [expected](const wchar_t* argument) {
        return argument != nullptr && _wcsicmp(argument, expected.data()) == 0;
    });
}

[[nodiscard]] DWORD argument_after(
    const std::span<wchar_t*> arguments,
    const std::wstring_view name)
{
    for (std::size_t index = 1; index + 1U < arguments.size(); ++index) {
        if (arguments[index] != nullptr &&
            _wcsicmp(arguments[index], name.data()) == 0) {
            wchar_t* end{};
            const unsigned long parsed = std::wcstoul(
                arguments[index + 1U], &end, 10);
            if (end != arguments[index + 1U] && *end == L'\0') {
                return static_cast<DWORD>(parsed);
            }
            break;
        }
    }
    return 0U;
}

[[nodiscard]] std::filesystem::path path_argument_after(
    const std::span<wchar_t*> arguments,
    const std::wstring_view name,
    const std::size_t offset)
{
    for (std::size_t index = 1; index + offset < arguments.size(); ++index) {
        if (arguments[index] != nullptr &&
            _wcsicmp(arguments[index], name.data()) == 0 &&
            arguments[index + offset] != nullptr) {
            return std::filesystem::path(arguments[index + offset]).lexically_normal();
        }
    }
    return {};
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int)
{
    ComGuard com;
    if (FAILED(com.result)) {
        return 1;
    }
    int count{};
    wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (values == nullptr || count <= 0) {
        return 1;
    }
    const std::span arguments(values, static_cast<std::size_t>(count));
    const bool silent = has_argument(arguments, L"/silent");
    const auto current_module = module_path();
    const bool installed_uninstaller =
        _wcsicmp(current_module.filename().c_str(), kUninstallerFile) == 0 &&
        has_valid_install_marker(current_module.parent_path());
    if (has_argument(arguments, L"/verify")) {
        const int result = verify_embedded_payload();
        LocalFree(values);
        return result;
    }
    if (has_argument(arguments, L"/verify-autoplay")) {
        const int result = verify_autoplay_registration();
        LocalFree(values);
        return result;
    }
    if (has_argument(arguments, L"/cleanup")) {
        const DWORD parent = argument_after(arguments, L"/cleanup");
        const auto directory = path_argument_after(arguments, L"/cleanup", 2U);
        const bool purge = has_argument(arguments, L"/purge");
        LocalFree(values);
        const int result = cleanup_after_uninstaller(parent, directory, purge);
        mark_self_for_deletion();
        return result;
    }
    if (has_argument(arguments, L"/uninstall-ui")) {
        const DWORD parent = argument_after(arguments, L"/uninstall-ui");
        const auto directory = path_argument_after(arguments, L"/uninstall-ui", 2U);
        LocalFree(values);
        const int result = cd404::installer::run_uninstall_wizard(instance, {
            [parent, directory](bool purge,
                                const cd404::installer::ProgressCallback& progress,
                                DWORD& error) {
                const int result = cleanup_after_uninstaller(
                    parent, directory, purge, progress);
                if (result != 0) {
                    error = static_cast<DWORD>(result);
                    return false;
                }
                return true;
            }});
        mark_self_for_deletion();
        return result;
    }
    if (has_argument(arguments, L"/uninstall") || installed_uninstaller) {
        DWORD error{};
        const auto directory = current_module.parent_path().lexically_normal();
        const bool launched = has_valid_install_marker(directory) &&
            launch_cleanup_helper(
                error, !silent, has_argument(arguments, L"/purge"), directory);
        if (!launched && error == ERROR_SUCCESS) {
            error = ERROR_INVALID_DATA;
        }
        if (!launched && !silent) {
            show_message(L"无法启动卸载程序", windows_error(error), TD_ERROR_ICON);
        }
        LocalFree(values);
        return launched ? 0 : 1;
    }

    if (silent) {
        DWORD error{};
        const auto previous = registered_install_directory();
        auto directory = path_argument_after(arguments, L"/install-dir", 1U);
        if (directory.empty()) {
            directory = previous;
            if (!has_valid_install_marker(directory)) {
                directory = default_install_directory();
            }
        }
        const bool installed = install_with_migration(
            directory, previous, false, [](int, std::wstring_view) {}, error);
        LocalFree(values);
        return installed ? 0 : 1;
    }

    auto directory = registered_install_directory();
    if (!has_valid_install_marker(directory)) {
        directory = default_install_directory();
    }
    const auto application = resource_bytes(IDR_CD404_APPLICATION);
    const auto privacy = resource_bytes(IDR_CD404_PRIVACY);
    const auto notices = resource_bytes(IDR_CD404_NOTICES);
    std::error_code filesystem_error;
    const auto self_size = std::filesystem::file_size(module_path(), filesystem_error);
    const std::uint64_t required =
        (application ? application->size() : 0U) +
        (privacy ? privacy->size() : 0U) +
        (notices ? notices->size() : 0U) +
        (filesystem_error ? 0U : self_size);
    const bool updating = std::filesystem::exists(
        directory / kApplicationFile, filesystem_error);
    LocalFree(values);
    return cd404::installer::run_install_wizard(instance, {
        .version = L"版本 " + std::wstring(CD404_VERSION_WIDE),
        .destination = directory.wstring(),
        .required_bytes = required,
        .updating = updating,
        .install = [previous = directory](const std::wstring& destination,
                       bool desktop,
                       const cd404::installer::ProgressCallback& progress,
                       DWORD& error) {
            return install_with_migration(
                destination, previous, desktop, progress, error);
        },
        .launch = [](const std::wstring& destination) {
            const auto executable = std::filesystem::path(destination) / kApplicationFile;
            static_cast<void>(ShellExecuteW(nullptr, L"open", executable.c_str(),
                nullptr, executable.parent_path().c_str(), SW_SHOWNORMAL));
        }});
}
