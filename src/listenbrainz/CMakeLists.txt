add_library(cd404_listenbrainz STATIC)

target_sources(
    cd404_listenbrainz
    PRIVATE
        src/playback_tracker.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/cd404/listenbrainz/playback_tracker.hpp
)

target_compile_features(cd404_listenbrainz PUBLIC cxx_std_20)
target_link_libraries(cd404_listenbrainz PUBLIC cd404::core)
cd404_set_project_warnings(cd404_listenbrainz)

add_library(cd404::listenbrainz ALIAS cd404_listenbrainz)
