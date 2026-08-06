add_library(cd404_audio STATIC)

target_sources(
    cd404_audio
    PRIVATE
        src/continuous_cdda_stream.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/cd404/audio/cdda_sector_source.hpp
            include/cd404/audio/continuous_cdda_stream.hpp
)

target_compile_features(cd404_audio PUBLIC cxx_std_20)
target_link_libraries(cd404_audio PUBLIC cd404::core)
cd404_set_project_warnings(cd404_audio)

add_library(cd404::audio ALIAS cd404_audio)
