# The offline terrain compiler (D-019): the only user of GDAL, built on
# Linux where a system GDAL is found (libgdal-dev on Ubuntu). Elsewhere it's
# skipped, unless FOVEA_REQUIRE_GDAL is set when CMake configures (CI sets
# it for the Ubuntu jobs).
if(WIN32)
    return()
endif()
if(DEFINED ENV{FOVEA_REQUIRE_GDAL})
    find_package(GDAL 3.8 CONFIG REQUIRED GLOBAL)  # GLOBAL: the tests link it too
else()
    find_package(GDAL 3.8 CONFIG QUIET GLOBAL)
endif()
if(NOT GDAL_FOUND)
    message(STATUS "fovea-compiler: no GDAL 3.8 or later found; not built")
    return()
endif()

# The library holds what the tests use too.
add_library(fovea_compiler_lib STATIC
    compile.cpp
    elevation.cpp
    imagery.cpp
    runways.cpp
    settings.cpp
    texture.cpp)
target_include_directories(fovea_compiler_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fovea_compiler_lib PUBLIC fovea::core fovea::terrain
    PRIVATE GDAL::GDAL fovea_tomlplusplus fovea_bc7enc)
fovea_target_defaults(fovea_compiler_lib)

add_executable(fovea-compiler main.cpp)
target_link_libraries(fovea-compiler PRIVATE fovea_compiler_lib)
fovea_target_defaults(fovea-compiler)
