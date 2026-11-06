# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

Fovea is an open-source, cross-platform Vulkan image generator (IG) for real-time geographic scenes, driven by a host program. Flight simulation over CIGI is its first application, not its definition (D-011): keep vehicle-, domain- and protocol-specific behaviour in adapters and modules, out of core and the renderer. `docs/fovea-plan.md` is the authoritative design handoff, `docs/architecture.md` turns it into concrete structure, and `docs/decisions.md` records design choices (D-xxx). The build, test and CI stack is in place. There's no IG code yet.

## Commands

Builds use CMake presets with Ninja. Each preset builds into `build/<preset>`.

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
# presets: debug, release, relwithdebinfo, asan (ASan+UBSan, -Og), asan-fast (-O1), tsan
# tsan on WSL/Ubuntu 24.04 needs ASLR off: setarch "$(uname -m)" -R ctest --preset tsan
ctest --preset debug -R 'Toolchain\.FloatingPointIsIeeeWithoutFastMath'   # one test
./build/debug/apps/fovea/fovea --list-monitors                            # monitors, in config index order
./build/debug/apps/fovea/fovea --identify --seconds 10 config/<file>.toml  # colour each channel's window
CC=clang CXX=clang++ cmake --preset debug -B build/clang-debug --fresh     # Clang build
```

GCC (≥ 11), Clang (≥ 14) and MSVC (VS 2022) must all build without warnings, and warnings are errors. On Windows, configure from a VS developer prompt with `CC=cl CXX=cl` and the same presets (ASan presets are GCC/Clang only). Every compiled target must call `fovea_target_defaults()` (`cmake/FoveaCompilerFlags.cmake`). Header-only INTERFACE targets can't take its PRIVATE flags; their headers are compiled, and checked, in the targets that include them. Fast-math is forbidden. Dependencies go in `cmake/FoveaDependencies.cmake`, pinned by version and SHA-256, and are added only when code needs them. The test presets fail when no tests match (`noTestsAction: error`). Linux builds need `libx11-dev libxext-dev libxrandr-dev` for SDL. Tests that open windows use SDL's dummy display (`SDL_VIDEO_DRIVER=dummy`, or `PlatformOptions{"dummy"}`), and must have a time limit. Real monitors are checked by hand on the Windows machine.

Each library keeps its public headers in `libs/<lib>/include/fovea/<lib>/` and its sources in `libs/<lib>/src/`. Tests go in `tests/unit/`, added to `tests/CMakeLists.txt`. Use `test::AllocationCounter` (`tests/support/alloc_counter.hpp`) to prove that render-path code allocates nothing. The `IncludeRules.*` tests (`cmake/CheckIncludeRules.cmake`) enforce the library boundaries from `docs/architecture.md`. Update the rules there when a library gains a new dependency.

The lint tools are pinned to CI's versions in a git-ignored venv:

```sh
python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt
git ls-files '*.cpp' '*.hpp' | xargs .venv/bin/clang-format --dry-run --Werror
.venv/bin/clang-tidy -p build/debug $(git ls-files 'apps/*.cpp' 'libs/*.cpp' 'tools/*.cpp')
```

CI (`.github/workflows/ci.yml`) runs GCC debug, release and asan-fast, Clang debug, Rocky Linux 9 with GCC 11, Windows MSVC debug and release, and the format and tidy checks. If a test takes more than about 1 s in Release, add it to `SLOW_TESTS` there. `full-verification.yml` runs nightly.

The first session's tasks are listed in the plan under "First Claude Code session tasks". Milestone 1 is the skeleton: three-window rendering, off-axis frustums from config, a 60 Hz clock, frame-time logging and a display sync test.

## Planned layout and stack

- `apps/fovea` (runtime IG), `apps/fovea-compiler` (offline terrain compiler), `libs/core`, `libs/render`, `libs/platform`, `libs/net`, `tools/cigi-test-host`, `config/`, `docs/`.
- C++20, CMake, Vulkan 1.3, volk, VMA, vk-bootstrap, SDL3, toml++, Slang (see `docs/decisions.md`).
- GDAL is used **only** in the compiler, never in the runtime.
- Use one shader toolchain targeting SPIR-V (Slang or GLSL). Pick it before significant shader work starts.
- Record design choices in `docs/decisions.md`. Summarize the architecture in `docs/architecture.md`.

## Architecture (big picture)

There are two halves. The **offline compiler** reprojects and tiles public source data (3DEP, NAIP, NLCD, OSM, FAA) into a versioned, layered, multi-resolution tile database with a manifest. The **runtime IG** streams that database.

The runtime has three threads:
1. **Network**: a host adapter (CIGI first) writes the latest entity state into a lock-free buffer.
2. **Paging**: predicts motion, loads tiles and models from disk, and uploads them through a transfer queue. Models load only on this thread.
3. **Render**: runs a fixed 60 Hz clock and consumes only prepared data.

Invariants that are expensive to retrofit, so respect them in all code:
- **No disk I/O or heap allocation on the render path.** The render thread never blocks.
- Double-precision ECEF positions on the CPU, camera-relative floats on the GPU, and reverse-Z depth.
- Linear HDR, scene-referred rendering, with tone mapping applied per view.
- GPU-driven rendering: descriptor indexing, indirect draws, and clustered lighting for many airport lights.
- **Protocol-neutral scene state.** CIGI (and later other protocols) are adapters that write timestamped scene state. Nothing in the renderer depends on packet types.
- **View** is the general rendering unit: pose source, projection, render mode, post chain and output target. Display channels and future sensor views (EO, NVG, IR, lidar) share it. A channel is config-driven (monitor, eye offset, asymmetric frustum) so it can later move to another process or machine.
- The **Presenter** abstraction must support three swapchains or one spanned surface. Render resolution is independent of output resolution.
- Terrain database reading and spatial queries (height of terrain, LOS, collision) live in a CPU-only library with **no Vulkan dependency**, so hosts can share it.
- Material IDs exist in the data format from day one: terrain material maps and material tags on glTF models.
- A separate control and stats plane (start/stop, database selection, module toggles, health, frame time, missed frames, latency) is kept apart from real-time CIGI traffic.
- Headless and replay modes (logged CIGI plus off-screen rendering) support regression and golden-image tests.

Modules are compiled in behind stable interfaces and enabled through config. Don't build a DLL plugin system. A feature that changes the main render path or the data format belongs in core. A feature that only uses core services is a module. Each module implements init, update, record and shutdown, and declares a GPU time budget that it reports through the stats feed.

## Repository rules

- Use public data and generic models only. Don't commit proprietary or restricted code, data or assets. Sensor modes use generic, publicly documented behavior.
- Never commit terrain databases or raw source data. Commit source manifests, download scripts and compiler settings instead.
- Keep a data attribution file listing each source, its license and any required notice.
- Win32-only APIs belong only in the thin platform layer.
- `main` is the protected default branch. Do work on feature branches, not directly on `main`.
