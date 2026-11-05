# Third-party dependencies, fetched at configure time and pinned by version
# and SHA-256. For machines without internet access, point CMake at local
# copies of the same archives' contents, e.g.
#   -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest-1.15.2

include(FetchContent)

# Mark a target's include directories as SYSTEM so its headers cannot trigger
# our -Werror (D-008).
function(fovea_mark_system_includes target)
    get_target_property(dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
        set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
    endif()
endfunction()

# --- GoogleTest 1.15.2 (tests only) -------------------------------------------
if(FOVEA_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    # Use the same MSVC runtime (/MD) as our targets; no effect elsewhere.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926)
    FetchContent_MakeAvailable(googletest)
    fovea_mark_system_includes(gtest)
    fovea_mark_system_includes(gtest_main)
endif()
