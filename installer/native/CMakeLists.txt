set(CD404_APPLICATION_RESOURCE "$<TARGET_FILE:cd404_app>")
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/docs/PRIVACY.md" CD404_PRIVACY_RESOURCE)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" CD404_NOTICES_RESOURCE)
configure_file(resources.rc.in resources.rc.in.generated @ONLY)
file(
    GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/resources-$<CONFIG>.rc"
    INPUT "${CMAKE_CURRENT_BINARY_DIR}/resources.rc.in.generated"
)

add_executable(
    cd404_installer
    WIN32
    main.cpp
    wizard.cpp
    wizard.hpp
    "${CMAKE_CURRENT_BINARY_DIR}/resources-$<CONFIG>.rc"
)
add_dependencies(cd404_installer cd404_app)
target_compile_features(cd404_installer PRIVATE cxx_std_20)
target_compile_definitions(
    cd404_installer
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        CD404_VERSION_WIDE=L"${PROJECT_VERSION}-public-beta.1"
)
target_include_directories(cd404_installer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
    cd404_installer
    PRIVATE
        comctl32
        advapi32
        dwmapi
        ole32
        shell32
        user32
        uuid
        uxtheme
)
set_target_properties(
    cd404_installer
    PROPERTIES OUTPUT_NAME "CD.404-${PROJECT_VERSION}-public-beta.1-x64-setup"
)
add_custom_command(
    TARGET cd404_installer
    POST_BUILD
    COMMAND
        "${CMAKE_MT}" -nologo
        -manifest "${CMAKE_CURRENT_SOURCE_DIR}/installer.manifest"
        "-outputresource:$<TARGET_FILE:cd404_installer>;#1"
    VERBATIM
    COMMENT "Embedding CD.404 installer manifest"
)
cd404_set_project_warnings(cd404_installer)
