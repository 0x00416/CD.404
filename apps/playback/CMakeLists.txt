add_executable(cd404_playback main.cpp)

target_compile_features(cd404_playback PRIVATE cxx_std_20)
target_compile_definitions(cd404_playback PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(cd404_playback PRIVATE cd404::windows)
set_target_properties(cd404_playback PROPERTIES OUTPUT_NAME "CD.404.Playback")
cd404_set_project_warnings(cd404_playback)
