add_library(cd404_ui STATIC)

target_sources(
    cd404_ui
    PRIVATE
        src/main_window.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/cd404/ui/main_window.hpp
)

target_compile_features(cd404_ui PUBLIC cxx_std_20)
target_compile_definitions(cd404_ui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(
    cd404_ui
    PUBLIC
        cd404::windows
    PRIVATE
        d2d1
        dwrite
        dwmapi
        user32
)
cd404_set_project_warnings(cd404_ui)

add_library(cd404::ui ALIAS cd404_ui)
