# Models read from glTF binary files (D-023). CPU-only: depends on core
# alone, like terrain, so it's tested without a GPU and a host can use it.
add_library(fovea_models STATIC
    src/model.cpp)
add_library(fovea::models ALIAS fovea_models)
target_include_directories(fovea_models PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fovea_models PUBLIC fovea::core PRIVATE fovea_cgltf)
fovea_target_defaults(fovea_models)
