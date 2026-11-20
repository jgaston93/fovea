# Headless with a host (D-044): Fovea headless, driven over CIGI by the test
# host, at once (a pipeline: both start together; the host's output is
# Fovea's ignored input). Fovea must have taken the host's entity updates,
# answered each frame with a Start of Frame, and written its last frame.
#   cmake -DFOVEA=... -DHOST=... -DCONFIG=... -DSCRIPT=... -DOUT=... -P headless_host.cmake
file(REMOVE_RECURSE ${OUT})
execute_process(
    COMMAND ${HOST} --seconds 6 ${SCRIPT}
    COMMAND ${FOVEA} --headless --seconds 4 --dump ${OUT} ${CONFIG}
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULTS_VARIABLE codes TIMEOUT 60)
message("${out}${err}")
list(GET codes 1 fovea_code)
if(NOT fovea_code EQUAL 0)
    message(FATAL_ERROR "fovea exited ${fovea_code}")
endif()
if(NOT out MATCHES "CIGI: [0-9]+ messages \\(0 rejected[^\n]*, ([1-9][0-9]*) entity updates, ([1-9][0-9]*) Start of Frame sent")
    message(FATAL_ERROR "no entity updates taken, or no Start of Frame sent")
endif()
if(CMAKE_MATCH_1 LESS 60)
    message(FATAL_ERROR "only ${CMAKE_MATCH_1} entity updates in 4 s")
endif()
if(NOT out MATCHES "0 validation messages")
    message(FATAL_ERROR "validation messages")
endif()
if(NOT EXISTS ${OUT}/eye.png)
    message(FATAL_ERROR "no ${OUT}/eye.png")
endif()
