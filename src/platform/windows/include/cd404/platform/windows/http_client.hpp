#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::platform::windows {

struct HttpResponse final {
    std::vector<std::uint8_t> body;
    unsigned long system_error{};
    unsigned long status{};
    std::uint64_t rate_limit_remaining{};
    std::uint64_t rate_limit_reset_seconds{};
    bool has_rate_limit_remaining{};
    bool has_rate_limit_reset{};
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    [[nodiscard]] virtual HttpResponse get(
        std::wstring_view host,
        std::wstring_view path,
        std::wstring_view headers,
        std::size_t maximum_response_bytes) = 0;
    [[nodiscard]] virtual HttpResponse post(
        std::wstring_view host,
        std::wstring_view path,
        std::wstring_view headers,
        std::span<const std::uint8_t> body,
        std::size_t maximum_response_bytes) = 0;
};

[[nodiscard]] std::shared_ptr<HttpClient> make_win_http_client();

namespace detail {

using ::cd404::platform::windows::HttpResponse;

[[nodiscard]] HttpResponse http_get(
    std::wstring_view host,
    unsigned short port,
    bool secure,
    std::wstring_view path,
    std::wstring_view headers = {},
    std::size_t maximum_response_bytes = 2U * 1'024U * 1'024U);
[[nodiscard]] HttpResponse http_post(
    std::wstring_view host,
    unsigned short port,
    bool secure,
    std::wstring_view path,
    std::wstring_view headers,
    std::span<const std::uint8_t> body,
    std::size_t maximum_response_bytes = 2U * 1'024U * 1'024U);
[[nodiscard]] HttpResponse https_get(
    std::wstring_view host,
    std::wstring_view path,
    std::size_t maximum_response_bytes = 2U * 1'024U * 1'024U);
[[nodiscard]] HttpResponse https_get(
    std::wstring_view host,
    std::wstring_view path,
    std::wstring_view headers,
    std::size_t maximum_response_bytes = 2U * 1'024U * 1'024U);
[[nodiscard]] HttpResponse https_post(
    std::wstring_view host,
    std::wstring_view path,
    std::wstring_view headers,
    std::span<const std::uint8_t> body,
    std::size_t maximum_response_bytes = 2U * 1'024U * 1'024U);

[[nodiscard]] std::wstring percent_encode_utf8(std::wstring_view value);
[[nodiscard]] std::wstring utf8_to_wide(std::string_view value);
[[nodiscard]] std::string wide_to_utf8(std::wstring_view value);

} // namespace detail
} // namespace cd404::platform::windows
