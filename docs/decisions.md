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

**Status:** Accepted (2026-11-10). `LICENSE` is the license's text and `NOTICE` names the copyright holder; `THIRD_PARTY_NOTICES.md` lists the libraries Fovea builds with, each checked as compatible.

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

GoogleTest 1.15.2 is used for CPU-side unit tests (math, config parsing, containers, terrain queries), discovered and run through CTest (`gtest_discover_tests`). It matches the flight-model repo Fovea was set up beside, so the two share tooling and habits. GPU and image tests go through the headless and replay harness instead.

## D-008 Compilers, warnings and CI (adapted from a sibling repo)

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

The sky and the haze over distance follow Hillaire (2020): transmittance and multiple-scattering tables, and a sky-view table and an aerial-perspective volume per view, rebuilt by compute shaders as the sun or atmosphere changes; Rayleigh and Mie scattering and ozone over a spherical Earth centred under the eye, its altitude the ellipsoidal height. The terrain and model shaders apply the aerial perspective per sample in the multisampled pass (a pass after the resolve would read one depth per pixel and haze edges wrongly). Visibility sets the aerosol density at the ground by Koschmieder's relation (extinction 3.912 / V; the WMO's ln 20 / V since D-048). Bruneton's 4D tables cover every sun angle but take far longer than a frame to rebuild when the atmosphere changes (visibility, weather); analytic skies have no aerial perspective or twilight.

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

A CIGI altitude is converted to and from ellipsoidal height with `[host] geoid_height_m`, the same constant the host uses (the first host's `ellipsoid_minus_msl_m`; at KHSV −29.55 m since D-037, GEOID18's −28.25 m less the 1.30 m from NAD83(2011) to WGS-84). This covers entity positions, weather layer bases, and HAT/HOT altitudes and heights. Fovea and the host then agree on MSL exactly everywhere, which matters more than either matching the true geoid. The configs on both sides must match: a mismatch shifts the whole world vertically. A geoid grid waits for a host that uses a geoid model, or for areas large enough that one constant drifts by metres (#19, kept open to revisit).

## D-036 HAT/HOT are answered off the render thread, from the database's finest data

**Status:** Accepted (2026-11-09)

A query thread with its own `TerrainDatabase` reader answers HAT/HOT requests, which reach it from the network thread and go back to it without locks, to be sent in the next IG message. The render thread is never involved. Answers come from the finest tile that reads cleanly, the true ground rather than whatever level of detail a far view draws. Terrain only, for now (the team's choice); models aren't included.

## D-037 The database is WGS-84

**Status:** Accepted (2026-11-09)

The compiler converts positions and ellipsoidal heights from NAD83(2011), the sources' frame, to WGS-84 (a current realization, at an epoch written in the manifest), with PROJ through GDAL. It refuses to compile if PROJ offers only a ballpark transformation, which would leave the coordinates unchanged. The runtime and hosts are WGS-84 already; the frames differ by about a metre at KHSV (#37).

## D-038 64-bit builds only; no build path in a Windows binary

**Status:** Accepted (2026-11-17)

- **64-bit only.** A 32-bit target makes Vulkan's non-dispatchable handles 64-bit integers rather than pointers, and the build failed deep inside (`reinterpret_cast` from `VkSurfaceKHR`) when configured from Visual Studio's default developer shell, whose `cl` targets x86. CMake now refuses a target without 8-byte pointers, saying which tools to use.
- **No build path in the binary.** An installed IG would otherwise carry its builder's name, twice over on Windows:
  - MSVC's linker writes the PDB's full path into each executable (its debug directory), usually under the builder's home folder. `/PDBALTPATH:%_PDB%` records the PDB's file name alone; a debugger still finds it beside the executable or through a symbol path.
  - Sources' paths are compiled in as `__FILE__`: SDL's assertion messages, fetched into the build tree, named the builder's home after the first fix. MSVC's `/d1trimfile:` (undocumented, long supported, the counterpart of GCC's and Clang's `-ffile-prefix-map`) trims the source and build directories off them, for every target and dependency compiled after the flags are set; GCC and Clang get `-ffile-prefix-map` for both. A debugger is pointed at the sources by its source path.

## D-039 A host's particle volumes arrive as a stream of self-contained puffs, apart from CIGI

**Status:** Accepted (2026-11-19)

A particle simulation (rotor-wash dust, smoke, spray) sends what it simulates to Fovea for the views to draw. CIGI 3.3 has nothing for a particle field: its weather regions would throw a simulation's motion away, and its user-defined packets would put a few thousand datagrams a second on the host link that carries entity updates and terrain answers.

- **Puffs, not a density grid.** Each particle is a sphere of uniform density with a velocity, in a local east-north-up frame. A puff keeps its own motion, which the IG continues between updates, so dust moves smoothly at 60 Hz from a 5–30 Hz sender. A grid on the wire would have to be cross-faded, smearing exactly the motion a pilot reads (moving dust gives false self-motion cues), and fixes one resolution everywhere. Particles are also how real-time simulators have shown rotor-wash dust (a particle system in DLR's image generator, Aeronautical Journal 2011; a loosely coupled brownout model feeding the U.S. Army's simulators' IG, I/ITSEC 2008).
- **Every datagram stands alone:** a header, then whole puffs, each under 1472 bytes (never fragmented). No frames or chunks to reassemble: a lost datagram costs its own puffs one refresh. A particle takes a record unless it holds a newer one by the sender's time (an equal time is taken: a frozen sender's refreshes), and fades out when not refreshed within the header's hold time. Senders may refresh particles at different rates.
- **The origin by id.** The frame's origin is sent in its own datagram, about once a second, under an id the puffs name; a new origin is a new id, so a switch never misplaces datagrams in flight. A new session from a source (a restarted sender, its time from zero) resets that source.
- **Time is the receiver's.** A particle moves on its velocity from when its record arrived, on Fovea's clock, not from the sender's time: no clock mapping to break across a freeze or a restart.
- **Its own thread and its own state.** A `PuffAdapter` on a `PollThread`, apart from CIGI's, keeps up to a configured capacity by (source, id) in a table sized at start, and publishes `ParticleFrame`s (core, protocol-neutral) through a triple buffer of their own: a frame is megabytes, too large for `SceneSnapshot`. Nothing allocates after start.
- **Drawing is the next step:** soft, depth-tested particles lit by the sky, their optical depth from bulk density and a configured mass extinction coefficient; a volume filled from the particles each frame only if that can't hold 60 Hz with the eye inside a cloud.

The format is in `docs/integration-guide.md` ("Particle volumes").

## D-040 Particle volumes are drawn as spheres of uniform density, unsorted, after the resolve

**Status:** Accepted (2026-11-19)

The particles D-039 receives are drawn into each view's resolved HDR target after the stars and airport lights, single-sampled (`shaders/particles.slang`, `ParticlesGpu`).

- **Each particle is a sphere of uniform density.** It is drawn as a quad facing the eye that exactly covers the sphere's silhouette, or covers the whole view when the eye is inside it or close to it. Each pixel takes the length of its ray inside the sphere as optical depth, giving opacity 1 − e^(−σℓ), with σ the particle's bulk density times a configured mass extinction coefficient. The edge is soft by itself: the chord goes to nothing at the rim. The intersection is computed as in Ray Tracing Gems, chapter 7 [RTG7], for float precision: the discriminant as r² − l² (its equation 5), so a small puff far off isn't lost, and the roots as q and c/q (its equation 10), about twice as precise for the nearer when the eye is close to a large puff's surface (c = |c|² − r² still cancels there, as in the chapter; the error is well under a millimetre either way). Over 20 000 random cases in float32 against a double reference, the worst chord error is 0.06 % of the radius, against 88 % (and two misses) for the textbook b² − 4ac; almost all of that is equation 5.
- **Cut at the scene's depth.** The chord ends where the resolved depth says something nearer is, so dust meets the ground and the aircraft softly, with no seam, and dust behind a hill isn't drawn. The resolved depth is sampled for this, and the reference grid now writes its ground's depth so it does the same with no database.
- **Lit by the view's sun, moon and sky** (`ViewSky`, at the view's exposure).
  - The sun and moon are scattered with the Cornette–Shanks phase function [CS92], the sky's Mie phase (D-030), in the form Hillaire gives [H20, section 4]. It integrates to 1 over the sphere (checked numerically).
  - The sky's light is taken as coming evenly from the upper hemisphere: its irradiance over 2π.
- **Light scattered more than once** is what makes a thick cloud bright. It is adapted from Wrenninge, Kulla and Lundqvist's octaves [WKL13, equations 1 and 2], which Hillaire used for Frostbite's clouds [H16, page 41].
  - b = c = ½, their defaults: each octave carries half the light and half the eccentricity of the one before.
  - Three octaves, not their default eight: they're evaluated for every pixel of every overlapping puff, and the five left out are 12 % of the eight's sum (0.24 of 1.99).
  - a = ½, their default, since D-041: each octave's sun reaches the point through aⁱ of the light path's optical depth, from the light buffer. Before D-041 there was no light path and no a, and the approximation wasn't energy-conserving. With a = b the scattering is scaled no more than the extinction, which is the condition Hillaire gives [H16, page 41, equation 20, where his letters are swapped: his a ≤ b is b ≤ a in Wrenninge's]. Where nothing shadows, the octaves still sum to 1.75 times the light once scattered, as the approximation does, so the albedo stays a tuned colour (below).
  - Single scattering alone left sunlit dust several times darker than the ground.
- **Shadows:** the cloud shadows itself (D-041); the terrain's shadow on the dust isn't drawn yet.
- **Unsorted.** Each particle is composited over what is behind it, premultiplied. Particles of one colour give the same result in any order, and here their colours differ only smoothly, with the angle to the sun. Sorting 200 000 particles each frame, or a weighted order-independent pass, is left until the order is seen to show.
- **Packed by the render thread at the frame's start.** Each particle is moved on its velocity to when the frame is seen, faded, and has its extinction folded in; invisible ones are left out. The result goes into a host-visible buffer per frame slot, with no allocation. The origins are placed relative to the first view's eye in double, so the GPU sees offsets of metres.
- **The look is configuration** (`[particles]`: `extinction_m2_per_kg`, `albedo`, `forward_scattering`). Two defaults are mineral dust's, as calculated by Mie theory (spherical particles) from size distributions measured from aircraft [R18, section 2.6, table 6]; one is chosen:
  - **Extinction, 320 m²/kg.** Saharan dust over its full size range (0.1 to 100 µm) at 550 nm: 0.27 to 0.35 m²/g, mean 0.32 [R18].
  - **g, 0.68.** The same dust's asymmetry parameter at 550 nm is 0.74 [R18]. Cornette–Shanks' g isn't the asymmetry parameter (the mean cosine), and 0.68 gives a mean cosine of 0.74, computed numerically.
  - **Albedo 0.62, 0.52, 0.40, chosen, a tan.** The same dust's single-scattering albedo is about 0.95 at 550 nm, calculated the same way [R18]. A thick cloud's tan comes from many scatterings compounding small spectral differences in it, which the octaves don't model, so the colour is set and tuned by eye against video instead.
  - **A rough check of the extinction against the conditions it meets.** Koschmieder's visibility for a black target is about 3.9 over the extinction, for the eye's 2 % contrast threshold [LS16, equation 9]. Lee and Shang find the model doesn't hold for visual ranges of hundreds of metres or less, naming severe dust storms, so this is an order of magnitude only. At 320 m²/kg:
    - a 10 mg/m³ dust storm [E19] gives about 1.2 km;
    - the up to 3500 mg/m³ a V-22's brownout cloud "has been shown to produce" [E19, citing a source not read] gives about 3.5 m;
    - 0.1 mg/m³, a dusty background [E19], gives about 120 km.
- **Not measured yet:** the cost at 200 000 particles on a real GPU with the eye inside the cloud (overdraw). A volume filled from the particles each frame and marched (a game engine's fog technique) stays the fallback if quads can't hold 60 Hz there.

Sources (each read for this, 2026-11-19, except where said):
- [CS92] W. M. Cornette and J. G. Shanks, "Physically reasonable analytic expression for the single-scattering phase function", Applied Optics 31(16), 3152–3160 (1992), doi:10.1364/AO.31.003152. Not read: paywalled, as is its erratum (Applied Optics 34(4), 641, 1995). The formula used is as [H20] gives it, and Bruneton's reference implementation (precomputed_atmospheric_scattering, `MiePhaseFunction`) agrees.
- [H20] S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique", Computer Graphics Forum 39(4) (EGSR 2020), section 4. Its source for the formula is Thomas and Stamnes, *Radiative Transfer in the Atmosphere and Ocean*, Cambridge University Press (1999).
- [WKL13] M. Wrenninge, C. Kulla and V. Lundqvist, "Oz: The Great and Volumetric", ACM SIGGRAPH 2013 Talks, 46:1, doi:10.1145/2504459.2504518. Equations 1 and 2 and their defaults.
- [H16] S. Hillaire, "Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite", SIGGRAPH 2016 course: Physically Based Shading in Theory and Practice, course notes, section 5, page 41 (equations 19 and 20, and the a ≤ b condition).
- [RTG7] E. Haines, J. Günther and T. Akenine-Möller, "Precision Improvements for Ray/Sphere Intersection", in Ray Tracing Gems, E. Haines and T. Akenine-Möller (eds.), Apress (2019), chapter 7, equations 5 and 10. Equation 5 is after Hearn and Baker, equation 10 after Press et al.
- [R18] C. L. Ryder et al., "Coarse-mode mineral dust size distributions, composition and optical properties from AER-D aircraft measurements over the tropical eastern Atlantic", Atmospheric Chemistry and Physics 18, 17225–17257 (2018), doi:10.5194/acp-18-17225-2018. Its values are calculated with a Mie code, the particles taken as spheres, from size distributions measured from aircraft (section 2.6); table 6 gives them at 550 nm. Its corrigendum (2020) corrects only table 5 (the lognormal modes), the derived results unchanged.
- [LS16] Z. Lee and S. Shang, "Visibility: How Applicable is the Century-Old Koschmieder Model?", Journal of the Atmospheric Sciences 73 (2016), doi:10.1175/JAS-D-16-0102.1. Its equations 8 and 9 (threshold 0.02, a black target), and its finding that the model doesn't hold for visual ranges of hundreds of metres.
- [E19] M. Ellis et al., "Reduced-Order Modelling of Mineral Dust Deposition in Turboshaft Engine Hot Sections", 45th European Rotorcraft Forum (2019), paper 37, pages 1 and 2. The V-22 figure is its reference 3's, not read.

## D-041 Particle volumes shadow themselves by half-angle slicing

**Status:** Accepted (2026-11-20)

D-040's dust was lit as if nothing stood between a puff and the sun, so a thick cloud had no shadowed side and read as a flat shape (seen in renders over KHSV). It now shadows itself by half-angle slicing [G13], after Kniss et al.'s volume shading (GPU Gems, 2004, as [G13] cites it):

- **Sorted along the half vector.** The particles are sorted along the vector halfway between the view and the light's direction of travel. With the view looking the way the light travels (their dot product positive), that's the two's half vector, and the slices go front to back from the eye; otherwise it's the reversed view's and the light's, and back to front. Either way they go front to back from the light [G13]. ([G13]'s pseudocode tests the dot product against 1.0; its text, which is followed, says positive.) The sort is a radix sort on the render thread, over buffers sized at start, one per view.
- **The view's direction is its frustum's central ray** (where clip x and y are 0), not its axis: a lopsided channel's rays all lie to one side of the axis. The order is right for rays within about 90° of the half vector, as [G13] assumes ("for a given view ray"). A ray more than about 45° off the central ray, with the sun side-on, can be composited in reverse. That's a limit for channels wider than 90°; KHSV's are 51°.
- **Drawn in 32 slices** ([G13]: 32 to 128 a good trade-off). Each slice goes first into the view's particle image ("under" front to back, "over" back to front, premultiplied: [G13]'s blending), lit by the light buffer as the slices before it left it. Then it goes into the light buffer.
- **The light buffer holds optical depth, added.** It is 512², orthographic along the sun's travel over the particles' bounding sphere. Each puff adds its own optical depth along the light (its extinction times the chord a light ray crosses), so the order within a slice doesn't matter. [G13] accumulates opacity instead.
  - **Its format is R32F** where the GPU blends and filters it, else R16F, which every GPU does. Half floats' 11 bits stop adding the small depths of thin puffs once the sum nears 4 to 8, and a thick cloud's far side would come out too bright.
  - **One buffer covers every particle.** Particles from emitters far apart (kilometres) make its texels metres wide, and small puffs then cast little or flickering shadow. That's a limit to lift with a buffer per cluster if it's ever met.
- **Only the views the frame was given are drawn.** An output beyond them (the renderer draws it with an empty view) has no sorted order and draws no particles.
- **The sun's light** at a puff is read at the middle of the view ray's path through it. Each multiple-scattering octave i takes e^(−aⁱτ) of it, Wrenninge et al.'s equation 2 with a = ½ [WKL13]. The sky's and the moon's light aren't shadowed: the light buffer is the sun's.
- **The particle image is composited over the scene** at the end, premultiplied. The depth cut (D-040) stays per pixel.
- **Costs to measure on a real GPU:** per view, a sort of the particles and 63 rendering passes (32 into the image, 31 into the light buffer). [G13] notes the technique is fill-rate bound and that the particle image can be drawn at a lower resolution; neither is measured here yet.

Measured on lavapipe: a puff with a thick one between it and the sun is 22 % darker, summed over RGB, than the same puff alone, and bluer, because the sky's light isn't shadowed.

Sources:
- [G13] S. Green, "Volumetric Particle Shadows", NVIDIA (September 2013), the CUDA sample smokeParticles' paper. Read for this, 2026-11-20.

## D-042 The particle pass's cost is measured and can be traded against its look in config

**Status:** Accepted (2026-11-20)

Particle volumes (D-039 to D-041) are core, not a module. The plan's "Core services and modules" lists the particle system in core, and domain behaviour on top of it (rotor downwash, brownout) as modules. So the pass stays in the renderer. What the plan asks of anything with a GPU cost still applies: its cost is reported, and it can be turned down, without a rebuild, on hardware where it hasn't been measured.

- **Its GPU time is reported.** Each output's timestamps grow from two to four: around its whole work, as before, and around its particle pass inside it. Every one is written on every path (a skipped output, a cleared frame), or a frame's results can't be read. `FrameTiming::particles_ms` per output is shown in the overlay ("GPU output N: x ms (particles y)"), written to the stats CSV as a second per-output list after `gpu_ms`, and summarised at exit (mean and max).
- **`[particles]` trades cost against look,** fixed at start:
  - `self_shadowing` (true): off, the particles are drawn in one slice with no light pass;
  - `slices` (32, 1 to 128);
  - `light_buffer` (512 texels a side, 64 to 2048);
  - `resolution` ("full" or "half"). Half draws the particles at half each view's width and height, about a quarter of the fill, and composites them up with bilinear filtering, Green's suggestion [G13, D-041]. Each half-size pixel is cut at the nearest of the scene depths it covers, so dust doesn't show over nearer scenery, but its edge against that scenery is half as sharp. That's 2 × 2 at even sizes and up to 3 a side at odd ones, where the half image is (n + 1) / 2 and a pixel covers a little under 2; the range is worked out from the sizes, checked to cover every overlapped pixel at sizes from 90 to 1920.
- **Not measured yet:** what any of these cost on KHSV's IG. These are the levers for that measurement, and their defaults are the full look.


## D-043 Particle volumes take their detail from one field of noise, swirled by curl noise

**Status:** Accepted (2026-11-20)

Drawn as uniform spheres (D-040, D-041), a cloud of puffs read as a heap of bubbles, its texture the puffs' own outlines. Its density now varies by one field of noise shared by every particle, after Schneider and Vos's clouds [SV15], with its lookup swirled by curl noise [BHN07]. `[particles]` `detail` 0 keeps the uniform spheres and costs nothing.

- **Two small volumes, built once at start on the CPU** (`particle_noise.hpp`; no Vulkan, tested without a GPU), tiling in all three directions, sampled with linear filtering and wrapping:
  - **Detail: inverted cellular noise**, 3 octaves at doubling frequencies and halving amplitudes, 4 cells a side at the first, 32³, R8; stretched to span 0 to 1. Inverted cellular noise is what [SV15] found makes "tightly packed billow shapes" (slide 31), and their detail texture is 32³ cellular noise "at increasing frequencies" (slide 34). The cellular noise is the distance to the nearest of one random point per cell, after Worley (SIGGRAPH 1996), whose paper couldn't be fetched and wasn't read: one point per cell may be simpler than his scattering.
  - **Swirl: curl noise**, 32³, RGBA16F: the curl of a vector potential of three Perlin noises, by central differences ([BHN07] equation 1 and 2.1), so divergence-free: it swirls without gathering anywhere. The three are variants of one noise, their first hash lookup offset as by another permutation. [BHN07] suggest "the same noise function evaluated at large offsets" (section 2.2), but this noise wraps: whole periods give the same function back, and the curl of (f, f, f) swirls only across (1, 1, 1) (the review found it; a test now checks the swirl has no such plane). Perlin's improved noise is his reference implementation's (https://mrl.cs.nyu.edu/~perlin/noise/, read): its fade, gradients, corner hashes and lerps, with the lattice wrapping at a period and a permutation of its own. [SV15]'s curl texture is 2D; this one is 3D.
- **One field in the world, not one per particle.** [SV15]'s noise is the world's, and so is this: in each particle's origin's frame (east, north, up, metres), its features `detail_size` metres (2 by default). Overlapping particles make one cloud, and its billows run across them. Noise in each particle's own frame was tried first: over KHSV a cloud of 20 000 puffs looked no different, because every visible point is inside some puff and each puff's own detail averages away.
- **The density is varied everywhere, and its mass kept.** [SV15] erode only their cloud's edges, "subtracting the second 3d texture at the edges of the cloud" (slide 39), from one density field. Here a cloud is many particles, and its inside is the inside of each, so the field varies the density throughout. At a point a fraction r of a particle's radius from its centre, its density is its uniform density times

  base(r) × (1 − detail × (1 − n)) / (1 − detail × (1 − n̄)),

  where n is the field there (0 to 1) and n̄ its mean over the volume, 0.4586 (tested against the shader's constant). The division keeps the particles' mass on average: the sender's bulk density is a mass, and the detail moves it about, it doesn't remove it. Measured on lavapipe: a thin puff's cover summed over its disc is 1.5 % less than the uniform sphere's (cover grows slower than optical depth, so denser patches cover less than their share; the test allows 6 %); without the base's division it is 24 % less, without the detail's 42 % less.
- **Each particle's edge is softened:** base(r) = min(1, 3(1 − r²)) / 0.7645, 1 inside r² = ⅔ and 0 at the surface, so a particle's outline doesn't show as a sphere's. The divisor is its mean over the sphere's volume, the integral of 3r² min(1, 3(1 − r²)) from 0 to 1, so it keeps the mass too (the outer shell holds 46 % of the volume).
- **Swirled, drifting in time.** The field's lookup is moved by the curl volume, up to `swirl` features (0.5), and the curl volume's lookup drifts slowly with the frame's time (0.03 tiles a second, wrapped every 1000 s), as [SV15] distort their detail "to fake the swirly distortions from atmospheric turbulence" (slide 39).
- **Optical depth along a ray:** the base is integrated exactly along the chord (it is a polynomial in the distance along it; the depth cut included), and the noise's factor is its mean at 4 points evenly along the chord, weighted by the base there: 8 texture reads per pixel per particle (the field and its swirl at each point). Sampling the base too, the 4 points miss each chord's soft ends: through the centre, all four fall where it is 1, 10 % too much (the review's question about the mass test's margin found it). With `detail` 0 the chord's optical depth is exact, as before.
- **The light buffer isn't detailed:** shadows (D-041) are cast by the uniform spheres. That's cheaper, and the light buffer's texels are larger than the detail at 512² over a cloud of any size.
- **Chosen, to tune by eye** (not measured): `detail` 0.6, `detail_size` 2 m, `swirl` 0.5. The sender's particles carry no detail of their own, and none of these come from measured dust.

Measured on lavapipe, a puff of optical depth 1 through its middle: its cover changes from pixel to pixel 2.3 times as much with detail as without (0.8, features 1 m). Its GPU cost is measured only on site (D-042's overlay and stats).

Sources:
- [SV15] A. Schneider and N. Vos, "The Real-time Volumetric Cloudscapes of Horizon: Zero Dawn", SIGGRAPH 2015, Advances in Real-Time Rendering in Games (slides, PDF, http://advances.realtimerendering.com/s2015/). Read for this, 2026-11-20: slides 31, 34, 35, 39 and 78. The exact remapping of Schneider's later chapter (GPU Pro 7, 2016) wasn't read, and isn't used.
- [BHN07] R. Bridson, J. Hourihan and M. Nordenstam, "Curl-Noise for Procedural Fluid Flow", ACM Transactions on Graphics 26(3), SIGGRAPH 2007, article 46, doi:10.1145/1239451.1239497. Read for this, 2026-11-20 (the author's copy).
- K. Perlin, "Improved Noise reference implementation" (2002), https://mrl.cs.nyu.edu/~perlin/noise/, for his SIGGRAPH 2002 paper. Read for this, 2026-11-20.

## D-044 Headless with a host: the host's loop, offscreen

**Status:** Accepted (2026-11-20)

A headless run (`--headless`, or the offscreen presenter) drew `--frames` frames of the reference scene and ignored `[host]`: nothing could drive Fovea without a display. A host's own tests need that: a host flying a scripted run against an IG on a machine with no display (a build container, a CI runner), checking what the IG drew for its poses. The plan's control and stats plane lists headless and replay modes for regression and golden-image tests (architecture.md, "Invariants").

- **With `[host]`, a headless run is the on-screen run with no windows.** The loop (the render thread's frame clock, the scene state, CIGI and its Start of Frame each frame, the HAT/HOT query thread, particle volumes, `--stats`, `--overlay` into the first channel's image) is one function (`run_loop`) for both, waiting for what ends the run through a callback: window events on screen, a sleep and a signal flag headless. So what a host's test runs against is what a site runs, less the swapchains. Without `[host]`, headless is as before.
- **It stops after `--seconds`, or at SIGINT or SIGTERM**, and prints what the host link did, as on screen. With `--dump DIR`, each channel's last frame is then written as `DIR/<channel>.png`: what the IG drew for the host's last pose (not if the run failed, or drew nothing). An offscreen-presenter config with `[host]` runs so too without `--headless`. `--frames` and `--identify` are refused (the host drives it); with `--clear` the host isn't used, and Fovea says so.
- **Paced by its own clock:** with no display to pace it, the loop sleeps to the next 60 Hz deadline (as it already did when nothing was drawable). On a software Vulkan driver (lavapipe) a frame can take longer than a period, and the loop counts it missed, as on screen.
- **Tried:** `App.HeadlessFollowsAHost`: Fovea headless and `cigi-test-host` started together (a CMake script), on ports of their own: the host's entity updates taken (at least 60 in 4 s), Start of Frame messages sent, no validation messages, the last frame written. `--frames` with a host refused; `--clear` with a host saying it isn't used. By hand: 480 frames on lavapipe in 8 s, 378 entity updates, one Start of Frame for each frame; a port taken: the bind refused and no read-back error after it. The whole suite as before (the loop moved, unchanged). Mutation: headless with `[host]` routed to the reference scene again: the test fails.
- **Review** (a fresh reviewer, each finding checked; fixed): a failed start also printed a read-back error (dumped though nothing was drawn); `--frames` with a host was ignored; `--clear` with a host dropped it silently and its refusal named the wrong reason; `usage()` lacked the mode; the offscreen presenter's behaviour with `[host]` was undocumented; the test claimed a Start of Frame per frame, matched "10 validation messages" as "0", and wasn't in CI's slow tests.

## D-045 A jump in time or place takes its exposure at once

**Status:** Accepted (2026-11-20)

The exposure eases toward its target with a time constant of a second (D-031), as eyes adapt, by the frame clock. A user saw it wrong on site: after the time of day was set, the picture lagged a few seconds behind; and jumping to a place, or freezing, sometimes washed the screen out white for a few seconds before it came back. Reproduced against a host (headless, D-044, each frame's mean brightness read back): with the clock set from 17:00 to dusk, the frame's mean fell from 136 to 69 and took about 4 s to reach 108; set back to day, it rose to 179 and took about 6 s to fall to 136. From night to day the exposure is about 18 stops off: white for many seconds (the user saw ten or more), and longer where the frame rate is below 60 (it eases by frames). An exercise applied sets its time of day, and its placement moves the eye: a "jump to a place" is both.

- **A jump takes the target at once**, as the first frame always has: the sun more than half a degree from the last frame's (it moves a quarter of a degree a minute: more is the clock set), or the first view's eye more than a kilometre from the last frame's (60 km/s: a reposition). Between jumps, the exposure eases as before: the sun setting, a climb, the world moving on.
- **A jump takes the target itself** (`cs_exposure`, with the frame's easing at 1), as the first frame does: easing all the way, a + (b - a) 1, missed it by up to 9 units in the last place (the review's finding).
- **Not detected:** weather set by the host at once (an overcast arriving) eases as before; a reposition of under a kilometre at the same time of day changes the light little.
- **Freezing:** the user also saw it after a freeze. A freeze stops the host's ephemeris (the sun stops) and moves neither the sun nor the eye, so it isn't a jump, and it wasn't reproduced: what was seen likely came with a time set or a placement (an exercise applied does both). A freeze alone did it too, by another cause: D-047.
- **Tried:** `Sky.AJumpTakesItsExposureAtOnce`: after day frames, the clock set to night gives night's exposure bit for bit, as a renderer drawing only night from its first frame (about 18 stops from day's); the eye 10 km up gives that place's bit for bit; 900 m up eases under a tenth of the way to its target. Mutations, each caught: the sun's jump not seen; the eye's not seen; every 100 m a jump. Against a host: set to dusk, the frame's mean went from 136 to 108 in a frame; back to day, from 108 to 136.

## D-047 A frozen entity's pose stays a number, and light that isn't one leaves the exposure

**Status:** Accepted (2026-11-20)

On site, a few times, pressing freeze turned the screen white for ten seconds or more; most freezes didn't. Reproduced with a host freezing and running forty times: three of the forty went white. The frames show why. A frozen host sends the same pose every frame. `pose_at` took the rotation between its two samples, cur × inverse(prev): with two equal samples its axis part is exactly zero, but its w can round to just below 1 (it depends on the attitude: yaw 0, pitch −15.9, roll −30 does it). The angle, from w alone (acos), came out small but not zero, so the axis, normalised from zero, was NaN, and the eye with it, every frame of the freeze. The light read at a NaN eye is NaN; `max(NaN, 1e-12)` is 1e-12 on a GPU, the far end of the night floor, so the exposure eased up toward it, and when the host ran again the picture was white, coming back as it eased down.

- **The angle from both parts:** 2 atan2(|axis part|, w), and the axis only where its length is above zero. Equal samples give an angle of zero and the pose as sent, at every attitude.
- **Light that isn't a number leaves the exposure as it was** (`cs_exposure`): whatever else makes a NaN eye or light one day, it can't drive the picture white. Tested on its bits (exponent all ones), since without SPIR-V's float controls a driver may assume no NaN and fold `isnan`/`isinf` and unordered compares away (the review's finding). Before any exposure, a neutral one (1), the state untouched, so the first good frame takes its own at once (a first NaN frame had left it about 40 stops over).
- **Tried:** `PoseAt.AFrozenEntityStaysFiniteAtEveryAttitude` (4500 attitudes, two equal samples): fails before the change (yaw 0, pitch −15.9, roll −30), passes after. `Sky.LightThatIsntANumberLeavesTheExposure`: three NaN frames keep the exposure, the next good one goes on from it; `Sky.LightThatIsntANumberFirstLeavesNoExposure`: a NaN first frame leaves 1, and the next good frame matches a fresh renderer's first.

## D-048 Visibility is the WMO's meteorological optical range

**Status:** Accepted (2026-11-20)

Over KHSV at dusk the scene looked far too hazy: a user judged that what the exercises call 20 to 50 km looked like thick haze, and that Fovea's 200 km looked like an ordinary clear day. D-030 had set the aerosols from a visibility by Koschmieder's 3.912 / V, its threshold contrast 0.02, named but not read from any source.

- **The WMO's definition:** the meteorological optical range, MOR = ln(1/0.05) / σ ≈ 3 / σ, where a black object's contrast against the horizon falls to 0.05 [WMO18, section 9.1.4, equation 9.6]. The guide says Koschmieder proposed 0.02 in 1924, that for aeronautical use "it is taken as 0.05" [WMO18, section 9.1.3], and that "MOR has been formally adopted by WMO as the measure of visibility for both general and aeronautical uses" [WMO18, section 9.1.3]. Hosts' visibilities (a weather report's, an exercise's) mean that. `with_visibility`, and the fog, rain and snow bands, now take ln 20 / V (`kExtinctionTimesVisibility`): for the same number, 23% less extinction than 3.912.
- **Hillaire's default atmosphere is clearer still:** its aerosols, Bruneton's [B17: extinction 5.328e-3 over the 1.2 km scale height, 4.44e-6 per metre, scattering 0.9 of it], with Rayleigh's 13.558e-6 in green [H20, table 1], give an extinction of 18.0e-6 per metre at the ground: a MOR of about 166 km by the WMO's equation. That is near what the user picked by eye. Rendered again with ln 20, 150 km looks as the old 200 km did. So the haze was the visibilities asked for, made a third heavier by the older threshold: a clear day is set as 150 km or so, not 50.
- **The model is outside what its author tested at low visibility:** Hillaire's multiple scattering is an approximation that "drifts from ground truth due to dense atmosphere or strong anisotropic phase function" [H20, section 8], and "when using very high scattering coefficients, the hue can be lost" [H20, section 6]. At 20 km the aerosols are about 30 times his default's. Not changed here; a reason to check hazy skies against a reference before trusting their look.
- **A slip in a source:** [H20, table 1] gives the Mie absorption as 4.40 (×10⁻⁶ per metre); its source's code [B17] gives the extinction as 4.44 and the scattering as 0.9 of it (absorption 0.444). Fovea keeps [B17]'s, as it did.
- **Koschmieder's law, by the guide, holds only where "the scatter coefficient is independent of the azimuth angle and ... there is uniform illumination along the whole path" [WMO18, section 9.1.4]**: looking toward a low sun it doesn't, and the haze there is brighter than a visibility alone says. That is physics, kept.
- **Tried:** `Sky.VisibilityIsWhereTheHazeLeavesFivePercent`: along a level ray from sea level, the transmittance falls to 5% at 20.2 km with the visibility set to 20 km and at 41.8 km with 40, within 10% (Koschmieder's 3.912 would put it at 77%). The airport lights' fog test takes 600 m, the fog its 800 m gave before. The golden images are regenerated: the sky a little bluer, far ground a little clearer.

Sources (each read for this, 2026-11-20, except where said):
- [WMO18] World Meteorological Organization, *Guide to Instruments and Methods of Observation* (WMO-No. 8), Volume I, Chapter 9, "Measurement of visibility", preliminary 2018 edition (old.wmo.int, CIMO-Guide, Prelim_2018_ed, 8_I_9_en). Sections 9.1.1 to 9.1.4, equations 9.6 and 9.9 to 9.10. The current edition was not read.
- [H20] S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique", Computer Graphics Forum 39(4) (EGSR 2020), the author's copy: section 4 and table 1, sections 5.5.1, 6 and 8.
- [B17] E. Bruneton, *Precomputed Atmospheric Scattering: a New Implementation* (2017), github.com/ebruneton/precomputed_atmospheric_scattering, `atmosphere/demo/demo.cc`, the constants kMieAngstromBeta, kMieScaleHeight and kMieSingleScatteringAlbedo. [H20]'s [Bru17b]. Its paper, [H20]'s [Bru17a], was not read.
- Koschmieder's 1924 paper was not read: what it proposed is as [WMO18] gives it.
