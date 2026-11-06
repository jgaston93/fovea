# The scripted CIGI 3.3 test host (milestone 2 step 4). The library holds
# what the tests use too: the flight, the script reader and the session.
add_library(fovea_cigi_test_host_lib STATIC
    flight.cpp
    script.cpp
    session.cpp)
target_include_directories(fovea_cigi_test_host_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fovea_cigi_test_host_lib PUBLIC fovea::core fovea::net
    PRIVATE fovea_tomlplusplus)
fovea_target_defaults(fovea_cigi_test_host_lib)

add_executable(cigi-test-host main.cpp)
target_link_libraries(cigi-test-host PRIVATE fovea_cigi_test_host_lib)
fovea_target_defaults(cigi-test-host)
