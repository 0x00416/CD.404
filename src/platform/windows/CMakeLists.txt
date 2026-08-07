add_library(cd404_windows STATIC)

target_sources(
    cd404_windows
    PRIVATE
        src/cdda_playback_engine.cpp
        src/optical_drive.cpp
        src/http_client.cpp
        src/gnudb_client.cpp
        src/itunes_client.cpp
        src/musicbrainz_client.cpp
        src/online_metadata.cpp
        src/raw_cdda_sector_source.cpp
        src/wasapi_output.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/cd404/platform/windows/cdda_playback_engine.hpp
            include/cd404/platform/windows/optical_drive.hpp
            include/cd404/platform/windows/gnudb_client.hpp
            include/cd404/platform/windows/itunes_client.hpp
            include/cd404/platform/windows/musicbrainz_client.hpp
            include/cd404/platform/windows/online_metadata.hpp
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
        shell32
        shlwapi
        uuid
        winhttp
        windowsapp
        xmllite
)
cd404_set_project_warnings(cd404_windows)

add_library(cd404::windows ALIAS cd404_windows)
