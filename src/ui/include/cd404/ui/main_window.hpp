#pragma once

#include <windows.h>

#include <cd404/ui/application_launch.hpp>

namespace cd404::ui {

[[nodiscard]] int run_application(
    HINSTANCE instance,
    int show_command,
    ApplicationLaunchOptions options = {});

} // namespace cd404::ui
