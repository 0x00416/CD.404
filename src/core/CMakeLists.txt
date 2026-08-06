add_library(cd404_core STATIC)

target_sources(
    cd404_core
    PRIVATE
        src/cd_time.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/cd404/core/cd_time.hpp
)

target_compile_features(cd404_core PUBLIC cxx_std_20)
cd404_set_project_warnings(cd404_core)

add_library(cd404::core ALIAS cd404_core)
