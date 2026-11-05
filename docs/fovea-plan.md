# Fovea — Image Generator Plan

Fovea is an independent, open-source, cross-platform Vulkan image generator (IG) for real-time flight simulation. It works with any compatible host through the public Common Image Generator Interface (CIGI). This document is the handoff for a Claude Code session to begin implementation.

## Goals and constraints

- Custom C++ renderer on Vulkan; no commercial engine.
- Windows first, but cross-platform (Linux) by design: no Win32-only APIs outside a thin platform layer.
- 60 Hz locked frame rate as the first target; never drop frames for content loading.
- Three configurable display channels at launch, driven by one PC and one GPU.
- Host interface: CIGI first; DIS later.
- First database: local area around Huntsville International Airport (KHSV).
- Public data and generic models only.
- Low-altitude and hover visual cues matter more than distant terrain fidelity.

## Non-goals for now

- Dynamic global illumination, a scene editor, and general-purpose engine features.
- Sensor modes are planned for, but not built in the first milestones.
- DIS/HLA support (design the entity layer so it can be added later).

## Architecture overview

Fovea has two halves: an offline terrain compiler and a runtime IG.

### Offline terrain compiler

Runs once per database version, ahead of time, like compiling code. It reprojects and tiles source data into a single multi-resolution database on disk.

- Inputs: elevation (USGS 3DEP 1 m lidar DEM near KHSV, USGS 3DEP 1/3 arc-second as coarse backdrop), imagery (USDA NAIP ~60 cm; Sentinel-2 10 m fallback), land cover (USGS NLCD), vectors (OpenStreetMap roads, buildings, water), FAA airport data, and hand-placed model placements.
- Tooling: GDAL for reading, reprojection, and resampling.
- Outputs per tile: elevation mesh/heightfield with LOD pyramid, GPU-compressed imagery (BC formats with mips), an 8-bit material-ID map aligned with imagery, model placement lists, and scatter (vegetation) parameters.
- Airport handling: flatten terrain under runways and aprons.
- Incremental rebuilds: only tiles affected by changed sources are recompiled.
- A small index/manifest describes which tiles exist.

### Runtime IG

One executable, three borderless fullscreen windows (one per monitor), each with its own swapchain, rendered from a shared scene in a single frame and presented back to back.

Threads:

1. Network thread: receives CIGI packets and writes the latest eyepoint and entity state into a lock-free buffer.
2. Paging thread: predicts aircraft motion, loads tiles and models from disk, uploads to GPU via a transfer queue.
3. Render thread: fixed 60 Hz frame clock; consumes only prepared data; never blocks on I/O or allocation.

Key rules:

- No disk I/O and no heap allocation on the render path.
- Double-precision ECEF positions on the CPU; camera-relative floats on the GPU; reverse-Z depth.
- "Channel" is a first-class, config-driven abstraction (eye offset, off-axis frustum, output display) so channels can later split across processes or machines without a rewrite.
- Stats feed: frame time, missed frames, and end-to-end latency, available to an external control UI.

## Views

A view is the general rendering unit: pose source, projection, render mode, post-processing chain, and output target. Display channels and sensor views use the same abstraction, so EO, night vision, IR, and lidar views can render off-screen without a separate pipeline.

## Core services and modules

Use compiled-in modules behind stable interfaces, enabled through config. Do not build a runtime DLL plugin system at this stage.

- Rule: if a feature changes the main render path or the data format, it is core. If it only uses core services, it is a module.
- Core: time of day and atmosphere, lighting, shadows, spatial queries (collision, height of terrain, line of sight), and the particle system.
- Modules: rotor downwash, brownout and whiteout, cloud and precipitation visuals, PAPI behavior, and sensor modes.
- Lifecycle: each module implements init, update, record, and shutdown.
- Budget: each module declares a GPU time budget and reports actual cost through the stats feed.

## Foundational decisions

Lock these in before milestone 1 is complete, because they are expensive to retrofit.

1. Shared terrain queries: keep database reading and spatial queries in a CPU-only library with no Vulkan dependency, so a host can use the same surface the IG displays.
2. Versioned, layered database: include a schema version and named per-tile layers for elevation, imagery, materials, placements, lights, and collision. New layers should not require rebuilding everything.
3. Linear HDR rendering: render scene-referred light values, with tone mapping applied per view. Night lighting, night vision, and IR all depend on this.
4. GPU-driven rendering: use descriptor indexing and indirect draws, with clustered lighting or an equivalent design that handles many airport lights.
5. Protocol-neutral scene state: CIGI and later DIS are adapters that write timestamped scene state. Nothing in the renderer depends on packet types.
6. Presenter abstraction: support three swapchains or one spanned surface. The renderer should not assume either. Render resolution stays independent of output resolution.
7. Separate control plane: an external UI uses a control and stats interface for start, stop, database selection, module toggles, and health, separate from real-time CIGI traffic.
8. Headless and replay support: logged CIGI traffic plus an off-screen mode enables regression tests, golden-image comparisons, and repeatable performance runs.
9. Single shader toolchain: choose one toolchain targeting SPIR-V, such as Slang or GLSL, before significant shader work begins.

## Public repository rules

- Use public data and generic models only.
- Do not commit proprietary or third-party restricted code, data, documents, or assets.
- Fovea depends only on public interfaces and its own example configuration.
- Sensor modes use generic, publicly documented behavior rather than models of specific fielded systems.
- Do not commit terrain databases or raw source data. Commit source manifests, download scripts, and compiler settings so the database can be rebuilt.
- Choose a repository license before the first public push.
- Keep a data attribution file listing each source, its license, and any notice required for published builds or databases.

## Display channels

- Each channel uses an asymmetric (off-axis) frustum matched to the physical screen position relative to the pilot eye point, so the horizon stays continuous across bezels.
- Curved displays: start with flat-screen frustums; add mild warp correction only if needed.
- TVs used as displays should run in game mode with motion smoothing and image processing disabled to minimize and equalize latency.

Display sync is not guaranteed just because one GPU renders all three views. Presenting the same frame does not by itself lock the outputs or remove differences in display processing delay. Test this in milestone 1 with a high-speed camera or similar measurement, and treat GPU driver features such as display spanning or sync as options to evaluate.

## Static models

- Format: glTF, authored in Blender.
- Unique landmarks (KHSV tower, hangars, terminal) are hand-placed via a placement file (lat, lon, heading, model ID) and baked into tiles by the compiler.
- Vegetation and clutter are scattered procedurally from NLCD land cover and drawn with GPU instancing, with impostor billboards at distance.
- Models load on the paging thread only.
- Close-range detail (procedural detail textures, ground clutter) is a priority for hover and low-level flight cues.

## Dynamic entities

- Other aircraft, UAVs, and vehicles come from the host via CIGI entity control packets; never baked into terrain.
- Entity table: ID, model type, position, orientation, articulated parts (e.g., rotors).
- Interpolation/extrapolation to smooth host update rate vs. 60 Hz frame rate.
- Height-of-terrain query support so ground vehicles sit on terrain.
- Entity layer kept protocol-neutral so a future DIS bridge can feed the same table.

## Sensor views (future)

- IR: render per-material temperature using a GPU material property table (thermal parameters, time of day, weather), then sensor effects (blur, noise, gain, white-hot/black-hot).
- Lidar: ray queries (Vulkan ray query extension) or depth-view sampling to produce returns.
- Material IDs must exist in the data format from day one: terrain material maps, and material tags on glTF models.

## Data budget

- 60 cm imagery is roughly 2–4 MB per km² after GPU compression and mips.
- A 100 × 100 km area around Huntsville is roughly 20–40 GB of imagery, with elevation adding a similar amount before compression.
- Use high resolution only near airfields and low-level routes; coarser data elsewhere.

## Suggested tech stack

- C++20, CMake, Vulkan 1.3.
- volk (loader), VMA (memory), vk-bootstrap (setup), GLFW or SDL for windows and input.
- GDAL in the compiler only.
- A glTF loader such as cgltf or fastgltf.
- A CIGI implementation: evaluate the Common Image Generator Interface Class Library (CCL) and check its license, or write a minimal packet codec for the needed packets.

## Milestones

1. Skeleton: CMake project, platform layer, Vulkan init, three borderless windows on three monitors, per-channel off-axis frustums from config, 60 Hz frame clock, frame-time logging, and a measured display sync test.
2. CIGI eyepoint: build a small scripted CIGI test host, receive its eyepoint over CIGI, and fly a camera over a test grid; log end-to-end latency.
3. First terrain: compiler builds a small KHSV database (3DEP elevation + NAIP imagery + LOD); runtime streams it at a locked 60 Hz.
4. Airport and models: flattened runways, glTF model library, hand-placed KHSV landmarks.
5. Low-level cues: procedural detail textures, scattered vegetation with instancing and impostors.
6. Entities: CIGI entity control, interpolation, articulated rotors, height-of-terrain queries.
7. Evaluation: fly scripted hover and low-level routes and compare frame timing and visual cues against written acceptance criteria.
8. Later: material maps and IR, lidar, DIS bridge, multi-machine channels.

## First Claude Code session tasks

1. Create the repository layout: apps/fovea (runtime), apps/fovea-compiler (offline), libs/core, libs/render, libs/platform, libs/net, tools/cigi-test-host, config/, docs/.
2. Set up CMake with Windows and Linux builds, warnings as errors, and dependency fetching for volk, VMA, vk-bootstrap, and GLFW or SDL.
3. Implement milestone 1: three-window rendering with per-channel config (JSON or TOML) describing monitor index, eye offset, and frustum angles.
4. Draw a reference scene (ground grid and horizon) to verify continuous horizon across all three screens.
5. Add a frame-timing overlay and log file.
6. Write docs/architecture.md summarizing this plan, docs/decisions.md for recording design choices, and stub interfaces for View, Presenter, SceneState, Module, and TerrainQuery.

## Open questions

- Which CIGI version to implement first, and which packets are required for milestones 2 and 6.
- Target GPU model and display resolution for development.
- Physical screen geometry and eye point measurements for the example frustum config.
- Model sources and licenses for KHSV landmarks and aircraft entities.
- Repository license.
