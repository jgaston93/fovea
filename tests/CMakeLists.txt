include(GoogleTest)

add_executable(fovea_unit_tests
    unit/test_toolchain.cpp)
target_include_directories(fovea_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fovea_unit_tests PRIVATE GTest::gtest_main)
fovea_target_defaults(fovea_unit_tests)

gtest_discover_tests(fovea_unit_tests DISCOVERY_MODE PRE_TEST)
