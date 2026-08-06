add_library(cd404_windows STATIC)

target_sources(
    cd404_windows
    PRIVATE
        src/optical_drive.cpp
        src/raw_cdda_sector_source.cpp
        src/wasapi_output.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/cd404/platform/windows/optical_drive.hpp
            include/cd404/platform/windows/raw_cdda_sector_source.hpp
            include/cd404/platform/windows/wasapi_output.hpp
)

target_compile_features(cd404_windows PUBLIC cxx_std_20)
target_compile_definitions(cd404_windows PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(
    cd404_windows
    PUBLIC
        cd404::audio
        cd404::disc
    PRIVATE
        kernel32
        ole32
        uuid
)
cd404_set_project_warnings(cd404_windows)

add_library(cd404::windows ALIAS cd404_windows)
