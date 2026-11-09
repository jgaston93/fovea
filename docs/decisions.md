# Decisions

This file records design choices. Each entry is **Accepted**, **Proposed** (awaiting review) or **Superseded** (with a link to the entry that replaces it). Add new entries at the bottom and never renumber.

## D-001 Windowing and input: SDL3

**Status:** Accepted

SDL3 creates the windows, enumerates monitors and handles input in `libs/platform`. It gives finer control over displays and display modes than GLFW (per-display bounds, borderless fullscreen placed on a specific monitor), and it also covers gamepad and joystick input for later test tools. SDL is used only inside `libs/platform`. Other libraries see the platform layer's own types. It is SDL 3.4.18, built static. On Linux it uses only XRandR beyond libX11/libXext, so it needs `libx11-dev libxext-dev libxrandr-dev`. Wayland is built out: compositors ignore client window positions, so a window can't be put on a chosen monitor. Wayland desktops run Fovea through XWayland. Windows are "fullscreen desktop": borderless, covering the monitor at its desktop resolution, with no display-mode switch.

## D-002 Configuration format: TOML (toml++)

**Status:** Accepted

People edit channel and runtime config by hand, and TOML allows comments and is easy to read. Parsing uses toml++ (header-only). Config is parsed once at startup into plain structs, and nothing reads TOML at runtime.

## D-003 Shader toolchain: Slang to SPIR-V

**Status:** Accepted

All shaders are written in Slang and compiled ahead of time to SPIR-V with `slangc`, run as a CMake build step, and embedded in the binary as word arrays (`cmake/FoveaShaders.cmake`), so nothing is read from disk at run time. The build fetches Slang 2026.17.1's official prebuilt release, pinned by SHA-256 like the other dependencies and matching the SDK's Slang. `-DFOVEA_SLANGC=<path>` substitutes another `slangc`. Matrices use column-major layout, like glm. Slang modules and generics fit the per-view post-processing chains and the future sensor modes. `slangc` ships with the Vulkan SDK, so there's no extra toolchain to install. The runtime compiles no shaders.

## D-004 Repository license: Apache-2.0

**Status:** Proposed. This must be accepted before the first public push.

Apache-2.0 is permissive like MIT and also includes an explicit patent grant, which matters for simulation and defense-adjacent users. Third-party data has its own licenses, which are tracked separately in the attribution file.

## D-005 Build and dependencies

**Status:** Accepted. Compiler and CI details are in D-008.

- C++20, CMake ≥ 3.24, Vulkan 1.3. The Vulkan SDK must be installed on the build machine once rendering work starts. It provides headers, validation layers and `slangc`.
- Third-party libraries are fetched with CMake `FetchContent`, pinned by version and SHA-256, in `cmake/FoveaDependencies.cmake`. A library is added there when the first code that uses it lands. Today that's only GoogleTest. SDL3, toml++, glm, and for Vulkan the headers, volk, vk-bootstrap and VMA, followed with milestone 1. The Vulkan pieces are pinned to the SDK 1.4.363 release (VMA 3.4.0), so building needs no Vulkan SDK. The SDK only supplies the validation layers and `slangc`. There's no package manager. GDAL is the exception: it's large, so the compiler finds a system or package-manager install of it. The runtime doesn't use GDAL.

## D-006 Debug overlay: Dear ImGui

**Status:** Accepted (2026-11-06). Dear ImGui 1.93.0 is pinned, with the core and the Vulkan backend (on volk, with dynamic rendering) built as `fovea_imgui`. It has no platform backend: the overlay takes no input and runs on the render thread. It uses no files (no `.ini`, built-in font) and allocates through `operator new`, so the zero-allocation test covers it. `fovea --overlay` shows it on the first channel.

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

Windows is the primary target, and MSVC is the native toolchain for Vulkan development, the Visual Studio debugger and GPU tools there. `fovea_target_defaults()` has an MSVC branch that is as close to the GCC/Clang rules as MSVC allows. It uses `/W4 /permissive- /WX`, plus the opt-in warnings that match `-Wconversion`, `-Wsign-conversion`, `-Wnon-virtual-dtor`, `-Woverloaded-virtual` and `-Wimplicit-fallthrough`. It uses `/fp:precise`, which in VS 2022 does no FMA contraction, and `/fp:fast` is rejected the same way as `-ffast-math`. Third-party headers are compiled at `/external:W0`. `_CRT_SECURE_NO_WARNINGS` is defined, because MSVC deprecates standard C functions (`sscanf`, `fopen`) in favor of its non-portable `_s` versions. MSVC Debug builds keep `/Od`, since `-Og` has no MSVC equivalent. clang-cl is rejected for now, because it interprets the GCC-style flags differently. The ASan presets apply to GCC and Clang only. CI builds the `debug` and `release` presets on `windows-2022` with Ninja.

## D-010 Math library: glm

**Status:** Accepted

glm 1.0.1 is header-only and fetched pinned like the other dependencies. It provides the vector, matrix and quaternion types on both the CPU and the GPU side: `glm::dvec3`/`dquat`/`dmat4` for ECEF positions and attitudes, and `float` types for camera-relative data sent to the GPU. Its conventions match the shader code. glm is compiled with `GLM_FORCE_DEPTH_ZERO_TO_ONE` (Vulkan depth range) and `GLM_FORCE_INTRINSICS` off, so results don't depend on the build machine's SIMD support. Geodesy (WGS-84) and the CIGI attitude conventions are Fovea's own code in `libs/core`, built on glm types.

## D-011 General purpose, with flight simulation as the first application

**Status:** Accepted

Fovea is a real-time renderer for geographic scenes, not only an image generator for one aircraft. Flight simulation over CIGI, around KHSV, is the first application and still drives milestone priorities, but the core doesn't assume it:

- **No special entity.** The scene is a table of entities. Nothing marks one as "the ownship". A view's viewpoint rides on any entity, at an offset and rotation in that entity's body axes, or is fixed in the world. A config channel names its entity (default 0, the host's main entity, which is CIGI's ownship).
- **Neutral terms in core and render.** Viewpoint, entity, body axes. Not ownship, pilot or design eye point.
- **Protocols are adapters.** CIGI is the first `HostAdapter`. Others (DIS, a simple generic protocol, a direct in-process API) can sit beside it without renderer changes.
- **Domain features are modules or data.** Runway and approach lights, rotor downwash and brownout belong in modules (`Module`) or the terrain database's layers, not in core or the main render path.
- The yaw-pitch-roll body-axis convention stays: it is the common convention for anything with a forward direction (vehicles, vessels, camera mounts), not an aircraft-only one.

## D-012 stb_image for PNG files

**Status:** Accepted (2026-11-06)

Headless dumps and golden-image tests read and write PNG. `stb_image` 2.30 and `stb_image_write` 1.16 are two public-domain headers with no dependencies, pinned by commit and SHA-256 and built with only PNG enabled. They are used by `libs/core/src/image_file.cpp`, which is file I/O and never on the render path. libpng plus zlib would add two libraries to build on three platforms for the same job. When the runtime needs to load textures (KTX2 and compressed formats, from the terrain database and glTF), that is a separate choice, made by the paging work. (Since D-024, JPEG is built too, for model textures.)

## D-013 CIGI 3.3 first

**Status:** Accepted (2026-11-06)

The first host protocol is CIGI 3.3. It is the version most hosts and IGs implement, and its interface control document is freely available. CIGI 4.0 (SISO-STD-013-2014) changes packet sizes and IDs and is less widely supported. Version-specific packet layouts stay in one part of the codec, so 4.0 can be added beside 3.3 later.

## D-014 Fovea's own CIGI codec

**Status:** Accepted (2026-11-06)

The codec is Fovea's own: fixed-size encode and decode of the packets in use, with no allocation, tested byte for byte against the ICD's layouts and fuzzed nightly. The Boeing CIGI Class Library is LGPL, older C++, and allocates per packet; only a handful of packets are needed for milestones 2 and 6.

*Amended 2026-11-07:* the milestones were renumbered (`fovea-plan.md`): "milestones 2 and 6" above are the eyepoint and the entities, now milestones 2 and 9. The environment milestone (6) adds the packets that set time of day, the atmosphere and the weather; its plan amends this decision with them, from the ICD. UDP sockets are a thin non-blocking wrapper in one file of `libs/net`, with no networking library.

*Amended 2026-11-08 (milestone 6's plan):* the environment adds Celestial Sphere Control (ICD 4.1.9, p. 82), Atmosphere Control (4.1.10, p. 85) and Weather Control (4.1.12, p. 100, global layers). Environmental Region Control (4.1.11, p. 88), regional layers and weather entities are accepted and ignored for now, counted in the stats.

Packet layouts come from the ICD itself, never from memory: `tools/fetch-cigi-icd.sh` downloads the CIGI 3.3 ICD (Boeing document TST08I016, 3 Nov 2008) and its errata (Appendix C, 4 May 2009) into the git-ignored `docs/reference/`, checked against pinned SHA-256 hashes. The ICD calls itself "intended to be a public domain document" but asks not to be reproduced for modifying the interface, so it isn't committed. Codec code cites the ICD section and page for each packet. One ICD quirk: the 3.3 document gives Minor Version = 2 in IG Control and Start of Frame, and the errata don't change it. Fovea sends 2, as written, and accepts any minor version on receive, checking only Major Version = 3.

## D-015 Tiling: a geographic quadtree

**Status:** Accepted (2026-11-06)

Terrain tiles split latitude and longitude in half at each level, from two root tiles (east and west hemispheres). The scheme covers the whole Earth, matches how sources are indexed, and has no zone seams (UTM grids, simpler for one small area, do). Each tile holds a square grid of heights with edges shared with its neighbours and an imagery texture; its vertices are relative to the tile's own ECEF origin, so the GPU stays camera-relative. Tiles shrink in longitude toward the poles; that is a cost only far from any likely database.

## D-016 Terrain level of detail: chunked quadtree by screen-space error

**Status:** Accepted (2026-11-06)

Each frame a tile splits into its children where its geometric error (recorded by the compiler) would show as more than about a pixel. Skirts hide cracks between levels. Popping is measured on the desk; morphing between levels (CDLOD-style) follows if it shows. Clipmaps are smoother but harder to combine with paging mixed-resolution sources.

## D-017 Terrain imagery: compressed, the format chosen per database

**Status:** Accepted (2026-11-06)

Imagery is stored as GPU-compressed textures with full mip chains, in a format chosen per database: BC7 (sRGB) by default, about 1 byte per texel, so 0.6 m imagery is 3-4 MB per km² with mips; BC1 at half that, blockier up close; uncompressed RGBA8 for exact colour at four times BC7's size. The compiler takes the format as a build setting and the manifest and each tile's imagery layer record it, so the runtime reads whichever it finds and one database can mix formats by region (detail where a deployment needs it, size elsewhere). Close-range and wide-area uses are both first-class (D-011), so neither format is assumed. Encoders come from a pinned library, chosen and licence-checked in milestone 3 step 4: bc7enc_rdo's `bc7enc` (BC7) and `rgbcx` (BC1), MIT or public domain, built only with the compiler (docs/milestone-3.md, step 4).

## D-018 Terrain database: a manifest and one file per tile

**Status:** Accepted (2026-11-06)

`manifest.toml` holds the schema version, area, layers and sources. Each tile is one binary file: a header (magic, schema version, tile key) and a table of named layers (elevation and imagery first; materials, placements and lights later), so a new layer needs no rebuild of the others (foundational decision 2). Binary, little-endian, with a checksum per layer.

Alternatives considered: one pack file per database (fewer files, but rebuilds and new layers rewrite it, and one damaged file loses everything); existing formats (Cesium 3D Tiles and quantized-mesh, KTX2, MBTiles), which don't cover material maps and placements cleanly; and **OGC CDB**, the simulation industry's open standard for synthetic environment databases (terrain, imagery, models, features, at many levels). CDB covers far more than milestone 3 needs and assumes a large toolchain, so Fovea starts with its own simpler format; a CDB importer or exporter can be added later (#33).

## D-019 The terrain compiler runs on Linux

**Status:** Accepted (2026-11-06)

GDAL is the compiler's main dependency. On Ubuntu 24.04 it is one package (`libgdal-dev`, GDAL 3.8.4); on Windows it means building GDAL and PROJ through vcpkg, or a conda environment. Databases are plain files any runtime reads, so the compiler builds and runs on Linux (WSL and CI) only, and the Windows runtime flies the result. A Windows compiler can follow if needed.

## D-020 Database heights are ellipsoidal

**Status:** Accepted (2026-11-06)

The compiler converts the sources' NAVD88 heights to WGS-84 ellipsoid heights with NOAA's GEOID18 (h = H + N), so the runtime works in one vertical datum. The database also carries the geoid height over its area, which lets the runtime convert a host's sea-level altitudes properly there (issue #19) instead of with one constant.

## D-021 Terrain uploads go on the render queue, for now

**Status:** Accepted (2026-11-07)

The plan has the paging thread upload tiles "through a transfer queue". Milestone 3 step 6 does everything but the hand-over on the paging thread (reading, decoding, making images, recording the copies) and has the render thread submit those command buffers ahead of its own frame, in the same submit, on its one queue. The render thread waits for nothing; a timeline semaphore every frame signals tells the paging thread when staging memory can be reused. This needs no queue-ownership transfers, and works where there is one queue (lavapipe). On the desk the uploads cost too little to show (GPU time at most 1.8 ms an output while paging a 150 m/s flight). A dedicated transfer queue follows if a heavier load shows the need: the paging thread would then submit itself, with ownership transfers to the render queue.

## D-022 Runways are smoothed in the compiler, from the elevation itself

**Status:** Accepted (2026-11-07)

The plan asks for flattened runways. The compiler takes each runway's rectangle from a committed airport file (the FAA's surveyed ends and the runway's width) and, inside it, replaces the finest level's heights with a smooth surface fitted to the 1 m lidar: smooth along the runway, keeping its grade changes, and across it, keeping its crown (a 1 to 1.5% cross slope puts a 150 ft runway's edges 0.2 to 0.35 m below its centreline), blended into the surrounding ground over the shoulders. Coarser levels are filtered from it as before, so each tile's recorded error stays true. That removes the lidar's noise and anything that stood on the runway when it was flown, without moving the surface off the survey. A straight line between the surveyed ends would not (runways have grade changes, so mid-runway it could sit off the real surface), nor would a level cross-section. Taxiways and aprons follow later.

## D-023 Models are glTF binary, read by a CPU-only library with cgltf

**Status:** Accepted (2026-11-07)

Models are glTF 2.0 binary files (`.glb`), as Blender exports them. `libs/models` reads them with no Vulkan dependency, like `libs/terrain`, so parsing is tested without a GPU and a host can use it too. The reader is cgltf (MIT): one file, no dependencies, and it writes glTF too, which tests use. fastgltf is faster but needs simdjson, and parsing is a small part of loading a model, which happens once per model on the paging thread.

## D-024 Model textures are uncompressed, for now

**Status:** Accepted (2026-11-07)

The paging thread decodes a model's PNG or JPEG textures with stb_image (built until now for PNG only, D-012, which left texture formats to this choice; JPEG is added) and builds their mips, and the GPU holds them as RGBA8. Model files stay plain glTF, straight from Blender, with no conversion step, and a few dozen landmarks take little of the memory budget. The cost is four times BC7's memory and upload, and the decode on the paging thread. Compressed textures (BC7 made offline, as terrain imagery is, or the standard `KHR_texture_basisu`) are revisited in milestone 7, when vegetation and many models make memory and uploads matter.

## D-025 Models are Fovea's own and generic, built by committed scripts

**Status:** Accepted (2026-11-07)

Each model is a Blender Python script in `data/models/` that builds it and exports a `.glb`. The scripts and the small `.glb` files are committed (own work, under the repository's licence), so models are reproducible and nothing proprietary enters the repository. Landmarks are generic: the right footprint, height and colour, and generic detail made by the script (window bands, doors, metal cladding, roof membranes and plant, hangars' arched roofs), not any building's own. Their roof colours are the imagery's over each footprint, measured once and written into the script, so they sit in the photograph round them; not the photograph itself on the roofs, which against generic walls would jar (decided in milestone 4 step 5). Each glTF material names its Fovea material ID in `extras`, checked when a model loads, so material IDs exist on models from the start, as the plan requires (Sensor views).

## D-026 Placements are a tile layer

**Status:** Accepted (2026-11-07)

The plan bakes hand-placed landmarks into tiles. A committed placement file per database lists each one (model, latitude, longitude, true heading). The compiler checks each model file exists and writes them into a `placements` layer (D-018) of the tile containing each, at a level the compiler settings choose and the manifest records, with the lowest terrain height under the model's footprint; models carry a plinth below their ground line, so on sloping ground nothing is buried or floats. Placements are drawn while their tile is resident, not only while it is selected, since a tile stops being drawn once it splits. For KHSV that level is 12, the pinned roots, so placements are always there; databases with more can place them finer, where they page with their tiles.

## D-027 Multisampling first, for anti-aliasing

**Status:** Accepted (2026-11-07)

The scene pass renders with multisampling (MSAA), 4 samples by default, the count set per deployment in config (1, 2, 4 or 8, as the GPU allows). It smooths every geometric edge and thin geometry, in every view alike, with no history: nothing smears under fast motion, and the channels stay consistent with each other. Temporal anti-aliasing (TAA) would also smooth shading, but blends earlier frames, so it needs motion vectors for every moving object and blurs or ghosts under a simulator's fast, steady motion; post-process filters (FXAA, SMAA) only guess at edges from the final image and crawl on thin geometry. Texture aliasing is already handled by mip chains and anisotropic filtering. MSAA costs memory (about 180 MB for a 2560 x 1440 view at 4 samples) and bandwidth, measured on the desk. TAA or SMAA can follow as options if shading aliasing shows. Alpha-masked materials use alpha to coverage, their alpha sharpened about their own cutoff, so their cut-out edges are smoothed too and fall where authored.

## D-028 A weighted resolve before tone mapping

**Status:** Accepted (2026-11-07)

Each pixel's colour is its samples' weighted mean in linear HDR, c = sum(c_i w_i) / sum(w_i) with w_i = 1 / (1 + L_i), L_i a sample's luminance (no weight is zero, so neither is the sum), by a resolve shader of Fovea's own, before the tone mapping pass. A plain average lets one very bright sample (a glint of the sun) swamp an edge, which then stays jagged once tone-mapped; the weighting keeps the edge's gradient. Point lights (airport lights) are drawn after the resolve, single-sampled and at least a pixel across, as the weighting would all but erase a light covering one sample, and it would flicker. The depth is resolved too, by the same shader, to the nearest sample, for later passes such as haze. (The render pass's own depth resolve can't do this on every GPU, lavapipe among them, and the validation layer CI uses, 1.3.275, misreports its ordering as a hazard.)

## D-029 Environment state is scene state

**Status:** Accepted (2026-11-08)

Date and time (UTC, as CIGI has it, an epoch with the host time it was set at, so a running clock is the epoch plus elapsed time and every channel agrees), whether time runs, visibility and the global weather layers are protocol-neutral scene state in `libs/core`, written by the host adapter or by config and read by the render thread from the snapshot. The renderer derives the sun's and moon's directions (the Almanac's low-precision sun; Meeus's truncated lunar theory with the eye's parallax; geometric, in double precision on the CPU) and the lighting from it each frame, and writes them with the atmosphere's parameters into a per-frame uniform buffer; push constants can't hold them.

## D-030 A physically based sky and aerial perspective, from precomputed tables

**Status:** Accepted (2026-11-08)

The sky and the haze over distance follow Hillaire (2020): transmittance and multiple-scattering tables, and a sky-view table and an aerial-perspective volume per view, rebuilt by compute shaders as the sun or atmosphere changes; Rayleigh and Mie scattering and ozone over a spherical Earth centred under the eye, its altitude the ellipsoidal height. The terrain and model shaders apply the aerial perspective per sample in the multisampled pass (a pass after the resolve would read one depth per pixel and haze edges wrongly). Visibility sets the aerosol density at the ground by Koschmieder's relation (extinction 3.912 / V). Bruneton's 4D tables cover every sun angle but take far longer than a frame to rebuild when the atmosphere changes (visibility, weather); analytic skies have no aerial perspective or twilight.

## D-031 One exposure for all channels, from the light

**Status:** Accepted (2026-11-08)

Exposure follows the illuminance at the eye from the sky model (sun, moon and sky), smoothed over about a second, the same in every view, shifted by config's offset. Exposure from each view's histogram would let neighbouring channels disagree and show the seams between displays.

The tone map applies its curve (the ACES fit) to each colour's brightest channel and scales the others with it (2026-11-09, milestone 6 step 8): at night's exposure, lights and lit windows are far past the curve's top, and the curve on each channel clipped them to white.

## D-032 Cascaded shadow maps, per view

**Status:** Accepted (2026-11-08)

The sun (or the moon, at night) casts shadows through 4 cascades fitted to each view's frustum, 2048² in 32-bit depth, filtered: 64 MB and four depth passes a view. Models cast onto terrain and each other, terrain onto models and itself. The terrain's imagery already holds its day's shadows, so terrain receives only what the imagery lacks: models' shadows, and terrain's at a low sun.

## D-033 Airport lights are a tile layer, drawn as points after the resolve

**Status:** Accepted (2026-11-08)

The compiler builds the `lights` layer (D-018) from the runways' FAA geometry (D-022) and a committed light file per airport naming each runway's systems from the FAA's published data, laid out by the FAA's standard (AC 150/5340-30) in code. The lights' points are drawn after the resolve (D-028), GPU-driven, single-sampled, at least a pixel across, by inverse square and the atmosphere's transmittance, coloured and directed by type. Clustered lighting (foundational decision 4) handles the light they cast on surfaces (none yet), not their points. Lit windows are models' emissive materials (glTF `emissiveFactor`, `emissiveTexture`, `KHR_materials_emissive_strength`).

## D-034 Clouds are 2D layers first; precipitation is visibility for now

**Status:** Accepted (2026-11-08)

Each global cloud layer is a lit, textured layer at its base (the ICD's mean-sea-level heights converted with the database's geoid height, D-020), its coverage shaping procedural noise, shadowing the ground below; ground fog is a band of density in the atmosphere; precipitation layers reduce visibility within them, their streaks waiting for particles (milestone 10). Volumetric clouds, regional weather and wet or snowy ground come later, as the frame allows.

## D-035 MSL is the host's: one constant, shared

**Status:** Accepted (2026-11-09)

A CIGI altitude is converted to and from ellipsoidal height with `[host] geoid_height_m`, the same constant the host uses (the team's simulator, Aerie under EYAS: `ellipsoid_minus_msl_m`, −28.25 m at KHSV, GEOID18 there). This covers entity positions, weather layer bases, and HAT/HOT altitudes and heights. Fovea and the host then agree on MSL exactly everywhere, which matters more than either matching the true geoid. The configs on both sides must match: a mismatch shifts the whole world vertically. A geoid grid waits for a host that uses a geoid model, or for areas large enough that one constant drifts by metres (#19, kept open to revisit).

## D-036 HAT/HOT are answered off the render thread, from the database's finest data

**Status:** Proposed (2026-11-09)

A query thread with its own `TerrainDatabase` reader answers HAT/HOT requests, which reach it from the network thread and go back to it without locks, to be sent in the next IG message. The render thread is never involved. Answers come from the finest tile that reads cleanly, the true ground rather than whatever level of detail a far view draws. Terrain only, for now (the team's choice); models aren't included.

## D-037 The database is WGS-84

**Status:** Accepted (2026-11-09)

The compiler converts positions and ellipsoidal heights from NAD83(2011), the sources' frame, to WGS-84 (a current realization, at an epoch written in the manifest), with PROJ through GDAL. It refuses to compile if PROJ offers only a ballpark transformation, which would leave the coordinates unchanged. The runtime and hosts are WGS-84 already; the frames differ by about a metre at KHSV (#37).
