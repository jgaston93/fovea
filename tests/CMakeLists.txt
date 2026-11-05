include(GoogleTest)

add_executable(fovea_unit_tests
    support/alloc_counter.cpp
    unit/test_alloc_counter.cpp
    unit/test_attitude.cpp
    unit/test_config.cpp
    unit/test_containers.cpp
    unit/test_geodesy.cpp
    unit/test_log.cpp
    unit/test_monitors.cpp
    unit/test_projection.cpp
    unit/test_scene.cpp
    unit/test_terrain.cpp
    unit/test_time.cpp
    unit/test_toolchain.cpp
    unit/test_view.cpp)
target_include_directories(fovea_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fovea_unit_tests PRIVATE fovea::core fovea::terrain fovea::platform
                                               fovea::net fovea::render GTest::gtest_main)
target_compile_definitions(fovea_unit_tests PRIVATE FOVEA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
fovea_target_defaults(fovea_unit_tests)

gtest_discover_tests(fovea_unit_tests DISCOVERY_MODE PRE_TEST)

# Library boundaries (architecture.md): the repository passes, and a planted
# violation in each of two libraries is caught.
add_test(NAME IncludeRules.Repository
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckIncludeRules.cmake)
add_test(NAME IncludeRules.CatchesPlantedViolations
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/include_rules
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckIncludeRules.cmake)
set_tests_properties(IncludeRules.CatchesPlantedViolations PROPERTIES
    PASS_REGULAR_EXPRESSION "libs/core/include/fovea/core/bad.hpp: #include <vulkan/vulkan.h>.*libs/render/src/bad.cpp: #include \"fovea/net/host_adapter.hpp\"")

# The app loads the example config, and reports a bad one.
add_test(NAME App.LoadsTheExampleConfig
    COMMAND fovea ${PROJECT_SOURCE_DIR}/config/channels.example.toml)
set_tests_properties(App.LoadsTheExampleConfig PROPERTIES
    PASS_REGULAR_EXPRESSION "3 views at 60 Hz")
add_test(NAME App.ReportsABadConfig COMMAND fovea ${PROJECT_SOURCE_DIR}/config)
set_tests_properties(App.ReportsABadConfig PROPERTIES WILL_FAIL TRUE)
