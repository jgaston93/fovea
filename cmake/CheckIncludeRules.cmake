# Checks the library boundaries in architecture.md ("Library dependency
# graph") by scanning #include lines. Run by ctest:
#   cmake -DROOT=<repo> -P cmake/CheckIncludeRules.cmake
# The target links enforce the same graph for linking; this catches a header
# included through a path the links don't stop.

if(NOT ROOT)
    message(FATAL_ERROR "set -DROOT=<repository root>")
endif()

set(gpu "vulkan/|volk|vk_mem_alloc|VkBootstrap|SDL|imgui")
# GDAL's headers: gdal*.h, cpl_*.h (its portability layer), ogr*.h.
set(gdal "gdal|cpl_|ogr")

# Library directory, then the include paths it must not use. Each library
# may include only the fovea libraries it depends on in the graph.
set(rule_core "${gpu}|${gdal}|fovea/(terrain|platform|net|render)/")
set(rule_terrain "${gpu}|${gdal}|fovea/(platform|net|render)/")
set(rule_net "${gpu}|${gdal}|fovea/(terrain|platform|render)/")
set(rule_platform "${gdal}|fovea/(terrain|net|render)/")
set(rule_render "${gdal}|fovea/(terrain|net)/")

set(violations "")
foreach(lib IN ITEMS core terrain net platform render)
    # Every C/C++ source or header: .h .hpp .c .cpp .inl .ipp and the like.
    file(GLOB_RECURSE sources LIST_DIRECTORIES false "${ROOT}/libs/${lib}/*.[hci]*")
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
