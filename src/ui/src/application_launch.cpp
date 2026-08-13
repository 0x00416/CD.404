#include <cd404/ui/application_launch.hpp>

#include <cwctype>
#include <cwchar>

namespace cd404::ui {

std::optional<std::wstring> normalize_autoplay_drive_root(
    const std::wstring_view argument)
{
    std::size_t first{};
    while (first < argument.size() && std::iswspace(argument[first]) != 0) {
        ++first;
    }
    std::size_t last = argument.size();
    while (last > first && std::iswspace(argument[last - 1U]) != 0) {
        --last;
    }
    const std::wstring_view value = argument.substr(first, last - first);
    if (value.size() < 2U || value.size() > 3U ||
        std::iswalpha(value[0]) == 0 || value[1] != L':' ||
        (value.size() == 3U && value[2] != L'\\' && value[2] != L'/')) {
        return std::nullopt;
    }
    wchar_t letter = value[0];
    if (letter >= L'a' && letter <= L'z') {
        letter = static_cast<wchar_t>(letter - L'a' + L'A');
    }
    return std::wstring{letter, L':', L'\\'};
}

ApplicationLaunchOptions parse_application_launch_options(
    const std::span<const std::wstring_view> arguments)
{
    ApplicationLaunchOptions options;
    for (std::size_t index = 1U; index < arguments.size(); ++index) {
        if (_wcsicmp(std::wstring(arguments[index]).c_str(), L"/autoplay") != 0) {
            continue;
        }
        if (index + 1U < arguments.size()) {
            options.autoplay_drive_root = normalize_autoplay_drive_root(
                arguments[index + 1U]);
        }
        break;
    }
    return options;
}

} // namespace cd404::ui
