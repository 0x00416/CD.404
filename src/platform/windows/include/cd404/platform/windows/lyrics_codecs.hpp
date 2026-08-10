#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::platform::windows::detail {

[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_base64(
    std::wstring_view value);
[[nodiscard]] std::optional<std::wstring> decode_krc(
    std::span<const std::uint8_t> encrypted);
[[nodiscard]] std::optional<std::wstring> decode_qrc(
    std::string_view encrypted_hex);

} // namespace cd404::platform::windows::detail
