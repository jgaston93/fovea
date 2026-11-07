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

# --- Vulkan (libs/render only, D-005) ------------------------------------------
# Headers, loader, setup and memory, all pinned to the Vulkan SDK 1.4.363
# release, so every platform builds against the same headers; the Vulkan
# loader itself is found at run time by volk. No Vulkan SDK is needed to
# build (only for validation layers and the shader compiler).
FetchContent_Declare(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
    URL_HASH SHA256=4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e)
FetchContent_MakeAvailable(vulkan_headers)
fovea_mark_system_includes(Vulkan-Headers)

set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(volk
    URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
    URL_HASH SHA256=1547d8d74395d4048fb3f4a6313da56db626e89b55db238d0d7e8944c8a645f3)
FetchContent_GetProperties(vulkan_headers)
set(VULKAN_HEADERS_INSTALL_DIR "${vulkan_headers_SOURCE_DIR}" CACHE PATH "" FORCE)
FetchContent_MakeAvailable(volk)
fovea_mark_system_includes(volk)

set(VK_BOOTSTRAP_TEST OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vk_bootstrap
    URL https://github.com/charles-lunarg/vk-bootstrap/archive/refs/tags/v1.4.363.tar.gz
    URL_HASH SHA256=11927b43a5c5d64646982b273d6436d2243acaa2da8834f7ba382060431155fe)
FetchContent_MakeAvailable(vk_bootstrap)
fovea_mark_system_includes(vk-bootstrap)

set(VMA_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA256=822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1)
FetchContent_MakeAvailable(vma)
fovea_mark_system_includes(VulkanMemoryAllocator)

# --- Slang 2026.17.1 (shader compiler, D-003; a build tool, not linked) ---------
# The official prebuilt release, pinned and hashed, so every machine compiles
# shaders with the same slangc (it matches the Vulkan SDK 1.4.363's Slang).
# -DFOVEA_SLANGC=<path> uses another slangc instead (e.g. offline).
set(FOVEA_SLANGC "" CACHE FILEPATH "slangc to compile shaders with (empty: fetch Slang 2026.17.1)")
if(NOT FOVEA_SLANGC)
    if(WIN32)
        FetchContent_Declare(slang
            URL https://github.com/shader-slang/slang/releases/download/v2026.17.1/slang-2026.17.1-windows-x86_64.zip
            URL_HASH SHA256=a41896cba523ecfecaf67d18bdb7767349112a45540ac0b88e8ce48e10ce239f
            SOURCE_SUBDIR do-not-configure)
        set(slangc_name slangc.exe)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        FetchContent_Declare(slang
            URL https://github.com/shader-slang/slang/releases/download/v2026.17.1/slang-2026.17.1-linux-x86_64-glibc-2.28.tar.gz
            URL_HASH SHA256=163f9527bcb92e005e83a3441a77c92317c9ad478614a2af98333e660075def2
            SOURCE_SUBDIR do-not-configure)
        set(slangc_name slangc)
    else()
        message(FATAL_ERROR "No pinned Slang release for this platform; set -DFOVEA_SLANGC=<path to slangc>")
    endif()
    FetchContent_MakeAvailable(slang)
    set(FOVEA_SLANGC "${slang_SOURCE_DIR}/bin/${slangc_name}" CACHE FILEPATH "" FORCE)
endif()

# --- Dear ImGui 1.93.0 (debug overlay, D-006; libs/render only) ----------------
# No CMake project of its own: the core and the Vulkan backend are built here,
# with Vulkan functions from volk. No platform backend: the overlay takes no
# input and runs on the render thread. Its default font is built in.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz
    URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee
    SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(imgui)
add_library(fovea_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(fovea_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(fovea_imgui PUBLIC
    IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DISABLE_OBSOLETE_FUNCTIONS VK_NO_PROTOTYPES)
target_link_libraries(fovea_imgui PUBLIC volk::volk Vulkan::Headers)
set_target_properties(fovea_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- stb_image 2.30 / stb_image_write 1.16 (PNG and JPEG; libs/core only) ----
# Two public-domain headers, pinned by commit and SHA-256. Used for headless
# image dumps, golden-image tests and model textures (D-024), never on the
# render path.
set(stb_commit a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d)
FetchContent_Declare(stb_image
    URL https://raw.githubusercontent.com/nothings/stb/${stb_commit}/stb_image.h
    URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_Declare(stb_image_write
    URL https://raw.githubusercontent.com/nothings/stb/${stb_commit}/stb_image_write.h
    URL_HASH SHA256=cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(stb_image stb_image_write)
add_library(fovea_stb INTERFACE)
target_include_directories(fovea_stb SYSTEM INTERFACE
    "${stb_image_SOURCE_DIR}" "${stb_image_write_SOURCE_DIR}")

# --- cgltf 1.15 (glTF reading; libs/models only, D-023) -----------------------
# One MIT-licensed C header with no dependencies, compiled once in its own
# library (third-party code, so without Fovea's warning flags).
FetchContent_Declare(cgltf
    URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz
    URL_HASH SHA256=84e352092e5cd6aab7f66de62ddb66beb5e6f18d412ca9d12950d7a55bfef25a
    SOURCE_SUBDIR do-not-configure)
FetchContent_MakeAvailable(cgltf)
file(WRITE "${CMAKE_BINARY_DIR}/fovea_cgltf/cgltf.c"
    "#define CGLTF_IMPLEMENTATION\n#include <cgltf.h>\n")
add_library(fovea_cgltf STATIC "${CMAKE_BINARY_DIR}/fovea_cgltf/cgltf.c")
target_include_directories(fovea_cgltf SYSTEM PUBLIC "${cgltf_SOURCE_DIR}")
set_target_properties(fovea_cgltf PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- bc7enc_rdo (BC7 and BC1 texture encoders; fovea-compiler only, D-017) ----
# Rich Geldreich's encoders, MIT or public domain (the files used here; the
# repository's bc7e.ispc is Apache-2.0 and not used, nor its bundled ispc
# binary). No releases, so pinned by commit. bc7enc.cpp encodes BC7 (modes 1,
# 5, 6 and 7), rgbcx BC1, bc7decomp decodes BC7 for the tests and checks.
# Only the compiler uses them, which isn't built on Windows (D-019).
if(NOT WIN32)
    FetchContent_Declare(bc7enc_rdo
        URL https://github.com/richgel999/bc7enc_rdo/archive/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0.tar.gz
        URL_HASH SHA256=bbb33d1dbb6178a3a2c044956c3cc0b8ea9fc2f2903824379dd86f87d53b64c6
        SOURCE_SUBDIR do-not-configure)
    FetchContent_MakeAvailable(bc7enc_rdo)
    add_library(fovea_bc7enc STATIC
        "${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp"
        "${bc7enc_rdo_SOURCE_DIR}/bc7decomp.cpp"
        "${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp")
    target_include_directories(fovea_bc7enc SYSTEM PUBLIC "${bc7enc_rdo_SOURCE_DIR}")
    set_target_properties(fovea_bc7enc PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

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
