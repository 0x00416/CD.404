add_executable(cd404_app WIN32 main.cpp)

target_compile_features(cd404_app PRIVATE cxx_std_20)
target_compile_definitions(cd404_app PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(cd404_app PRIVATE cd404::ui)
set_target_properties(cd404_app PROPERTIES OUTPUT_NAME "CD.404")
cd404_set_project_warnings(cd404_app)
