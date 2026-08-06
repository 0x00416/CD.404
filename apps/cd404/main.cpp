#include <windows.h>

#include <cd404/ui/main_window.hpp>

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    wchar_t*,
    const int show_command)
{
    return cd404::ui::run_application(instance, show_command);
}
