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
    unit/test_platform.cpp
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

# Library boundaries (architecture.md): the repository passes, and planted
# violations (a GPU header, a GDAL header in a .inl, a fovea library outside
# the graph, in .hpp/.inl/.h/.cpp files) are all caught.
add_test(NAME IncludeRules.Repository
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckIncludeRules.cmake)
add_test(NAME IncludeRules.CatchesPlantedViolations
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/include_rules
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckIncludeRules.cmake)
set_tests_properties(IncludeRules.CatchesPlantedViolations PROPERTIES
    PASS_REGULAR_EXPRESSION "core/bad.hpp: #include <vulkan/vulkan.h>.*terrain/src/bad.inl: #include <cpl_conv.h>.*net/include/bad.h: #include \"fovea/terrain/terrain_query.hpp\".*render/src/bad.cpp: #include \"fovea/net/host_adapter.hpp\"")

# The app on SDL's dummy display (no real monitors needed): it opens its
# window and exits after the time limit, rejects a config that needs more
# monitors than there are, and reports an unreadable config.
set(dummy_display "SDL_VIDEO_DRIVER=dummy")
add_test(NAME App.OpensItsWindowsAndExits
    COMMAND fovea --seconds 0.2 ${CMAKE_CURRENT_SOURCE_DIR}/data/one_channel.toml)
set_tests_properties(App.OpensItsWindowsAndExits PROPERTIES
    ENVIRONMENT ${dummy_display} TIMEOUT 20
    PASS_REGULAR_EXPRESSION "only +monitor 0 \\(window on 0, [0-9]+x[0-9]+ px\\)")
add_test(NAME App.RejectsAConfigNeedingMoreMonitors
    COMMAND fovea --seconds 0.2 ${PROJECT_SOURCE_DIR}/config/channels.example.toml)
set_tests_properties(App.RejectsAConfigNeedingMoreMonitors PROPERTIES
    ENVIRONMENT ${dummy_display} TIMEOUT 20
    PASS_REGULAR_EXPRESSION "channel \"center\" uses monitor 1, but there are only 1")
add_test(NAME App.ReportsABadConfig COMMAND fovea ${PROJECT_SOURCE_DIR}/config)
set_tests_properties(App.ReportsABadConfig PROPERTIES WILL_FAIL TRUE TIMEOUT 20)
add_test(NAME App.ListsMonitors COMMAND fovea --list-monitors)
set_tests_properties(App.ListsMonitors PROPERTIES
    ENVIRONMENT ${dummy_display} TIMEOUT 20 PASS_REGULAR_EXPRESSION "1 monitors \\(dummy\\)")
