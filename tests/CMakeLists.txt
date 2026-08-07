add_executable(cd404_tests cd404_tests.cpp)

target_compile_features(cd404_tests PRIVATE cxx_std_20)
target_link_libraries(cd404_tests PRIVATE cd404::audio cd404::core cd404::disc)
cd404_set_project_warnings(cd404_tests)

add_test(NAME cd404_tests COMMAND cd404_tests)

if(WIN32)
    add_executable(cd404_windows_tests cd404_windows_tests.cpp)
    target_compile_features(cd404_windows_tests PRIVATE cxx_std_20)
    target_link_libraries(cd404_windows_tests PRIVATE cd404::windows)
    cd404_set_project_warnings(cd404_windows_tests)

    add_test(NAME cd404_windows_tests COMMAND cd404_windows_tests)
endif()
