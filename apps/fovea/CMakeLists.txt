add_executable(fovea main.cpp)
target_link_libraries(fovea PRIVATE fovea::core fovea::terrain fovea::platform fovea::net
                                    fovea::render)
fovea_target_defaults(fovea)
