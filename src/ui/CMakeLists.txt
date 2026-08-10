add_library(cd404_ui STATIC)

target_sources(
    cd404_ui
    PRIVATE
        src/disc_snapshot.cpp
        src/main_window.cpp
        src/metadata_source_model.cpp
        src/playback_presenter.cpp
        src/settings_model.cpp
        src/theme.cpp
        src/ui_drawing.cpp
        src/ui_layout.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/cd404/ui/main_window.hpp
            include/cd404/ui/metadata_source_model.hpp
            include/cd404/ui/playback_presenter.hpp
            include/cd404/ui/settings_model.hpp
            include/cd404/ui/theme.hpp
)

target_compile_features(cd404_ui PUBLIC cxx_std_20)
target_compile_definitions(cd404_ui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(
    cd404_ui
    PUBLIC
        cd404::windows
    PRIVATE
        d2d1
        comctl32
        comdlg32
        dwrite
        dwmapi
        user32
        oleacc
        uxtheme
        windowscodecs
)
cd404_set_project_warnings(cd404_ui)

add_library(cd404::ui ALIAS cd404_ui)
