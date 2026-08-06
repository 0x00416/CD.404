#include <windows.h>

#include <winioctl.h>
#include <ntddcdrm.h>

#include <cd404/platform/windows/raw_cdda_sector_source.hpp>

#include <limits>

namespace cd404::platform::windows {

RawCddaSectorSource::RawCddaSectorSource(
    void* const native_handle,
    void* const native_io_event,
    const core::Sector first_lba,
    const core::Sector end_lba) noexcept
    : native_handle_(native_handle),
      native_io_event_(native_io_event),
      first_lba_(first_lba),
      end_lba_(end_lba)
{
}

RawCddaSectorSource::~RawCddaSectorSource()
{
    if (native_io_event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(native_io_event_));
    }
    if (native_handle_ != nullptr && native_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(native_handle_));
    }
}

core::Sector RawCddaSectorSource::first_lba() const noexcept
{
    return first_lba_;
}

core::Sector RawCddaSectorSource::end_lba() const noexcept
{
    return end_lba_;
}

audio::SectorReadResult RawCddaSectorSource::read_sectors(
    const core::Sector start_lba,
    const std::span<std::byte> destination)
{
    const auto bytes_per_sector =
        static_cast<std::size_t>(core::kCdBytesPerSector);
    if (destination.empty() || destination.size() % bytes_per_sector != 0 ||
        start_lba < first_lba_) {
        return {audio::ReadStatus::invalid_request, 0, ERROR_INVALID_PARAMETER};
    }

    if (start_lba >= end_lba_) {
        return {audio::ReadStatus::end_of_stream, 0, ERROR_SUCCESS};
    }

    const std::size_t sector_count = destination.size() / bytes_per_sector;
    const auto available_sectors = static_cast<std::size_t>(end_lba_ - start_lba);
    if (sector_count > available_sectors ||
        sector_count > std::numeric_limits<ULONG>::max() ||
        destination.size() > std::numeric_limits<DWORD>::max() || start_lba < 0 ||
        start_lba > std::numeric_limits<LONGLONG>::max() / 2'048) {
        return {audio::ReadStatus::invalid_request, 0, ERROR_INVALID_PARAMETER};
    }

    RAW_READ_INFO request{};
    request.DiskOffset.QuadPart = start_lba * 2'048;
    request.SectorCount = static_cast<ULONG>(sector_count);
    request.TrackMode = CDDA;

    auto* const event_handle = static_cast<HANDLE>(native_io_event_);
    if (event_handle == nullptr || ResetEvent(event_handle) == FALSE) {
        return {audio::ReadStatus::io_error, 0, GetLastError()};
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event_handle;
    DWORD bytes_returned{};
    const BOOL started = DeviceIoControl(
            static_cast<HANDLE>(native_handle_),
            IOCTL_CDROM_RAW_READ,
            &request,
            sizeof(request),
            destination.data(),
            static_cast<DWORD>(destination.size()),
            nullptr,
            &overlapped);
    if (started == FALSE) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            return {audio::ReadStatus::io_error, 0, error};
        }
        if (WaitForSingleObject(event_handle, INFINITE) != WAIT_OBJECT_0) {
            return {audio::ReadStatus::io_error, 0, GetLastError()};
        }
    }
    // FILE_FLAG_OVERLAPPED operations may still complete synchronously. In
    // both cases GetOverlappedResult is the single source of the byte count.
    if (GetOverlappedResult(
            static_cast<HANDLE>(native_handle_),
            &overlapped,
            &bytes_returned,
            FALSE) == FALSE) {
        return {audio::ReadStatus::io_error, 0, GetLastError()};
    }

    if (bytes_returned != destination.size()) {
        return {audio::ReadStatus::io_error, 0, ERROR_READ_FAULT};
    }

    return {audio::ReadStatus::ok, sector_count, ERROR_SUCCESS};
}

void RawCddaSectorSource::request_cancel() noexcept
{
    if (native_handle_ != nullptr && native_handle_ != INVALID_HANDLE_VALUE) {
        static_cast<void>(CancelIoEx(static_cast<HANDLE>(native_handle_), nullptr));
    }
}

RawCddaOpenResult open_raw_cdda_source(
    const OpticalDrive& drive,
    const core::Sector first_lba,
    const core::Sector end_lba)
{
    if (first_lba < 0 || first_lba >= end_lba) {
        return {nullptr, ERROR_INVALID_PARAMETER};
    }

    HANDLE handle = CreateFileW(
        drive.device_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {nullptr, GetLastError()};
    }

    HANDLE io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (io_event == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        return {nullptr, error};
    }

    return {
        std::make_unique<RawCddaSectorSource>(
            handle,
            io_event,
            first_lba,
            end_lba),
        ERROR_SUCCESS,
    };
}

} // namespace cd404::platform::windows
