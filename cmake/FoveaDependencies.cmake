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

# --- glm 1.0.1 (header-only math, D-010) ---------------------------------------
# glm's own CMake project is not needed, so SOURCE_SUBDIR points at a
# directory without a CMakeLists.txt and we wrap the headers in our own
# INTERFACE target, which also fixes glm's configuration for every user.
FetchContent_Declare(glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
    URL_HASH SHA256=9f3174561fd26904b23f0db5e560971cbf9b3cbda0b280f04d5c379d03bf234c
    SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(glm)

add_library(fovea_glm INTERFACE)
target_include_directories(fovea_glm SYSTEM INTERFACE "${glm_SOURCE_DIR}")
target_compile_definitions(fovea_glm INTERFACE
    GLM_FORCE_DEPTH_ZERO_TO_ONE  # Vulkan clip-space depth
    GLM_FORCE_CTOR_INIT)         # vectors and matrices start zeroed / identity

# --- toml++ 3.4.0 (header-only config parsing, D-002; init time only) -----------
FetchContent_Declare(tomlplusplus
    URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(tomlplusplus)

add_library(fovea_tomlplusplus INTERFACE)
target_include_directories(fovea_tomlplusplus SYSTEM INTERFACE "${tomlplusplus_SOURCE_DIR}/include")

# --- SDL 3.4.18 (windows, monitors, input; libs/platform only, D-001) --------------
# A static build with only what Fovea uses. On Linux, only X11 (with XWayland
# on Wayland desktops): Wayland compositors ignore window positions, so
# windows can't be placed on chosen monitors. Headless tests use SDL's dummy
# video driver.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_UNINSTALL OFF CACHE BOOL "" FORCE)
# X11: only XRandR (multi-monitor layout) is needed beyond libX11/libXext, so
# Linux builds need just libx11-dev, libxext-dev and libxrandr-dev. Xss would
# let SDL inhibit the screensaver on X11; revisit for Linux deployments.
foreach(ext IN ITEMS XCURSOR XDBE XINPUT XFIXES XSCRNSAVER XSHAPE XSYNC XTEST)
    set(SDL_X11_${ext} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL_X11_XRANDR ON CACHE BOOL "" FORCE)
set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz
    URL_HASH SHA256=c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90)
FetchContent_MakeAvailable(SDL3)
fovea_mark_system_includes(SDL3-static)

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
