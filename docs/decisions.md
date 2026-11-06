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

Headless dumps and golden-image tests read and write PNG. `stb_image` 2.30 and `stb_image_write` 1.16 are two public-domain headers with no dependencies, pinned by commit and SHA-256 and built with only PNG enabled. They are used by `libs/core/src/image_file.cpp`, which is file I/O and never on the render path. libpng plus zlib would add two libraries to build on three platforms for the same job. When the runtime needs to load textures (KTX2 and compressed formats, from the terrain database and glTF), that is a separate choice, made by the paging work.

## D-013 CIGI 3.3 first

**Status:** Accepted (2026-11-06)

The first host protocol is CIGI 3.3. It is the version most hosts and IGs implement, and its interface control document is freely available. CIGI 4.0 (SISO-STD-013-2014) changes packet sizes and IDs and is less widely supported. Version-specific packet layouts stay in one part of the codec, so 4.0 can be added beside 3.3 later.

## D-014 Fovea's own CIGI codec

**Status:** Accepted (2026-11-06)

The codec is Fovea's own: fixed-size encode and decode of the packets in use, with no allocation, tested byte for byte against the ICD's layouts and fuzzed nightly. The Boeing CIGI Class Library is LGPL, older C++, and allocates per packet; only a handful of packets are needed for milestones 2 and 6. UDP sockets are a thin non-blocking wrapper in one file of `libs/net`, with no networking library.

Packet layouts come from the ICD itself, never from memory: `tools/fetch-cigi-icd.sh` downloads the CIGI 3.3 ICD (Boeing document TST08I016, 3 Nov 2008) and its errata (Appendix C, 4 May 2009) into the git-ignored `docs/reference/`, checked against pinned SHA-256 hashes. The ICD calls itself "intended to be a public domain document" but asks not to be reproduced for modifying the interface, so it isn't committed. Codec code cites the ICD section and page for each packet. One ICD quirk: the 3.3 document gives Minor Version = 2 in IG Control and Start of Frame, and the errata don't change it. Fovea sends 2, as written, and accepts any minor version on receive, checking only Major Version = 3.

## D-015 Tiling: a geographic quadtree

**Status:** Accepted (2026-11-06)

Terrain tiles split latitude and longitude in half at each level, from two root tiles (east and west hemispheres). The scheme covers the whole Earth, matches how sources are indexed, and has no zone seams (UTM grids, simpler for one small area, do). Each tile holds a square grid of heights with edges shared with its neighbours and an imagery texture; its vertices are relative to the tile's own ECEF origin, so the GPU stays camera-relative. Tiles shrink in longitude toward the poles; that is a cost only far from any likely database.

## D-016 Terrain level of detail: chunked quadtree by screen-space error

**Status:** Accepted (2026-11-06)

Each frame a tile splits into its children where its geometric error (recorded by the compiler) would show as more than about a pixel. Skirts hide cracks between levels. Popping is measured on the desk; morphing between levels (CDLOD-style) follows if it shows. Clipmaps are smoother but harder to combine with paging mixed-resolution sources.

## D-017 Terrain imagery: compressed, the format chosen per database

**Status:** Accepted (2026-11-06)

Imagery is stored as GPU-compressed textures with full mip chains, in a format chosen per database: BC7 (sRGB) by default, about 1 byte per texel, so 0.6 m imagery is 3-4 MB per km² with mips; BC1 at half that, blockier up close; uncompressed RGBA8 for exact colour at four times BC7's size. The compiler takes the format as a build setting and the manifest and each tile's imagery layer record it, so the runtime reads whichever it finds and one database can mix formats by region (detail where a deployment needs it, size elsewhere). Close-range and wide-area uses are both first-class (D-011), so neither format is assumed. Encoders come from a pinned library, chosen and licence-checked in milestone 3 step 4.

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
