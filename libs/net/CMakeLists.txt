# Host protocol adapters. Writes SceneState only; never includes render
# headers. Header-only until the CIGI codec arrives in milestone 2.
add_library(fovea_net INTERFACE)
add_library(fovea::net ALIAS fovea_net)
target_include_directories(fovea_net INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fovea_net INTERFACE fovea::core)
