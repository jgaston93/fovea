cmake_minimum_required(VERSION 3.24)

project(fovea
    VERSION 0.1.0
    DESCRIPTION "Real-time Vulkan image generator for geographic scenes"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FOVEA_BUILD_TESTS "Build the Fovea unit tests" ${PROJECT_IS_TOP_LEVEL})
option(FOVEA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
# Optimization of Fovea's own targets in Debug builds (D-008). -O1 keeps the
# assertions (no NDEBUG) and lets the ASan build run the suite in CI time.
set(FOVEA_DEBUG_OPTIMIZATION "-Og" CACHE STRING "Optimization flag for Fovea targets in Debug builds")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(FoveaCompilerFlags)
include(FoveaDependencies)

add_subdirectory(libs/core)
add_subdirectory(libs/terrain)
add_subdirectory(libs/platform)
add_subdirectory(libs/net)
add_subdirectory(libs/render)
add_subdirectory(apps/fovea)

if(FOVEA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
