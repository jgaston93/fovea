# Fovea — Image Generator Plan

Fovea is an independent, open-source, cross-platform Vulkan image generator (IG) for real-time geographic scenes, driven by a host program. Flight simulation over the public Common Image Generator Interface (CIGI) is its first application, not its definition (D-011): ground vehicles, drones, maritime and people on the ground are as much its use, and anything specific to one of them belongs in a module or an adapter. This document began as the handoff for the first Claude Code session; it is kept as the plan.

## Goals and constraints

- Custom C++ renderer on Vulkan; no commercial engine.
- Windows first, but cross-platform (Linux) by design: no Win32-only APIs outside a thin platform layer.
- 60 Hz locked frame rate as the first target; never drop frames for content loading.
- Three configurable display channels at launch, driven by one PC and one GPU.
- Host interface: CIGI first; DIS later.
- First database: local area around Huntsville International Airport (KHSV).
- Public data and generic models only.
- Close-range detail (low flight and hover, drones, ground vehicles, people on the ground) and wide coverage (high-altitude flight, large areas) are both first-class: the database and renderer let each deployment choose where to spend detail. (Originally: "low-altitude and hover visual cues matter more than distant terrain fidelity"; generalized under D-011.)

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
2. Paging thread: predicts the views' motion, loads tiles and models from disk, uploads to GPU via a transfer queue.
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
- Core: time of day and atmosphere, lighting, shadows, anti-aliasing, spatial queries (collision, height of terrain, line of sight), and the particle system.
- Modules: domain behaviour on top of core, such as, for flight simulation, rotor downwash, brownout and whiteout, and PAPI behaviour; cloud and precipitation visuals; and sensor modes. Other domains (ground, maritime) add their own.
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
- Choose a repository license: still to do, now that the repository is public (#23).
- Keep a data attribution file listing each source, its license, and any notice required for published builds or databases.

## Display channels

- Each channel uses an asymmetric (off-axis) frustum matched to the physical screen position relative to the viewer's eye point, so the horizon stays continuous across bezels.
- Curved displays and projectors: start with flat-screen frustums; then warping (distortion correction) and edge blending, as domes and multi-projector setups need (milestone 14).
- TVs used as displays should run in game mode with motion smoothing and image processing disabled to minimize and equalize latency.

Display sync is not guaranteed just because one GPU renders all three views. Presenting the same frame does not by itself lock the outputs or remove differences in display processing delay. Test this in milestone 1 with a high-speed camera or similar measurement, and treat GPU driver features such as display spanning or sync as options to evaluate.

## Static models

- Format: glTF, authored in Blender.
- Unique landmarks (KHSV tower, hangars, terminal) are hand-placed via a placement file (lat, lon, heading, model ID) and baked into tiles by the compiler.
- Vegetation and clutter are scattered procedurally from NLCD land cover and drawn with GPU instancing, with impostor billboards at distance.
- Models load on the paging thread only.
- Close-range detail (procedural detail textures, ground clutter) for any viewer near the ground: low flight and hover, drones, ground vehicles, people on foot. It follows the environment (milestone 6), as haze and a moving sun change every view already drawn.

## Dynamic entities

- Other aircraft, UAVs, and vehicles come from the host via CIGI entity control packets; never baked into terrain.
- Entity table: ID, model type, position, orientation, articulated parts (rotors, gear, turrets, wheels).
- Interpolation/extrapolation to smooth host update rate vs. 60 Hz frame rate.
- Height-of-terrain query support so ground vehicles sit on terrain.
- Entity layer kept protocol-neutral so a future DIS bridge can feed the same table.
- Entity lights: navigation, landing and search lights; landing and search lights also light the ground they fall on.
- Terrain queries for the host: hosts ask the IG for height of terrain and line of sight over CIGI (request and response packets, from the ICD), answered from the same CPU-only library a host can link (foundational decision 1).

## Environment

Added 2026-11-07, at the owner's asking: the sky, the light and the weather, which the first milestones left as a fixed placeholder (a gradient sky and a sun fixed in the south-east). Linear HDR rendering with tone mapping per view (foundational decision 3) is what they build on.

- Shadows: the sun's and moon's, on terrain and models (cascaded shadow maps or an equivalent).
- Atmosphere and aerial perspective: a physically based sky (atmospheric scattering), and distant terrain and models fading into haze with distance, which also softens where terrain levels of detail change.
- Time of day: the sun and moon placed from the date, time and location; the sky and the lighting following them through dawn, dusk and night; stars.
- Night lighting: airport and approach lights, drawn with the clustered lighting reserved for them (foundational decision 4), from a lights layer in the tiles (D-018 names it) that the compiler builds from the runways' FAA geometry and a placement file of lights; lit windows, which need emissive materials in `libs/models` and the model scripts.
- Weather: visibility (the haze above, set by the host or config) and fog, cloud layers (2D layers first; volumetric clouds if the frame allows), precipitation; wet or snow-covered ground later.
- Host control: the environment set over CIGI (time of day, atmosphere, weather), from the ICD, as every packet: milestone 6's plan extends D-014's packets to them. And in config without a host.

## Image quality

- Anti-aliasing from early on (milestone 5): runway edges, thin roads, wires and building edges crawl without it, on every display. Which method (MSAA, temporal, or both, as the frame allows) is decided in its plan.

## Cultural features

- Roads (OpenStreetMap, with their markings where the imagery doesn't show them), buildings generally (extruded from footprints with generic detail, as the landmarks are, D-025), and water (rivers, lakes and shorelines, lit and reflecting the sky). The compiler builds them as tile layers; this is what makes ground-level and urban views work.
- The sea, later: its surface and sea state set by the host, and wakes, for maritime use.

## Effects

- A particle system (core): smoke, dust, fire, explosions, spray; driven over CIGI as the ICD has it (CIGI 3.3 has no effect packets of its own: effects are entities of effect types, under Entity Control), or by modules (rotor downwash, brownout).

## Wide-area coverage

- The counterpart to close-range detail: coarser global sources beyond the local high-resolution areas (Copernicus 30 m elevation, Sentinel-2 imagery), several databases or areas, and scaling from 20 km to regions and continents. That needs the paging and the format extended: today every root tile is pinned (30 for KHSV, about a million for a continent) and every tile is a file of its own (D-018), so at scale roots page like other tiles and tiles are packed into larger files.

## Control plane

- Foundational decision 7 delivered: start, stop, database selection, module toggles, health and stats, over an interface of their own (not CIGI), with a minimal UI.

## Sensor views (future)

- IR: render per-material temperature using a GPU material property table (thermal parameters, time of day, weather), then sensor effects (blur, noise, gain, white-hot/black-hot).
- Lidar: ray queries (Vulkan ray query extension) or depth-view sampling to produce returns.
- Material IDs must exist in the data format from day one: terrain material maps, and material tags on glTF models.

## Data budget

- 60 cm imagery is roughly 2–4 MB per km² after GPU compression and mips.
- A 100 × 100 km area around Huntsville is roughly 20–40 GB of imagery, with elevation adding a similar amount before compression.
- Use high resolution only near each deployment's areas of interest (airfields, routes, training areas); coarser data elsewhere.

## Suggested tech stack

As chosen (`docs/decisions.md` has each choice and why):

- C++20, CMake, Vulkan 1.3.
- volk (loader), VMA (memory), vk-bootstrap (setup), SDL3 for windows and input.
- Slang for shaders, compiled to SPIR-V by the build.
- GDAL in the compiler only (D-019).
- cgltf for glTF (D-023).
- Fovea's own CIGI 3.3 codec, from the ICD (D-014), rather than the Common Image Generator Interface Class Library.

## Milestones

1. Skeleton: CMake project, platform layer, Vulkan init, three borderless windows on three monitors, per-channel off-axis frustums from config, 60 Hz frame clock, frame-time logging, and a measured display sync test.
2. CIGI eyepoint: build a small scripted CIGI test host, receive its eyepoint over CIGI, and fly a camera over a test grid; log end-to-end latency.
3. First terrain: compiler builds a small KHSV database (3DEP elevation + NAIP imagery + LOD); runtime streams it at a locked 60 Hz.
4. Airport and models: flattened runways, glTF model library, hand-placed KHSV landmarks.
5. Anti-aliasing: on every view, chosen and measured against the frame budget.
6. Environment: shadows, atmosphere and aerial perspective, time of day with the sun, moon and stars, night lighting (airport lights), weather (visibility, cloud layers, precipitation), set over CIGI or by config.
7. Close-range detail: procedural detail textures, scattered vegetation with instancing and impostors, for any viewer near the ground.
8. Cultural features: roads, buildings from footprints, water.
9. Entities: CIGI entity control, interpolation, articulated parts, entity lights, height-of-terrain queries, and terrain queries answered over CIGI.
10. Effects: the particle system, set over CIGI or by modules.
11. Wide-area coverage: global coarse sources, several databases or areas, regional scale.
12. Control plane: start, stop, database selection, module toggles, health and stats, a minimal UI.
13. Evaluation: scripted routes for each kind of use (low and high flight, hover, ground vehicles, a viewer on foot), frame timing and visual cues against written acceptance criteria.
14. Later: material maps and IR, lidar, the sea, projector warping and edge blending, DIS bridge, multi-machine channels.

## First Claude Code session tasks

Done in milestone 1 (kept as the record):

1. Create the repository layout: apps/fovea (runtime), apps/fovea-compiler (offline), libs/core, libs/render, libs/platform, libs/net, tools/cigi-test-host, config/, docs/.
2. Set up CMake with Windows and Linux builds, warnings as errors, and dependency fetching for volk, VMA, vk-bootstrap, and GLFW or SDL.
3. Implement milestone 1: three-window rendering with per-channel config (JSON or TOML) describing monitor index, eye offset, and frustum angles.
4. Draw a reference scene (ground grid and horizon) to verify continuous horizon across all three screens.
5. Add a frame-timing overlay and log file.
6. Write docs/architecture.md summarizing this plan, docs/decisions.md for recording design choices, and stub interfaces for View, Presenter, SceneState, Module, and TerrainQuery.

## Open questions

- Answered: CIGI 3.3 first (D-013), with packets added as milestones need them (milestone 2's so far; milestones 6 and 9 add theirs); the development GPU is an RTX 5070 Ti; KHSV's landmarks are Fovea's own generic models (D-025).
- Physical screen geometry and eye point measurements for the example frustum config (#22).
- Model sources and licenses for aircraft and vehicle entities.
- Repository license (#23).
