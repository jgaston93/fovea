find_package(Threads REQUIRED)

# Math, time, logging, config and the thread-handoff containers
# (architecture.md).
# No Vulkan, SDL or GDAL here, ever.
add_library(fovea_core STATIC
    src/attitude.cpp
    src/config.cpp
    src/geodesy.cpp
    src/log.cpp
    src/projection.cpp
    src/time.cpp)
add_library(fovea::core ALIAS fovea_core)

target_include_directories(fovea_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fovea_core PUBLIC fovea_glm Threads::Threads PRIVATE fovea_tomlplusplus)
fovea_target_defaults(fovea_core)
