# Terrain database reading and spatial queries. CPU-only: depends on core
# alone, so a host can link it (foundational decision 1).
add_library(fovea_terrain STATIC
    src/database.cpp
    src/height_query_thread.cpp
    src/terrain_query.cpp
    src/tile_file.cpp
    src/tile_key.cpp)
add_library(fovea::terrain ALIAS fovea_terrain)
target_include_directories(fovea_terrain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fovea_terrain PUBLIC fovea::core PRIVATE fovea_tomlplusplus)
fovea_target_defaults(fovea_terrain)
