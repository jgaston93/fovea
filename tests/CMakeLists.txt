include(GoogleTest)

add_executable(fovea_unit_tests
    support/alloc_counter.cpp
    unit/test_alloc_counter.cpp
    unit/test_attitude.cpp
    unit/test_config.cpp
    unit/test_containers.cpp
    unit/test_geodesy.cpp
    unit/test_log.cpp
    unit/test_projection.cpp
    unit/test_time.cpp
    unit/test_toolchain.cpp)
target_include_directories(fovea_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fovea_unit_tests PRIVATE fovea::core GTest::gtest_main)
target_compile_definitions(fovea_unit_tests PRIVATE FOVEA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
fovea_target_defaults(fovea_unit_tests)

gtest_discover_tests(fovea_unit_tests DISCOVERY_MODE PRE_TEST)
