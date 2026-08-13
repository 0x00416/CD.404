#pragma once

#include <cstdint>
#include <string>

namespace cd404::platform::windows {

constexpr std::uint32_t kCdromAutoplayDisabledMask = 0x20U;

[[nodiscard]] constexpr bool drive_type_mask_blocks_cdrom(
    const std::uint32_t mask) noexcept
{
    return (mask & kCdromAutoplayDisabledMask) != 0U;
}

[[nodiscard]] constexpr std::uint32_t clear_cdrom_from_drive_type_mask(
    const std::uint32_t mask) noexcept
{
    return mask & ~kCdromAutoplayDisabledMask;
}

struct AudioCdAutoplayPolicyStatus final {
    bool current_user_blocked{};
    bool machine_blocked{};
    bool current_user_drive_blocked{};
    bool machine_drive_blocked{};
    bool query_failed{};

    [[nodiscard]] bool enabled() const noexcept
    {
        return !current_user_blocked && !machine_blocked &&
            !current_user_drive_blocked && !machine_drive_blocked &&
            !query_failed;
    }

    [[nodiscard]] bool repairable_for_current_user() const noexcept
    {
        return !query_failed && !machine_blocked && !machine_drive_blocked &&
            (current_user_blocked || current_user_drive_blocked);
    }
};

struct AudioCdAutoplayRepairResult final {
    bool succeeded{};
    bool explorer_restart_required{};
    std::wstring message;
    AudioCdAutoplayPolicyStatus status;
};

[[nodiscard]] AudioCdAutoplayPolicyStatus query_audio_cd_autoplay_policy();

[[nodiscard]] AudioCdAutoplayRepairResult repair_audio_cd_autoplay_policy();

[[nodiscard]] std::wstring describe_audio_cd_autoplay_policy(
    const AudioCdAutoplayPolicyStatus& status);

} // namespace cd404::platform::windows
