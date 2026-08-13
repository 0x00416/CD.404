#include <windows.h>

#include <shellapi.h>

#include <cd404/ui/main_window.hpp>

#include <span>
#include <string_view>
#include <vector>

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    wchar_t*,
    const int show_command)
{
    int count{};
    wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring_view> arguments;
    if (values != nullptr && count > 0) {
        arguments.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            arguments.emplace_back(values[index]);
        }
    }
    const auto options = cd404::ui::parse_application_launch_options(arguments);
    if (values != nullptr) {
        LocalFree(values);
    }
    return cd404::ui::run_application(instance, show_command, options);
}
