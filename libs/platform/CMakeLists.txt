# Windows, monitors, input and timers. SDL3 joins in step 4, linked PRIVATE.
add_library(fovea_platform STATIC
    src/monitors.cpp)
add_library(fovea::platform ALIAS fovea_platform)
target_include_directories(fovea_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fovea_platform PUBLIC fovea::core)
fovea_target_defaults(fovea_platform)
