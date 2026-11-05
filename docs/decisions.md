# Decisions

This file records design choices. Each entry is **Accepted**, **Proposed** (awaiting review) or **Superseded** (with a link to the entry that replaces it). Add new entries at the bottom and never renumber.

## D-001 Windowing and input: SDL3

**Status:** Accepted

SDL3 creates the windows, enumerates monitors and handles input in `libs/platform`. It gives finer control over displays and display modes than GLFW (per-display bounds, borderless fullscreen placed on a specific monitor), and it also covers gamepad and joystick input for later test tools. SDL is used only inside `libs/platform`. Other libraries see the platform layer's own types.

## D-002 Configuration format: TOML (toml++)

**Status:** Accepted

People edit channel and runtime config by hand, and TOML allows comments and is easy to read. Parsing uses toml++ (header-only). Config is parsed once at startup into plain structs, and nothing reads TOML at runtime.

## D-003 Shader toolchain: Slang to SPIR-V

**Status:** Accepted

All shaders are written in Slang and compiled ahead of time to SPIR-V with `slangc`, run as a CMake build step. Slang modules and generics fit the per-view post-processing chains and the future sensor modes. `slangc` ships with the Vulkan SDK, so there's no extra toolchain to install. The runtime compiles no shaders.

## D-004 Repository license: Apache-2.0

**Status:** Proposed. This must be accepted before the first public push.

Apache-2.0 is permissive like MIT and also includes an explicit patent grant, which matters for simulation and defense-adjacent users. Third-party data has its own licenses, which are tracked separately in the attribution file.

## D-005 Build and dependencies

**Status:** Accepted. Compiler and CI details are in D-008.

- C++20, CMake ≥ 3.24, Vulkan 1.3. The Vulkan SDK must be installed on the build machine once rendering work starts. It provides headers, validation layers and `slangc`.
- Third-party libraries are fetched with CMake `FetchContent`, pinned by version and SHA-256, in `cmake/FoveaDependencies.cmake`. A library is added there when the first code that uses it lands. Today that's only GoogleTest. volk, VMA, vk-bootstrap, SDL3 and toml++ follow with milestone 1. There's no package manager. GDAL is the exception: it's large, so the compiler finds a system or package-manager install of it. The runtime doesn't use GDAL.

## D-006 Debug overlay: Dear ImGui

**Status:** Proposed

The frame-timing overlay and future debug panels use Dear ImGui with its Vulkan backend. ImGui renders only in a final overlay pass on the debug channel and never in sensor or offscreen views. It's a development tool, not the operator control UI, which lives outside the process (see `architecture.md`, "Control plane").

## D-007 Unit test framework: GoogleTest

**Status:** Accepted

GoogleTest 1.15.2 is used for CPU-side unit tests (math, config parsing, containers, terrain queries), discovered and run through CTest (`gtest_discover_tests`). It matches the sibling eyas repo, so the two share tooling and habits. GPU and image tests go through the headless and replay harness instead.

## D-008 Compilers, warnings and CI (adapted from eyas)

**Status:** Accepted

- **Compilers:** GCC ≥ 11 and Clang ≥ 14, and both must build without warnings. `cmake/FoveaCompilerFlags.cmake` rejects any other compiler. Every Fovea target calls `fovea_target_defaults()`, which turns on strict warnings (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion …`) with `-Werror`. Third-party headers are SYSTEM includes, so their warnings can't fail the build.
- **Floating point:** no fast-math, and `-ffp-contract=off`. Configuring with fast-math flags is a hard error. CPU-side math (ECEF, terrain queries, replay) is then repeatable for a given compiler and configuration, which the headless regression tests depend on.
- **Debug optimization:** Debug builds use `-Og` (`FOVEA_DEBUG_OPTIMIZATION`). The `asan-fast` preset uses `-O1`.
- **Presets:** `debug`, `release`, `relwithdebinfo`, `asan` (ASan+UBSan, strict -Og) and `asan-fast` (-O1, used on PRs).
- **CI** (`ci.yml`, on every PR and push to main): GCC debug, release and asan-fast; Clang debug; Rocky Linux 9 with its system GCC 11 in release; clang-format and clang-tidy 18. ccache is used throughout. **Nightly** (`full-verification.yml`): strict ASan, Clang with every test, and clang-tidy on every file. A failed scheduled run opens or updates a GitHub issue.
- Windows builds with MSVC (D-009).

## D-009 Windows toolchain: MSVC (Visual Studio 2022)

**Status:** Accepted

Windows is the primary target, and MSVC is the native toolchain for Vulkan development, the Visual Studio debugger and GPU tools there. `fovea_target_defaults()` has an MSVC branch that is as close to the GCC/Clang rules as MSVC allows. It uses `/W4 /permissive- /WX`, plus the opt-in warnings that match `-Wconversion`, `-Wsign-conversion`, `-Wnon-virtual-dtor`, `-Woverloaded-virtual` and `-Wimplicit-fallthrough`. It uses `/fp:precise`, which in VS 2022 does no FMA contraction, and `/fp:fast` is rejected the same way as `-ffast-math`. Third-party headers are compiled at `/external:W0`. MSVC Debug builds keep `/Od`, since `-Og` has no MSVC equivalent. clang-cl is rejected for now, because it interprets the GCC-style flags differently. The ASan presets apply to GCC and Clang only. CI builds the `debug` and `release` presets on `windows-2022` with Ninja.
