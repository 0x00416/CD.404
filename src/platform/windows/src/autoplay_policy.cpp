#include <cd404/platform/windows/autoplay_policy.hpp>

#include <windows.h>
#include <shlobj.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace cd404::platform::windows {
namespace {

constexpr wchar_t kExplorerPolicyKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
constexpr wchar_t kNoDriveTypeAutoRun[] = L"NoDriveTypeAutoRun";
constexpr wchar_t kNoDriveAutoRun[] = L"NoDriveAutoRun";

struct RegistryMask final {
    DWORD type{};
    std::vector<std::byte> bytes;

    [[nodiscard]] std::uint32_t value() const noexcept
    {
        std::uint32_t result{};
        const std::size_t count = (bytes.size() < sizeof(result))
            ? bytes.size()
            : sizeof(result);
        for (std::size_t index = 0; index < count; ++index) {
            result |= static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[index])) << (index * 8U);
        }
        return result;
    }
};

[[nodiscard]] std::optional<RegistryMask> read_registry_mask(
    const HKEY root,
    const wchar_t* value_name,
    bool& query_failed)
{
    DWORD type{};
    DWORD size{};
    const LSTATUS size_status = RegGetValueW(
        root,
        kExplorerPolicyKey,
        value_name,
        RRF_RT_REG_DWORD | RRF_RT_REG_BINARY,
        &type,
        nullptr,
        &size);
    if (size_status == ERROR_FILE_NOT_FOUND || size_status == ERROR_PATH_NOT_FOUND) {
        return std::nullopt;
    }
    if (size_status != ERROR_SUCCESS || size == 0U || size > 64U) {
        query_failed = true;
        return std::nullopt;
    }

    RegistryMask result;
    result.type = type;
    result.bytes.resize(size);
    DWORD read_size = size;
    const LSTATUS read_status = RegGetValueW(
        root,
        kExplorerPolicyKey,
        value_name,
        RRF_RT_REG_DWORD | RRF_RT_REG_BINARY,
        &type,
        result.bytes.data(),
        &read_size);
    if (read_status != ERROR_SUCCESS || read_size == 0U) {
        query_failed = true;
        return std::nullopt;
    }
    result.type = type;
    result.bytes.resize(read_size);
    return result;
}

[[nodiscard]] std::uint32_t optical_drive_letter_mask() noexcept
{
    const DWORD logical_drives = GetLogicalDrives();
    std::uint32_t result{};
    for (unsigned int index = 0; index < 26U; ++index) {
        const std::uint32_t bit = 1U << index;
        if ((logical_drives & bit) == 0U) {
            continue;
        }
        const std::array<wchar_t, 4> root{
            static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
        if (GetDriveTypeW(root.data()) == DRIVE_CDROM) {
            result |= bit;
        }
    }
    return result;
}

[[nodiscard]] bool write_registry_mask(
    const wchar_t* value_name,
    RegistryMask mask,
    const std::uint32_t cleared_bits)
{
    if (mask.bytes.empty()) {
        return false;
    }
    std::uint32_t value = mask.value() & ~cleared_bits;
    const std::size_t count = (mask.bytes.size() < sizeof(value))
        ? mask.bytes.size()
        : sizeof(value);
    for (std::size_t index = 0; index < count; ++index) {
        mask.bytes[index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }

    HKEY key{};
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            kExplorerPolicyKey,
            0,
            KEY_SET_VALUE,
            &key) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = RegSetValueExW(
        key,
        value_name,
        0,
        mask.type,
        reinterpret_cast<const BYTE*>(mask.bytes.data()),
        static_cast<DWORD>(mask.bytes.size()));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

void notify_policy_changed() noexcept
{
    DWORD_PTR ignored{};
    static_cast<void>(SendMessageTimeoutW(
        HWND_BROADCAST,
        WM_SETTINGCHANGE,
        0,
        reinterpret_cast<LPARAM>(kExplorerPolicyKey),
        SMTO_ABORTIFHUNG,
        2'000,
        &ignored));
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

} // namespace

AudioCdAutoplayPolicyStatus query_audio_cd_autoplay_policy()
{
    AudioCdAutoplayPolicyStatus result;
    bool query_failed{};
    const auto user_type = read_registry_mask(
        HKEY_CURRENT_USER, kNoDriveTypeAutoRun, query_failed);
    const auto machine_type = read_registry_mask(
        HKEY_LOCAL_MACHINE, kNoDriveTypeAutoRun, query_failed);
    const auto user_drives = read_registry_mask(
        HKEY_CURRENT_USER, kNoDriveAutoRun, query_failed);
    const auto machine_drives = read_registry_mask(
        HKEY_LOCAL_MACHINE, kNoDriveAutoRun, query_failed);
    const std::uint32_t optical_drives = optical_drive_letter_mask();

    result.current_user_blocked = user_type &&
        drive_type_mask_blocks_cdrom(user_type->value());
    result.machine_blocked = machine_type &&
        drive_type_mask_blocks_cdrom(machine_type->value());
    result.current_user_drive_blocked = user_drives &&
        (user_drives->value() & optical_drives) != 0U;
    result.machine_drive_blocked = machine_drives &&
        (machine_drives->value() & optical_drives) != 0U;
    result.query_failed = query_failed;
    return result;
}

AudioCdAutoplayRepairResult repair_audio_cd_autoplay_policy()
{
    AudioCdAutoplayRepairResult result;
    bool query_failed{};
    const auto user_type = read_registry_mask(
        HKEY_CURRENT_USER, kNoDriveTypeAutoRun, query_failed);
    const auto user_drives = read_registry_mask(
        HKEY_CURRENT_USER, kNoDriveAutoRun, query_failed);
    const std::uint32_t optical_drives = optical_drive_letter_mask();

    if (query_failed) {
        result.message = L"自动播放状态读取失败";
        result.status = query_audio_cd_autoplay_policy();
        return result;
    }

    bool changed{};
    if (user_type && drive_type_mask_blocks_cdrom(user_type->value())) {
        if (!write_registry_mask(
                kNoDriveTypeAutoRun,
                *user_type,
                kCdromAutoplayDisabledMask)) {
            result.message = L"自动播放修复失败";
            result.status = query_audio_cd_autoplay_policy();
            return result;
        }
        changed = true;
    }
    if (user_drives && (user_drives->value() & optical_drives) != 0U) {
        if (!write_registry_mask(kNoDriveAutoRun, *user_drives, optical_drives)) {
            result.message = L"光驱自动播放修复失败";
            result.status = query_audio_cd_autoplay_policy();
            return result;
        }
        changed = true;
    }

    if (changed) {
        notify_policy_changed();
    }
    result.status = query_audio_cd_autoplay_policy();
    result.succeeded = result.status.enabled();
    result.explorer_restart_required = changed && result.succeeded;
    if (result.succeeded) {
        result.message = changed
            ? L"已修复，重启资源管理器后生效"
            : L"音频 CD 自动播放已启用";
    } else if (result.status.machine_blocked ||
               result.status.machine_drive_blocked) {
        result.message = L"系统策略已阻止音频 CD 自动播放";
    } else {
        result.message = L"自动播放修复失败";
    }
    return result;
}

std::wstring describe_audio_cd_autoplay_policy(
    const AudioCdAutoplayPolicyStatus& status)
{
    if (status.query_failed) {
        return L"状态读取失败";
    }
    if (status.machine_blocked || status.machine_drive_blocked) {
        return L"系统策略已阻止";
    }
    if (status.current_user_blocked || status.current_user_drive_blocked) {
        return L"当前账户已关闭";
    }
    return L"已启用";
}

} // namespace cd404::platform::windows
