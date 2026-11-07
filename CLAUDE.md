# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

Fovea is an open-source, cross-platform Vulkan image generator (IG) for real-time geographic scenes, driven by a host program. Flight simulation over CIGI is its first application, not its definition (D-011): keep vehicle-, domain- and protocol-specific behaviour in adapters and modules, out of core and the renderer. `docs/fovea-plan.md` is the authoritative design handoff, `docs/architecture.md` turns it into concrete structure, and `docs/decisions.md` records design choices (D-xxx). The build, test and CI stack is in place. The skeleton (milestone 1), the CIGI eyepoint (milestone 2) and their tests are in place; open items are GitHub issues by milestone.

## Commands

Builds use CMake presets with Ninja. Each preset builds into `build/<preset>`.

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
# presets: debug, release, relwithdebinfo, asan (ASan+UBSan, -Og), asan-fast (-O1), tsan
# tsan on WSL/Ubuntu 24.04 needs ASLR off, and runs without the validation layer
# (not TSan-built, it reports races between its own threads):
#   VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation TSAN_OPTIONS=suppressions=$PWD/tools/ci/tsan.supp \
#     setarch "$(uname -m)" -R ctest --preset tsan   (lavapipe isn't TSan-built either)
ctest --preset debug -R 'Toolchain\.FloatingPointIsIeeeWithoutFastMath'   # one test
./build/debug/apps/fovea/fovea --list-monitors                            # monitors, in config index order
./build/debug/apps/fovea/fovea --seconds 10 config/<file>.toml             # the reference scene on each channel
./build/debug/apps/fovea/fovea --seconds 15 --stats out.csv config/<file>.toml  # per-frame timing CSV + summary
./build/debug/apps/fovea/fovea --overlay config/<file>.toml                 # ImGui timing overlay, first channel
./build/debug/apps/fovea/fovea --clear --seconds 10 config/<file>.toml     # GPU-clear each channel's window
./build/debug/apps/fovea/fovea --identify --seconds 10 config/<file>.toml  # same, filled in software
./build/debug/apps/fovea/fovea tests/data/offscreen.toml                   # headless Vulkan, no display
# a [host] section (see config/channels.example.toml) makes fovea listen for CIGI 3.3 over UDP
./build/debug/apps/fovea/fovea --overlay config/cigi-test-host.toml        # then, alongside:
./build/debug/tools/cigi-test-host/cigi-test-host tools/cigi-test-host/scripts/circuit.toml
python3 tools/fetch-sources.py data/sources/khsv.toml   # terrain sources into data/cache (about 17 GB)
./build/release/apps/fovea-compiler/fovea-compiler data/compile/khsv.toml  # the KHSV database into data/db/khsv
python3 tools/check-elevation.py data/compile/khsv.toml # check it against the sources and the FAA's survey
python3 tools/check-imagery.py data/compile/khsv.toml   # its imagery against the sources (PSNR, seams)
./build/debug/apps/fovea/fovea --headless --dump out config/<file>.toml    # any config headless, out/<channel>.png
./build/release/apps/fovea/fovea --overlay config/khsv.toml                # the KHSV database on the desk, then:
./build/release/tools/cigi-test-host/cigi-test-host tools/cigi-test-host/scripts/khsv-approach.toml  # or khsv-survey.toml (paging), khsv-low.toml (110 m)
FOVEA_UPDATE_GOLDEN=1 ctest --preset debug -R Golden                       # rewrite tests/data/golden (on lavapipe)
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json FOVEA_REQUIRE_VULKAN=1 ctest --preset debug  # GPU tests on lavapipe
CC=clang CXX=clang++ cmake --preset debug -B build/clang-debug --fresh     # Clang build
cmake --preset fuzz && cmake --build --preset fuzz --target fovea_fuzz_cigi
./build/fuzz/tests/fovea_fuzz_cigi -max_total_time=60 tests/fuzz/corpus/cigi   # fuzz the CIGI codec
```

GCC (≥ 11), Clang (≥ 14) and MSVC (VS 2022) must all build without warnings, and warnings are errors. On Windows, configure from a VS developer prompt with `CC=cl CXX=cl` and the same presets (ASan presets are GCC/Clang only). Every compiled target must call `fovea_target_defaults()` (`cmake/FoveaCompilerFlags.cmake`). Header-only INTERFACE targets can't take its PRIVATE flags; their headers are compiled, and checked, in the targets that include them. Fast-math is forbidden. Dependencies go in `cmake/FoveaDependencies.cmake`, pinned by version and SHA-256, and are added only when code needs them. GDAL is the exception (D-005, D-019): the compiler uses the system's (3.8 or later), which nothing else links. The test presets fail when no tests match (`noTestsAction: error`). Linux builds need `libx11-dev libxext-dev libxrandr-dev` for SDL. The terrain compiler (`apps/fovea-compiler`, D-019) is built only where a system GDAL 3.8+ is found (`libgdal-dev`; Linux only), as are its tests; `FOVEA_REQUIRE_GDAL=1` at configure time makes a missing GDAL an error (CI sets it for the Ubuntu jobs). `tools/check-elevation.py` needs `python3-gdal` and `python3-numpy`, `tools/check-imagery.py` also `python3-pil`. Tests that open windows use SDL's dummy display (`SDL_VIDEO_DRIVER=dummy`, or `PlatformOptions{"dummy"}`), and must have a time limit. Real monitors are checked on the Windows machine with `tools/check-identify.ps1`, which screenshots each monitor while `fovea --identify` runs. It needs an unlocked desktop.

Each library keeps its public headers in `libs/<lib>/include/fovea/<lib>/` and its sources in `libs/<lib>/src/`. Tests go in `tests/unit/`, added to `tests/CMakeLists.txt`. Use `test::AllocationCounter` (`tests/support/alloc_counter.hpp`) to prove that render-path code allocates nothing. GPU tests go in `fovea_gpu_tests`. The counter must replace every `new`/`delete` form, because Vulkan drivers loaded at run time use them all. They skip without a Vulkan 1.3 driver unless `FOVEA_REQUIRE_VULKAN` is set (CI sets it, plus `FOVEA_REQUIRE_VALIDATION`). Vulkan types stay inside `libs/render/src/vk`, and `renderer.hpp` exposes none. Shaders are Slang in `shaders/`, compiled by the build (`fovea_add_shader`). Push-constant structs are mirrored in C++ with `static_assert`ed sizes, within 128 bytes. WSL has `vulkan-validationlayers` installed, so run GPU tests locally with `FOVEA_REQUIRE_VALIDATION=1` as CI does. Also run them on the Windows machine, which has the real GPU and present timing. Steady-frame allocation tests turn validation off, because the layer allocates on the calling thread. Golden images (`tests/data/golden`, for `tests/data/golden.toml`, and `terrain_hills.png` from a database `test_terrain_render.cpp` writes) are rendered on lavapipe; NVIDIA differs from them by at most 1 level. Regenerate them only for an intended visual change, and look at them first. The `IncludeRules.*` tests (`cmake/CheckIncludeRules.cmake`) enforce the library boundaries from `docs/architecture.md`. Update the rules there when a library gains a new dependency.

CIGI code is written from the CIGI 3.3 ICD and its errata, never from memory: `tools/fetch-cigi-icd.sh` puts them in `docs/reference/` (git-ignored, hash-checked). Cite the ICD section and page for each packet.

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
- C++20, CMake, Vulkan 1.3, volk, VMA, vk-bootstrap, SDL3, toml++, Slang, glm, Dear ImGui, stb_image (see `docs/decisions.md`).
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
- Win32-only APIs belong only in the thin platform layer, with one exception: the UDP socket file in `libs/net` (`udp_socket.cpp`, D-014) uses Winsock.
- `main` is the default branch: do work on feature branches, merged by pull request, never directly on `main`. GitHub branch protection (since 2026-11-06, when the repository went public) requires a pull request, blocks force pushes and deletion, and keeps history linear; no approval or status checks are required. Pushes that change only Markdown, `docs/` or a license file skip CI; a newer push to a pull request cancels its older CI run.
