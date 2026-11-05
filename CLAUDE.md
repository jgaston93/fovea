# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

Fovea is an open-source, cross-platform Vulkan image generator (IG) for real-time flight simulation, driven by a host over CIGI. The repository currently contains only the plan, `docs/fovea-plan.md`, which is the authoritative design handoff. No code, build system or tests exist yet. When they are added, update this file with the actual build, test and single-test commands. Don't guess them.

The first session's tasks are listed in the plan under "First Claude Code session tasks". Milestone 1 is the skeleton: three-window rendering, off-axis frustums from config, a 60 Hz clock, frame-time logging and a display sync test.

## Planned layout and stack

- `apps/fovea` (runtime IG), `apps/fovea-compiler` (offline terrain compiler), `libs/core`, `libs/render`, `libs/platform`, `libs/net`, `tools/cigi-test-host`, `config/`, `docs/`.
- C++20, CMake, Vulkan 1.3, volk, VMA, vk-bootstrap, GLFW or SDL. Windows first, but Linux must also build. Warnings are treated as errors.
- GDAL is used **only** in the compiler, never in the runtime.
- Use one shader toolchain targeting SPIR-V (Slang or GLSL). Pick it before significant shader work starts.
- Record design choices in `docs/decisions.md`. Summarize the architecture in `docs/architecture.md`.

## Architecture (big picture)

There are two halves. The **offline compiler** reprojects and tiles public source data (3DEP, NAIP, NLCD, OSM, FAA) into a versioned, layered, multi-resolution tile database with a manifest. The **runtime IG** streams that database.

The runtime has three threads:
1. **Network**: receives CIGI and writes the latest eyepoint and entity state into a lock-free buffer.
2. **Paging**: predicts motion, loads tiles and models from disk, and uploads them through a transfer queue. Models load only on this thread.
3. **Render**: runs a fixed 60 Hz clock and consumes only prepared data.

Invariants that are expensive to retrofit, so respect them in all code:
- **No disk I/O or heap allocation on the render path.** The render thread never blocks.
- Double-precision ECEF positions on the CPU, camera-relative floats on the GPU, and reverse-Z depth.
- Linear HDR, scene-referred rendering, with tone mapping applied per view.
- GPU-driven rendering: descriptor indexing, indirect draws, and clustered lighting for many airport lights.
- **Protocol-neutral scene state.** CIGI (and later DIS) are adapters that write timestamped scene state. Nothing in the renderer depends on packet types.
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
