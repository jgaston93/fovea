# Views, presenters, modules and (from step 5) the Vulkan backend. Reads
# SceneState only; never includes net headers.
add_library(fovea_render STATIC
    src/view.cpp)
add_library(fovea::render ALIAS fovea_render)
target_include_directories(fovea_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fovea_render PUBLIC fovea::core fovea::platform)
fovea_target_defaults(fovea_render)
