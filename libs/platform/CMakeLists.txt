# Windows, monitors and input, on SDL3 (D-001), linked PRIVATE: no SDL types
# in the public headers.
add_library(fovea_platform STATIC
    src/monitors.cpp
    src/platform.cpp)
add_library(fovea::platform ALIAS fovea_platform)
target_include_directories(fovea_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fovea_platform PUBLIC fovea::core PRIVATE SDL3::SDL3-static)
fovea_target_defaults(fovea_platform)
