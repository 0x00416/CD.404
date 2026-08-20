#include <cd404/platform/windows/bundled_font.hpp>

#include <windows.h>

#include <utility>
#include <vector>

namespace cd404::platform::windows {

std::filesystem::path bundled_font_path()
{
    std::vector<wchar_t> buffer(512U, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (written == 0U) {
            return {};
        }
        if (written + 1U < buffer.size()) {
            return std::filesystem::path(
                std::wstring(buffer.data(), written)).parent_path() /
                L"fonts" / kBundledFontFileName;
        }
        if (buffer.size() >= 32'768U) {
            return {};
        }
        buffer.resize(buffer.size() * 2U, L'\0');
    }
}

BundledFontRegistration::BundledFontRegistration()
    : BundledFontRegistration(bundled_font_path())
{
}

BundledFontRegistration::BundledFontRegistration(std::filesystem::path font_path)
    : path_(std::move(font_path))
{
    if (!path_.empty()) {
        loaded_resources_ = AddFontResourceExW(
            path_.c_str(),
            FR_PRIVATE,
            nullptr);
    }
}

BundledFontRegistration::~BundledFontRegistration()
{
    if (loaded_resources_ > 0 && !path_.empty()) {
        static_cast<void>(RemoveFontResourceExW(
            path_.c_str(),
            FR_PRIVATE,
            nullptr));
    }
}

bool BundledFontRegistration::loaded() const noexcept
{
    return loaded_resources_ > 0;
}

const std::filesystem::path& BundledFontRegistration::path() const noexcept
{
    return path_;
}

} // namespace cd404::platform::windows
