#include <cd404/core/cd_time.hpp>
#include <cd404/platform/windows/optical_drive.hpp>

#include <iomanip>
#include <iostream>
#include <string>

namespace {

[[nodiscard]] std::wstring widen_ascii(const char* value)
{
    std::wstring result;
    while (*value != '\0') {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*value)));
        ++value;
    }
    return result;
}

void print_track(const cd404::disc::Track& track)
{
    const auto absolute_sector = cd404::core::lba_to_absolute_sector(track.start_lba);
    const auto msf = absolute_sector
                         ? cd404::core::absolute_sector_to_msf(*absolute_sector)
                         : std::nullopt;

    std::wcout << L"  Track " << std::setw(2) << std::setfill(L'0')
               << static_cast<unsigned int>(track.number) << std::setfill(L' ')
               << (track.is_audio ? L"  audio" : L"  data ")
               << L"  LBA=" << track.start_lba << L"  sectors="
               << (track.end_lba - track.start_lba);

    if (msf) {
        std::wcout << L"  MSF=" << std::setw(2) << std::setfill(L'0') << msf->minute
                   << L":" << std::setw(2) << msf->second << L":" << std::setw(2)
                   << msf->frame << std::setfill(L' ');
    }

    std::wcout << L'\n';
}

} // namespace

int wmain()
{
    const auto drives = cd404::platform::windows::enumerate_optical_drives();
    if (drives.empty()) {
        std::wcout << L"No optical drives were detected.\n";
        return 0;
    }

    std::wcout << L"Detected " << drives.size() << L" optical drive(s).\n";
    for (const auto& drive : drives) {
        std::wcout << L"\n" << drive.root_path << L" (" << drive.device_path << L")\n";
        const auto result = cd404::platform::windows::read_toc(drive);
        if (!result.toc) {
            std::wcout << L"  Unable to read TOC. Win32 error " << result.system_error
                       << L": "
                       << cd404::platform::windows::format_system_error(
                              result.system_error)
                       << L"; validation="
                       << widen_ascii(cd404::disc::to_string(result.validation_error))
                       << L'\n';
            continue;
        }

        std::wcout << L"  Lead-out LBA: " << result.toc->lead_out_lba() << L'\n';
        for (const auto& track : result.toc->tracks()) {
            print_track(track);
        }
    }

    return 0;
}
