# Fovea

Fovea is an open-source, cross-platform Vulkan image generator (IG): it draws real-time geographic scenes for a host program that drives it. Flight simulation over CIGI 3.3 is its first use, not its definition. Protocols live in adapters, and the renderer knows nothing of packets.

It has two halves:

- **`fovea-compiler`** builds a tiled, multi-resolution terrain database from public data: USGS 3DEP elevation, USDA NAIP imagery, the FAA's runways and lights, and NOAA's geoid. It runs offline, on Linux.
- **`fovea`** streams that database and draws it at a locked 60 Hz across one or more display channels. It runs on Windows and Linux.

## Status

Fovea is in active development. The first database covers Huntsville International (KHSV) and Moontown (KHUA), Alabama.

**Working now:**
- **Displays:** three borderless channels with off-axis frustums from config, at a 60 Hz frame clock.
- **Terrain:** streamed with levels of detail, with smoothed runways and placed glTF landmarks.
- **Image quality:** MSAA.
- **Sky and light:** physically based sky and aerial perspective, sun, moon and stars at any date and time, shadows, and lit windows at night.
- **Weather:** visibility and ground fog.
- **Host control:** a host drives the eyepoint, time and weather over CIGI, and asks for terrain heights (HAT/HOT).

**Next:**
- **Airport lights:** they are laid out in the database and are being drawn next.
- **Later:** cloud layers (#73), entities and the rest, as the [milestones](docs/fovea-plan.md#milestones) order them.

Each milestone's record is in `docs/milestone-*.md`. Open work is in the GitHub issues.

## Building

Fovea needs CMake 3.24 or later, Ninja, a C++20 compiler (GCC 11+, Clang 14+ or Visual Studio 2022) and a Vulkan 1.3 driver. Every other dependency is fetched by CMake, pinned by version and hash. GDAL is the exception: it is needed only for the compiler, and only on Linux.

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
```

- **Linux:** the build also needs `libx11-dev libxext-dev libxrandr-dev` for SDL. The compiler is built only when GDAL 3.8 or later is installed (`libgdal-dev`).
- **Windows:** configure from a Visual Studio developer prompt with `CC=cl CXX=cl`, using the same presets.

The presets are `debug`, `release`, `relwithdebinfo`, `asan`, `asan-fast`, `tsan` and `fuzz` (the CIGI codec's fuzz target). Warnings are errors on every compiler.

## Building a database

Databases are never committed; you build them from the source lists in `data/`.

```sh
python3 tools/fetch-sources.py data/sources/khsv.toml                    # about 17 GB into data/cache
./build/release/apps/fovea-compiler/fovea-compiler data/compile/khsv.toml  # into data/db/khsv
python3 tools/check-elevation.py data/compile/khsv.toml                  # optional checks
python3 tools/check-imagery.py data/compile/khsv.toml
```

The settings format is documented in `apps/fovea-compiler/settings.hpp`.

## Running

```sh
./build/release/apps/fovea/fovea --list-monitors                          # monitors, in config index order
./build/release/apps/fovea/fovea --overlay config/khsv.toml               # the KHSV database, with a timing overlay
./build/release/tools/cigi-test-host/cigi-test-host tools/cigi-test-host/scripts/khsv-approach.toml
./build/release/apps/fovea/fovea --headless --frames 600 --dump out config/khsv.toml  # offscreen, a PNG per channel
```

A headless run ignores `[host]`: it stands the views at `[reference_scene]`'s origin and writes the last of its frames, so give it enough frames (`--frames`) for the finer tiles to page in.

A config file describes the display channels, the environment, the database and the host link. `config/channels.example.toml` documents its keys.

`cigi-test-host` is a scripted CIGI host for trying Fovea without a simulator. Its scripts are in `tools/cigi-test-host/scripts/`.

## Driving Fovea from a host

See the [integration guide](docs/integration-guide.md): the CIGI packets Fovea handles, the config a host needs, the height datum, and HAT/HOT.

## Documentation

- **[`docs/fovea-plan.md`](docs/fovea-plan.md):** the design and the milestones.
- **[`docs/architecture.md`](docs/architecture.md):** how the code is laid out, and the rules between libraries.
- **[`docs/decisions.md`](docs/decisions.md):** design decisions (D-xxx) and the alternatives considered.
- **[`docs/integration-guide.md`](docs/integration-guide.md):** connecting a host.
- **[`ATTRIBUTION.md`](ATTRIBUTION.md):** the data sources and the credit each asks for.

## Data and license

Fovea uses public data and generic models only. The data stays its providers'. Credit them as `ATTRIBUTION.md` lists wherever a database built from it is shown or shared.

The code's license hasn't been chosen yet (#23).
