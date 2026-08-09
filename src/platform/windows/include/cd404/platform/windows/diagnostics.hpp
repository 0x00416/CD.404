#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cd404::platform::windows {

struct DiagnosticEntry final {
    std::uint64_t sequence{};
    std::int64_t unix_time{};
    std::wstring component;
    std::wstring message;
};

[[nodiscard]] std::wstring redact_diagnostic_text(std::wstring_view text);

class DiagnosticLog final {
public:
    explicit DiagnosticLog(std::size_t capacity = 512);

    void record(std::wstring_view component, std::wstring_view message);
    [[nodiscard]] std::vector<DiagnosticEntry> snapshot() const;
    [[nodiscard]] bool export_to(const std::filesystem::path& path) const noexcept;

private:
    std::size_t capacity_{};
    mutable std::mutex mutex_;
    std::vector<DiagnosticEntry> entries_;
    std::uint64_t next_sequence_{};
};

} // namespace cd404::platform::windows
