add_library(cd404_windows STATIC)

target_sources(
    cd404_windows
    PRIVATE
        src/cdda_playback_engine.cpp
        src/device_lifecycle.cpp
        src/optical_drive.cpp
        src/http_client.cpp
        src/gnudb_client.cpp
        src/itunes_client.cpp
        src/listenbrainz_reporter.cpp
        src/listenbrainz_queue.cpp
        src/metadata_store.cpp
        src/musicbrainz_client.cpp
        src/online_metadata.cpp
        src/raw_cdda_sector_source.cpp
        src/system_media_controls.cpp
        src/user_settings.cpp
        src/wasapi_output.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/cd404/platform/windows/cdda_playback_engine.hpp
            include/cd404/platform/windows/device_lifecycle.hpp
            include/cd404/platform/windows/optical_drive.hpp
            include/cd404/platform/windows/gnudb_client.hpp
            include/cd404/platform/windows/http_client.hpp
            include/cd404/platform/windows/itunes_client.hpp
            include/cd404/platform/windows/listenbrainz_reporter.hpp
            include/cd404/platform/windows/listenbrainz_queue.hpp
            include/cd404/platform/windows/metadata_store.hpp
            include/cd404/platform/windows/musicbrainz_client.hpp
            include/cd404/platform/windows/online_metadata.hpp
            include/cd404/platform/windows/raw_cdda_sector_source.hpp
            include/cd404/platform/windows/system_media_controls.hpp
            include/cd404/platform/windows/user_settings.hpp
            include/cd404/platform/windows/wasapi_output.hpp
)

target_compile_features(cd404_windows PUBLIC cxx_std_20)
target_compile_definitions(cd404_windows PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(
    cd404_windows
    PUBLIC
        cd404::audio
        cd404::disc
        cd404::listenbrainz
    PRIVATE
        kernel32
        advapi32
        ole32
        propsys
        shell32
        shlwapi
        uuid
        winhttp
        winsqlite3
        windowsapp
        xmllite
)
cd404_set_project_warnings(cd404_windows)

add_library(cd404::windows ALIAS cd404_windows)
