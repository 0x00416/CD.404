configure_file(resources.rc.in resources.rc @ONLY)

add_executable(
    cd404_app
    WIN32
    main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/resources.rc"
)

target_compile_features(cd404_app PRIVATE cxx_std_20)
target_compile_definitions(cd404_app PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(cd404_app PRIVATE cd404::ui shell32)
set_target_properties(cd404_app PROPERTIES OUTPUT_NAME "CD.404")
add_custom_command(
    TARGET cd404_app
    POST_BUILD
    COMMAND
        "${CMAKE_MT}" -nologo
        -manifest "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
        "-outputresource:$<TARGET_FILE:cd404_app>;#1"
    VERBATIM
    COMMENT "Embedding CD.404 application manifest"
)
cd404_set_project_warnings(cd404_app)
