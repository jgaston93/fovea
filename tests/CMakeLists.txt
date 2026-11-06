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

# A time limit on every test: some open (dummy) windows, which can hang.
gtest_discover_tests(fovea_unit_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 60)

# GPU tests, in their own binary (they load a Vulkan driver at run time).
# The allocation counter must replace every operator new/delete form here:
# lavapipe's LLVM uses the aligned nothrow one, and a missing form paired its
# allocations with the wrong deallocator (ASan's alloc-dealloc-mismatch).
add_executable(fovea_gpu_tests
    support/alloc_counter.cpp
    unit/test_frame_loop.cpp
    unit/test_renderer.cpp)
target_include_directories(fovea_gpu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fovea_gpu_tests PRIVATE fovea::render GTest::gtest_main)
fovea_target_defaults(fovea_gpu_tests)
gtest_discover_tests(fovea_gpu_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)

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
    COMMAND fovea --seconds 0.2 --identify ${CMAKE_CURRENT_SOURCE_DIR}/data/one_channel.toml)
set_tests_properties(App.OpensItsWindowsAndExits PROPERTIES
    ENVIRONMENT ${dummy_display} TIMEOUT 20
    PASS_REGULAR_EXPRESSION "window \"only\": on monitor 0, [0-9]+x[0-9]+ px")
add_test(NAME App.IdentifyFillsTheWindows
    COMMAND fovea --seconds 0.2 --identify ${CMAKE_CURRENT_SOURCE_DIR}/data/one_channel.toml)
set_tests_properties(App.IdentifyFillsTheWindows PROPERTIES
    ENVIRONMENT ${dummy_display} TIMEOUT 20
    PASS_REGULAR_EXPRESSION "Identifying: red" FAIL_REGULAR_EXPRESSION "cannot fill")
# Offscreen rendering on Vulkan with no display at all: the clear colour
# comes back from the GPU. Skipped where there is no Vulkan driver, unless
# FOVEA_REQUIRE_VULKAN is set when CMake configures (CI sets it for every
# step); ctest can't read the variable at run time for this decision.
add_test(NAME App.OffscreenRendersWithoutADisplay
    COMMAND fovea --clear ${CMAKE_CURRENT_SOURCE_DIR}/data/offscreen.toml)
set_tests_properties(App.OffscreenRendersWithoutADisplay PROPERTIES
    ENVIRONMENT "SDL_VIDEO_DRIVER=no-such-driver" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "offscreen \"only\": 1024x768, middle rgb\\(200,30,30\\).*0 validation messages"
    FAIL_REGULAR_EXPRESSION "cannot start SDL")
add_test(NAME App.OffscreenRendersTheScene
    COMMAND fovea ${CMAKE_CURRENT_SOURCE_DIR}/data/offscreen.toml)
set_tests_properties(App.OffscreenRendersTheScene PROPERTIES
    ENVIRONMENT "SDL_VIDEO_DRIVER=no-such-driver" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "offscreen \"only\": 1024x768, middle rgb.*0 validation messages")
if(NOT DEFINED ENV{FOVEA_REQUIRE_VULKAN})
    set_tests_properties(App.OffscreenRendersWithoutADisplay App.OffscreenRendersTheScene PROPERTIES
        SKIP_REGULAR_EXPRESSION "no Vulkan loader found|no GPU with Vulkan 1.3")
endif()
add_test(NAME App.RejectsABadSecondsValue
    COMMAND fovea --seconds -1 ${CMAKE_CURRENT_SOURCE_DIR}/data/one_channel.toml)
set_tests_properties(App.RejectsABadSecondsValue PROPERTIES
    ENVIRONMENT ${dummy_display} TIMEOUT 20
    PASS_REGULAR_EXPRESSION "--seconds needs a positive number")
add_test(NAME App.RejectsNanSeconds
    COMMAND fovea --seconds nan ${CMAKE_CURRENT_SOURCE_DIR}/data/one_channel.toml)
set_tests_properties(App.RejectsNanSeconds PROPERTIES
    ENVIRONMENT ${dummy_display} TIMEOUT 20 PASS_REGULAR_EXPRESSION "--seconds needs a positive number")
add_test(NAME App.RejectsAnOverlongSecondsValue
    COMMAND fovea --seconds 3e6 ${CMAKE_CURRENT_SOURCE_DIR}/data/one_channel.toml)
set_tests_properties(App.RejectsAnOverlongSecondsValue PROPERTIES
    ENVIRONMENT ${dummy_display} TIMEOUT 20 PASS_REGULAR_EXPRESSION "up to 604800")
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
