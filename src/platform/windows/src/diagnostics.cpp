#include <windows.h>

#include <cd404/platform/windows/diagnostics.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

namespace cd404::platform::windows {
namespace {

void redact_value_after(
    std::wstring& text,
    const std::wstring_view marker,
    const std::wstring_view replacement)
{
    std::size_t search{};
    while ((search = text.find(marker, search)) != std::wstring::npos) {
        const std::size_t value_begin = search + marker.size();
        std::size_t value_end = value_begin;
        while (value_end < text.size() && text[value_end] != L'\r' &&
               text[value_end] != L'\n' && text[value_end] != L' ' &&
               text[value_end] != L'\t' && text[value_end] != L'"') {
            ++value_end;
        }
        text.replace(value_begin, value_end - value_begin, replacement);
        search = value_begin + replacement.size();
    }
}

void redact_path_prefix(std::wstring& text, const std::wstring_view prefix)
{
    std::size_t search{};
    while ((search = text.find(prefix, search)) != std::wstring::npos) {
        std::size_t end = search;
        while (end < text.size() && text[end] != L'\r' && text[end] != L'\n' &&
               text[end] != L'"') {
            ++end;
        }
        text.replace(search, end - search, L"[REDACTED_PATH]");
        search += std::size(L"[REDACTED_PATH]") - 1U;
    }
}

[[nodiscard]] std::string wide_to_utf8(const std::wstring_view value)
{
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return written == required ? result : std::string{};
}

} // namespace

std::wstring redact_diagnostic_text(const std::wstring_view source)
{
    std::wstring text(source);
    redact_value_after(text, L"Authorization: Token ", L"[REDACTED_TOKEN]");
    redact_value_after(text, L"Authorization: Bearer ", L"[REDACTED_TOKEN]");
    redact_value_after(text, L"CD404_LISTENBRAINZ_TOKEN=", L"[REDACTED_TOKEN]");
    redact_value_after(text, L"token=", L"[REDACTED_TOKEN]");

    std::size_t endpoint{};
    while ((endpoint = text.find(L"{0.0.0.", endpoint)) != std::wstring::npos) {
        const std::size_t first_close = text.find(L'}', endpoint);
        const std::size_t second_open = first_close == std::wstring::npos
            ? std::wstring::npos
            : text.find(L'{', first_close + 1U);
        const std::size_t second_close = second_open == std::wstring::npos
            ? std::wstring::npos
            : text.find(L'}', second_open + 1U);
        const std::size_t end = second_close == std::wstring::npos
            ? (first_close == std::wstring::npos ? text.size() : first_close + 1U)
            : second_close + 1U;
        text.replace(endpoint, end - endpoint, L"[REDACTED_ENDPOINT]");
        endpoint += std::size(L"[REDACTED_ENDPOINT]") - 1U;
    }

    for (wchar_t drive = L'A'; drive <= L'Z'; ++drive) {
        const std::wstring prefix{drive, L':', L'\\'};
        redact_path_prefix(text, prefix);
    }
    redact_path_prefix(text, L"\\\\");
    return text;
}

DiagnosticLog::DiagnosticLog(const std::size_t capacity)
    : capacity_(std::clamp<std::size_t>(capacity, 1, 4'096))
{
    entries_.reserve(capacity_);
}

void DiagnosticLog::record(
    const std::wstring_view component,
    const std::wstring_view message)
{
    DiagnosticEntry entry;
    entry.unix_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    entry.component = redact_diagnostic_text(component);
    entry.message = redact_diagnostic_text(message);

    std::scoped_lock lock(mutex_);
    entry.sequence = ++next_sequence_;
    if (entries_.size() == capacity_) {
        entries_.erase(entries_.begin());
    }
    entries_.push_back(std::move(entry));
}

std::vector<DiagnosticEntry> DiagnosticLog::snapshot() const
{
    std::scoped_lock lock(mutex_);
    return entries_;
}

bool DiagnosticLog::export_to(const std::filesystem::path& path) const noexcept
{
    try {
        const auto entries = snapshot();
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        output << "CD.404 diagnostic export\r\n"
                  "privacy: token/path/endpoint values are redacted; no media metadata is recorded\r\n";
        for (const auto& entry : entries) {
            const std::wstring line =
                std::to_wstring(entry.sequence) + L" " +
                std::to_wstring(entry.unix_time) + L" [" +
                redact_diagnostic_text(entry.component) + L"] " +
                redact_diagnostic_text(entry.message) + L"\r\n";
            const std::string encoded = wide_to_utf8(line);
            if (encoded.empty() && !line.empty()) {
                return false;
            }
            output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        }
        output.flush();
        return static_cast<bool>(output);
    } catch (...) {
        return false;
    }
}

} // namespace cd404::platform::windows
