add_library(cd404_windows STATIC)

target_sources(
    cd404_windows
    PRIVATE
        src/optical_drive.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/cd404/platform/windows/optical_drive.hpp
)

target_compile_features(cd404_windows PUBLIC cxx_std_20)
target_compile_definitions(cd404_windows PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(cd404_windows PUBLIC cd404::disc PRIVATE kernel32)
cd404_set_project_warnings(cd404_windows)

add_library(cd404::windows ALIAS cd404_windows)
