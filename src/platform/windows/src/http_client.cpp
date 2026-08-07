#include <windows.h>

#include <winhttp.h>

#include "http_client.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace cd404::platform::windows::detail {
namespace {

class InternetHandle final {
public:
    explicit InternetHandle(HINTERNET handle = nullptr) noexcept : handle_(handle) {}
    ~InternetHandle()
    {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }

private:
    HINTERNET handle_{};
};

} // namespace

HttpResponse https_get(
    const std::wstring_view host,
    const std::wstring_view path,
    const std::size_t maximum_response_bytes)
{
    if (host.empty() || path.empty() || maximum_response_bytes == 0U) {
        return {{}, ERROR_INVALID_PARAMETER, 0};
    }

    InternetHandle session(WinHttpOpen(
        L"CD.404/0.1 (https://github.com/0x00416/CD.404)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (session.get() == nullptr) {
        return {{}, GetLastError(), 0};
    }
    static_cast<void>(WinHttpSetTimeouts(session.get(), 3'000, 5'000, 5'000, 8'000));

    const std::wstring host_string(host);
    InternetHandle connection(WinHttpConnect(
        session.get(), host_string.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connection.get() == nullptr) {
        return {{}, GetLastError(), 0};
    }

    const std::wstring path_string(path);
    InternetHandle request(WinHttpOpenRequest(
        connection.get(),
        L"GET",
        path_string.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (request.get() == nullptr ||
        WinHttpSendRequest(
            request.get(),
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) == FALSE ||
        WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
        return {{}, GetLastError(), 0};
    }

    DWORD status{};
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &status_size,
            WINHTTP_NO_HEADER_INDEX) == FALSE) {
        return {{}, GetLastError(), 0};
    }

    HttpResponse response;
    response.status = status;
    for (;;) {
        DWORD available{};
        if (WinHttpQueryDataAvailable(request.get(), &available) == FALSE) {
            response.system_error = GetLastError();
            response.body.clear();
            return response;
        }
        if (available == 0U) {
            return response;
        }
        if (available > maximum_response_bytes - response.body.size()) {
            response.system_error = ERROR_FILE_TOO_LARGE;
            response.body.clear();
            return response;
        }
        const std::size_t offset = response.body.size();
        response.body.resize(offset + available);
        DWORD bytes_read{};
        if (WinHttpReadData(
                request.get(),
                response.body.data() + offset,
                available,
                &bytes_read) == FALSE) {
            response.system_error = GetLastError();
            response.body.clear();
            return response;
        }
        response.body.resize(offset + bytes_read);
    }
}

std::wstring percent_encode_utf8(const std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int byte_count = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (byte_count <= 0) {
        return {};
    }
    std::string bytes(static_cast<std::size_t>(byte_count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            bytes.data(),
            byte_count,
            nullptr,
            nullptr) != byte_count) {
        return {};
    }

    constexpr std::array<wchar_t, 16> digits{
        L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7',
        L'8', L'9', L'A', L'B', L'C', L'D', L'E', L'F',
    };
    std::wstring encoded;
    encoded.reserve(bytes.size() * 3U);
    for (const unsigned char byte : bytes) {
        const bool unreserved = (byte >= 'a' && byte <= 'z') ||
                                (byte >= 'A' && byte <= 'Z') ||
                                (byte >= '0' && byte <= '9') ||
                                byte == '-' || byte == '_' || byte == '.' || byte == '~';
        if (unreserved) {
            encoded.push_back(static_cast<wchar_t>(byte));
        } else {
            encoded.push_back(L'%');
            encoded.push_back(digits[byte >> 4U]);
            encoded.push_back(digits[byte & 0x0fU]);
        }
    }
    return encoded;
}

std::wstring utf8_to_wide(const std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int character_count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (character_count <= 0) {
        return {};
    }
    std::wstring converted(static_cast<std::size_t>(character_count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            converted.data(),
            character_count) != character_count) {
        return {};
    }
    return converted;
}

} // namespace cd404::platform::windows::detail
