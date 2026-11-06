# Host protocol adapters. Writes SceneState only; never includes render
# headers. The CIGI 3.3 codec (D-013, D-014) is here.
add_library(fovea_net STATIC
    src/cigi.cpp
    src/cigi_adapter.cpp
    src/host_thread.cpp
    src/udp_socket.cpp)
add_library(fovea::net ALIAS fovea_net)
target_include_directories(fovea_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fovea_net PUBLIC fovea::core)
fovea_target_defaults(fovea_net)
if(WIN32)
    target_link_libraries(fovea_net PRIVATE ws2_32)
endif()
