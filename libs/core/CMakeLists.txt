find_package(Threads REQUIRED)

# Math, time, logging, config and the thread-handoff containers
# (architecture.md).
# No Vulkan, SDL or GDAL here, ever.
add_library(fovea_core STATIC
    src/attitude.cpp
    src/celestial.cpp
    src/colour.cpp
    src/config.cpp
    src/file.cpp
    src/geodesy.cpp
    src/image_file.cpp
    src/ipv4.cpp
    src/stb_implementation.cpp
    src/log.cpp
    src/projection.cpp
    src/time.cpp)
add_library(fovea::core ALIAS fovea_core)

target_include_directories(fovea_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fovea_core PUBLIC fovea_glm Threads::Threads PRIVATE fovea_tomlplusplus fovea_stb)
fovea_target_defaults(fovea_core)
