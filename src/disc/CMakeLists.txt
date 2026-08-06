add_library(cd404_disc STATIC)

target_sources(
    cd404_disc
    PRIVATE
        src/cd_text.cpp
        src/toc.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/cd404/disc/cd_text.hpp
            include/cd404/disc/toc.hpp
)

target_compile_features(cd404_disc PUBLIC cxx_std_20)
target_link_libraries(cd404_disc PUBLIC cd404::core)
cd404_set_project_warnings(cd404_disc)

add_library(cd404::disc ALIAS cd404_disc)
