#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::platform::windows::detail {

struct HttpResponse final {
    std::vector<std::uint8_t> body;
    unsigned long system_error{};
    unsigned long status{};
};

[[nodiscard]] HttpResponse https_get(
    std::wstring_view host,
    std::wstring_view path,
    std::size_t maximum_response_bytes = 2U * 1'024U * 1'024U);

[[nodiscard]] std::wstring percent_encode_utf8(std::wstring_view value);
[[nodiscard]] std::wstring utf8_to_wide(std::string_view value);

} // namespace cd404::platform::windows::detail
