#include <windows.h>

#include <winioctl.h>
#include <ntddcdrm.h>

#include <cd404/core/cd_time.hpp>
#include <cd404/platform/windows/optical_drive.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <utility>
#include <vector>

namespace cd404::platform::windows {
namespace {

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] std::optional<core::Sector> track_address_to_lba(
    const TRACK_DATA& track_data) noexcept
{
    const core::Msf msf{
        static_cast<int>(track_data.Address[1]),
        static_cast<int>(track_data.Address[2]),
        static_cast<int>(track_data.Address[3]),
    };
    const auto absolute_sector = core::msf_to_absolute_sector(msf);
    return absolute_sector ? core::absolute_sector_to_lba(*absolute_sector) : std::nullopt;
}

} // namespace

std::vector<OpticalDrive> enumerate_optical_drives()
{
    const DWORD required_characters = GetLogicalDriveStringsW(0, nullptr);
    if (required_characters == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required_characters) + 1);
    if (GetLogicalDriveStringsW(
            static_cast<DWORD>(buffer.size()),
            buffer.data()) == 0) {
        return {};
    }

    std::vector<OpticalDrive> drives;
    for (const wchar_t* root = buffer.data(); *root != L'\0'; root += std::wcslen(root) + 1) {
        if (GetDriveTypeW(root) != DRIVE_CDROM) {
            continue;
        }

        std::wstring root_path(root);
        std::wstring device_path = L"\\\\.\\";
        device_path.append(root_path, 0, 2);
        drives.push_back(OpticalDrive{std::move(root_path), std::move(device_path)});
    }

    return drives;
}

TocReadResult read_toc(const OpticalDrive& drive)
{
    UniqueHandle handle(CreateFileW(
        drive.device_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));

    if (handle.get() == INVALID_HANDLE_VALUE) {
        return TocReadResult{std::nullopt, GetLastError(), disc::TocError::none};
    }

    CDROM_READ_TOC_EX request{};
    request.Format = CDROM_READ_TOC_EX_FORMAT_TOC;
    request.Msf = 1;

    CDROM_TOC native_toc{};
    DWORD bytes_returned{};
    if (!DeviceIoControl(
            handle.get(),
            IOCTL_CDROM_READ_TOC_EX,
            &request,
            sizeof(request),
            &native_toc,
            sizeof(native_toc),
            &bytes_returned,
            nullptr)) {
        return TocReadResult{std::nullopt, GetLastError(), disc::TocError::none};
    }

    if (native_toc.FirstTrack == 0 || native_toc.LastTrack < native_toc.FirstTrack) {
        return TocReadResult{
            std::nullopt,
            ERROR_INVALID_DATA,
            disc::TocError::invalid_track_number,
        };
    }

    const auto track_count = static_cast<std::size_t>(
        native_toc.LastTrack - native_toc.FirstTrack + 1);
    if (track_count == 0 || track_count > 99) {
        return TocReadResult{
            std::nullopt,
            ERROR_INVALID_DATA,
            disc::TocError::invalid_track_number,
        };
    }

    const auto minimum_toc_bytes =
        offsetof(CDROM_TOC, TrackData) + (track_count + 1) * sizeof(TRACK_DATA);
    if (bytes_returned < minimum_toc_bytes) {
        return TocReadResult{
            std::nullopt,
            ERROR_INVALID_DATA,
            disc::TocError::invalid_lead_out,
        };
    }

    std::vector<disc::RawTocEntry> entries;
    entries.reserve(track_count);
    for (std::size_t index = 0; index < track_count; ++index) {
        const TRACK_DATA& native_track = native_toc.TrackData[index];
        const auto start_lba = track_address_to_lba(native_track);
        if (!start_lba) {
            return TocReadResult{
                std::nullopt,
                ERROR_INVALID_DATA,
                disc::TocError::non_increasing_track_start,
            };
        }

        constexpr UCHAR kDataTrackFlag = 0x04;
        entries.push_back(disc::RawTocEntry{
            native_track.TrackNumber,
            *start_lba,
            (native_track.Control & kDataTrackFlag) == 0,
        });
    }

    const auto lead_out_lba = track_address_to_lba(native_toc.TrackData[track_count]);
    if (!lead_out_lba) {
        return TocReadResult{
            std::nullopt,
            ERROR_INVALID_DATA,
            disc::TocError::invalid_lead_out,
        };
    }

    disc::TocError validation_error{};
    auto toc = disc::Toc::create(entries, *lead_out_lba, validation_error);
    return TocReadResult{
        std::move(toc),
        static_cast<unsigned long>(toc ? ERROR_SUCCESS : ERROR_INVALID_DATA),
        validation_error,
    };
}

CdTextReadResult read_cd_text(const OpticalDrive& drive)
{
    UniqueHandle handle(CreateFileW(
        drive.device_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));

    if (handle.get() == INVALID_HANDLE_VALUE) {
        return CdTextReadResult{std::nullopt, GetLastError()};
    }

    CDROM_READ_TOC_EX request{};
    request.Format = CDROM_READ_TOC_EX_FORMAT_CDTEXT;

    constexpr std::size_t kMaximumCdTextBytes = 64U * 1'024U;
    std::vector<std::uint8_t> buffer(kMaximumCdTextBytes);
    DWORD bytes_returned{};
    if (!DeviceIoControl(
            handle.get(),
            IOCTL_CDROM_READ_TOC_EX,
            &request,
            sizeof(request),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_returned,
            nullptr)) {
        return CdTextReadResult{std::nullopt, GetLastError()};
    }

    constexpr std::size_t kHeaderBytes =
        offsetof(CDROM_TOC_CD_TEXT_DATA, Descriptors);
    if (bytes_returned < kHeaderBytes) {
        return CdTextReadResult{std::nullopt, ERROR_INVALID_DATA};
    }

    const std::size_t declared_bytes =
        (static_cast<std::size_t>(buffer[0]) << 8U) |
        static_cast<std::size_t>(buffer[1]);
    const std::size_t usable_bytes = std::min<std::size_t>(
        bytes_returned,
        declared_bytes + 2U);
    if (usable_bytes < kHeaderBytes) {
        return CdTextReadResult{std::nullopt, ERROR_INVALID_DATA};
    }

    const std::size_t descriptor_count =
        (usable_bytes - kHeaderBytes) / sizeof(CDROM_TOC_CD_TEXT_DATA_BLOCK);
    const auto* native_blocks = reinterpret_cast<const CDROM_TOC_CD_TEXT_DATA_BLOCK*>(
        buffer.data() + kHeaderBytes);
    std::vector<disc::CdTextPack> packs;
    packs.reserve(descriptor_count);
    for (std::size_t index = 0; index < descriptor_count; ++index) {
        const auto& native = native_blocks[index];
        disc::CdTextPack pack;
        pack.type = native.PackType;
        pack.track_number = native.TrackNumber;
        pack.sequence_number = native.SequenceNumber;
        pack.character_position = native.CharacterPosition;
        pack.block_number = native.BlockNumber;
        pack.unicode = native.Unicode != 0;
        pack.extension = native.ExtensionFlag != 0;
        std::copy(std::begin(native.Text), std::end(native.Text), pack.payload.begin());
        packs.push_back(pack);
    }

    auto metadata = disc::parse_cd_text(packs);
    if (metadata.empty()) {
        return CdTextReadResult{std::nullopt, ERROR_NOT_FOUND};
    }
    return CdTextReadResult{std::move(metadata), ERROR_SUCCESS};
}

unsigned long eject_media(const OpticalDrive& drive) noexcept
{
    if (drive.device_path.empty()) {
        return ERROR_INVALID_PARAMETER;
    }

    UniqueHandle handle(CreateFileW(
        drive.device_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));

    if (handle.get() == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }

    DWORD bytes_returned{};
    if (!DeviceIoControl(
            handle.get(),
            IOCTL_STORAGE_EJECT_MEDIA,
            nullptr,
            0,
            nullptr,
            0,
            &bytes_returned,
            nullptr)) {
        return GetLastError();
    }

    return ERROR_SUCCESS;
}

std::wstring format_system_error(const unsigned long error_code)
{
    if (error_code == ERROR_SUCCESS) {
        return L"success";
    }

    wchar_t* message_buffer{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        reinterpret_cast<wchar_t*>(&message_buffer),
        0,
        nullptr);

    if (length == 0 || message_buffer == nullptr) {
        return L"unknown Windows error";
    }

    std::unique_ptr<wchar_t, decltype(&LocalFree)> message(message_buffer, LocalFree);
    std::wstring result(message.get(), length);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

} // namespace cd404::platform::windows
