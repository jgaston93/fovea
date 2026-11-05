# Checks the library boundaries in architecture.md ("Library dependency
# graph") by scanning #include lines. Run by ctest:
#   cmake -DROOT=<repo> -P cmake/CheckIncludeRules.cmake
# The target links enforce the same graph for linking; this catches a header
# included through a path the links don't stop.

if(NOT ROOT)
    message(FATAL_ERROR "set -DROOT=<repository root>")
endif()

set(third_party_gpu "vulkan/|volk\\.h|vk_mem_alloc|VkBootstrap|SDL")

# Library directory, then the include paths it must not use.
set(rule_core "${third_party_gpu}|gdal|fovea/(terrain|platform|net|render)/")
set(rule_terrain "${third_party_gpu}|gdal|fovea/(platform|net|render)/")
set(rule_net "${third_party_gpu}|gdal|fovea/(platform|render)/")
set(rule_platform "gdal|fovea/(net|render)/")
set(rule_render "gdal|fovea/net/")

set(violations "")
foreach(lib IN ITEMS core terrain net platform render)
    file(GLOB_RECURSE sources "${ROOT}/libs/${lib}/*.hpp" "${ROOT}/libs/${lib}/*.cpp")
    foreach(source IN LISTS sources)
        file(STRINGS "${source}" lines REGEX "^[ \t]*#[ \t]*include")
        foreach(line IN LISTS lines)
            if(line MATCHES "#[ \t]*include[ \t]*[<\"](${rule_${lib}})")
                file(RELATIVE_PATH rel "${ROOT}" "${source}")
                string(STRIP "${line}" line)
                string(APPEND violations "\n  ${rel}: ${line}")
            endif()
        endforeach()
    endforeach()
endforeach()

if(violations)
    message(FATAL_ERROR "Include rule violations (see architecture.md):${violations}")
endif()
message(STATUS "Include rules: no violations")
