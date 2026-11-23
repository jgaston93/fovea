include(GoogleTest)

add_executable(fovea_unit_tests
    support/alloc_counter.cpp
    support/glb.cpp
    support/terrain_db.cpp
    unit/test_alloc_counter.cpp
    unit/test_attitude.cpp
    unit/test_cigi.cpp
    unit/test_cigi_adapter.cpp
    unit/test_cigi_test_host.cpp
    unit/test_particle_noise.cpp
    unit/test_particle_pack.cpp
    unit/test_puff_stream.cpp
    unit/test_config.cpp
    unit/test_observer.cpp
    unit/test_space_mouse.cpp
    unit/test_containers.cpp
    unit/test_celestial.cpp
    unit/test_geodesy.cpp
    unit/test_hat_hot.cpp
    unit/test_image_file.cpp
    unit/test_log.cpp
    unit/test_models.cpp
    unit/test_monitors.cpp
    unit/test_platform.cpp
    unit/test_projection.cpp
    unit/test_shadow_cascades.cpp
    unit/test_resolve.cpp
    unit/test_ride_entities.cpp
    unit/test_scene.cpp
    unit/test_terrain.cpp
    unit/test_terrain_database.cpp
    unit/test_terrain_cache.cpp
    unit/test_terrain_lod.cpp
    unit/test_time.cpp
    unit/test_toolchain.cpp
    unit/test_view.cpp)
target_include_directories(fovea_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fovea_unit_tests PRIVATE fovea::core fovea::terrain fovea::models fovea::platform
                                               fovea::net fovea::render fovea_cigi_test_host_lib
                                               GTest::gtest_main)
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
    support/atmosphere_reference.cpp
    support/glb.cpp
    unit/test_display_clock.cpp
    unit/test_frame_loop.cpp
    unit/test_golden.cpp
    unit/test_host_environment.cpp
    unit/test_particles_render.cpp
    unit/test_renderer.cpp
    unit/test_sky.cpp
    unit/test_terrain_render.cpp)
target_include_directories(fovea_gpu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fovea_gpu_tests PRIVATE fovea::render fovea::net fovea_cigi_test_host_lib
                                              GTest::gtest_main)
target_compile_definitions(fovea_gpu_tests PRIVATE FOVEA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
fovea_target_defaults(fovea_gpu_tests)
gtest_discover_tests(fovea_gpu_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)

# The terrain compiler, where it is built (GDAL found; apps/fovea-compiler).
if(TARGET fovea_compiler_lib)
    add_executable(fovea_compiler_tests support/glb.cpp unit/test_compiler.cpp
        unit/test_runway_lights.cpp)
    target_include_directories(fovea_compiler_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(fovea_compiler_tests PRIVATE fovea_compiler_lib GDAL::GDAL
                                                       GTest::gtest_main)
    target_compile_definitions(fovea_compiler_tests PRIVATE FOVEA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    fovea_target_defaults(fovea_compiler_tests)
    gtest_discover_tests(fovea_compiler_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 60)
endif()

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
# --headless with an on-screen config: no windows, one PNG per channel.
add_test(NAME App.HeadlessDumpsEachChannel
    COMMAND fovea --headless --frames 2 --dump headless-dump
        ${PROJECT_SOURCE_DIR}/config/channels.example.toml)
set_tests_properties(App.HeadlessDumpsEachChannel PROPERTIES
    ENVIRONMENT "SDL_VIDEO_DRIVER=no-such-driver" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "wrote headless-dump.left\\.png.*wrote headless-dump.center\\.png.*wrote headless-dump.right\\.png.*0 validation messages")
# --check (D-056): the config read and checked, nothing opened (no display
# or GPU), a good one said so; a bad one's error named, exit 1.
add_test(NAME App.CheckAcceptsAGoodConfig
    COMMAND fovea --check ${PROJECT_SOURCE_DIR}/config/channels.example.toml)
set_tests_properties(App.CheckAcceptsAGoodConfig PROPERTIES
    ENVIRONMENT "SDL_VIDEO_DRIVER=no-such-driver;VK_ICD_FILENAMES=/nonexistent;VK_DRIVER_FILES=/nonexistent"
    TIMEOUT 20 PASS_REGULAR_EXPRESSION "channels.example.toml is good: 3 channel\\(s\\)")
# ... with a host and a database (config/khsv.toml; its database isn't in a
# fresh checkout): neither opened.
add_test(NAME App.CheckOpensNoDatabaseOrSocket
    COMMAND fovea --check ${PROJECT_SOURCE_DIR}/config/khsv.toml)
set_tests_properties(App.CheckOpensNoDatabaseOrSocket PROPERTIES
    ENVIRONMENT "SDL_VIDEO_DRIVER=no-such-driver;VK_ICD_FILENAMES=/nonexistent;VK_DRIVER_FILES=/nonexistent"
    TIMEOUT 20 PASS_REGULAR_EXPRESSION "khsv.toml is good: [0-9]+ channel\\(s\\), a host")
add_test(NAME App.CheckRefusesABadConfig
    COMMAND fovea --check ${CMAKE_CURRENT_SOURCE_DIR}/data/check-bad.toml)
set_tests_properties(App.CheckRefusesABadConfig PROPERTIES
    TIMEOUT 20 WILL_FAIL TRUE)
add_test(NAME App.CheckNamesTheBadKey
    COMMAND fovea --check ${CMAKE_CURRENT_SOURCE_DIR}/data/check-bad.toml)
set_tests_properties(App.CheckNamesTheBadKey PROPERTIES
    TIMEOUT 20 PASS_REGULAR_EXPRESSION "check-bad.toml:[0-9]+.*particles.groups")
# Headless with a host (D-044): driven over CIGI by the test host, the last
# frame written (a host's tests, with no display).
add_test(NAME App.HeadlessFollowsAHost
    COMMAND ${CMAKE_COMMAND} -DFOVEA=$<TARGET_FILE:fovea> -DHOST=$<TARGET_FILE:cigi-test-host>
        -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/data/headless-host.toml
        -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/data/headless-host-script.toml
        -DOUT=headless-host-out -P ${CMAKE_CURRENT_SOURCE_DIR}/headless_host.cmake)
set_tests_properties(App.HeadlessFollowsAHost PROPERTIES
    ENVIRONMENT "SDL_VIDEO_DRIVER=no-such-driver" TIMEOUT 90)
# The host drives it: --frames refused (before rendering: no Vulkan needed);
# with --clear, the host isn't used, and says so.
add_test(NAME App.HeadlessWithAHostRefusesFrames
    COMMAND fovea --headless --frames 5 ${CMAKE_CURRENT_SOURCE_DIR}/data/headless-host.toml)
set_tests_properties(App.HeadlessWithAHostRefusesFrames PROPERTIES
    TIMEOUT 20 PASS_REGULAR_EXPRESSION "the host drives it: no --identify or --frames")
add_test(NAME App.HeadlessClearSaysTheHostIsntUsed
    COMMAND fovea --headless --clear ${CMAKE_CURRENT_SOURCE_DIR}/data/headless-host.toml)
set_tests_properties(App.HeadlessClearSaysTheHostIsntUsed PROPERTIES
    ENVIRONMENT "SDL_VIDEO_DRIVER=no-such-driver" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "note: \\[host\\] isn't used with --clear")
add_test(NAME App.HeadlessRefusesIdentify
    COMMAND fovea --headless --identify ${CMAKE_CURRENT_SOURCE_DIR}/data/one_channel.toml)
set_tests_properties(App.HeadlessRefusesIdentify PROPERTIES
    TIMEOUT 20 PASS_REGULAR_EXPRESSION "--identify are for on-screen runs")
# The dump directory is a file: refused before rendering (no Vulkan needed).
add_test(NAME App.DumpIntoAFileFailsAtOnce
    COMMAND fovea --headless --dump ${CMAKE_CURRENT_SOURCE_DIR}/data/one_channel.toml
        ${CMAKE_CURRENT_SOURCE_DIR}/data/one_channel.toml)
set_tests_properties(App.DumpIntoAFileFailsAtOnce PROPERTIES
    TIMEOUT 20 PASS_REGULAR_EXPRESSION "cannot make the directory .*one_channel.toml")
add_test(NAME App.DumpNeedsHeadless
    COMMAND fovea --dump out ${CMAKE_CURRENT_SOURCE_DIR}/data/one_channel.toml)
set_tests_properties(App.DumpNeedsHeadless PROPERTIES
    TIMEOUT 20 PASS_REGULAR_EXPRESSION "--frames and --dump need --headless")
if(NOT DEFINED ENV{FOVEA_REQUIRE_VULKAN})
    set_tests_properties(App.OffscreenRendersWithoutADisplay App.OffscreenRendersTheScene
        App.HeadlessDumpsEachChannel App.HeadlessFollowsAHost App.HeadlessClearSaysTheHostIsntUsed
        PROPERTIES
        SKIP_REGULAR_EXPRESSION "no Vulkan loader found|no GPU with Vulkan 1.3")
endif()
add_test(NAME App.StatsNeedsTheScene
    COMMAND fovea --clear --stats out.csv ${CMAKE_CURRENT_SOURCE_DIR}/data/one_channel.toml)
set_tests_properties(App.StatsNeedsTheScene PROPERTIES
    TIMEOUT 20 PASS_REGULAR_EXPRESSION "--stats and --overlay time the scene")
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

# libFuzzer targets (the `fuzz` preset, Clang only); run nightly.
option(FOVEA_FUZZ "Build the libFuzzer targets" OFF)
if(FOVEA_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "FOVEA_FUZZ needs Clang (libFuzzer); the fuzz preset sets it")
    endif()
    add_executable(fovea_fuzz_cigi fuzz/fuzz_cigi.cpp)
    target_link_libraries(fovea_fuzz_cigi PRIVATE fovea::net)
    target_link_options(fovea_fuzz_cigi PRIVATE -fsanitize=fuzzer)
    fovea_target_defaults(fovea_fuzz_cigi)
    add_executable(fovea_fuzz_puff_stream fuzz/fuzz_puff_stream.cpp)
    target_link_libraries(fovea_fuzz_puff_stream PRIVATE fovea::net)
    target_link_options(fovea_fuzz_puff_stream PRIVATE -fsanitize=fuzzer)
    fovea_target_defaults(fovea_fuzz_puff_stream)
endif()
