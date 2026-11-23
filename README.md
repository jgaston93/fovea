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
- **Airport lights:** runway edge, threshold and end, centreline and touchdown zone lights and PAPIs, laid out from the FAA's data and standard (AC 150/5340-30J) and drawn as points with their colours by side and through the air and fog.
- **Weather:** visibility and ground fog.
- **Host control:** a host drives the eyepoint, time and weather over CIGI, and asks for terrain heights (HAT/HOT).
- **Particle volumes:** a simulation's dust, smoke or spray, received over UDP as a stream of anisotropic Gaussian clusters, or puffs (D-039, D-053) and drawn in every view as lit, soft volumes that meet the ground without a seam (D-040) shadow themselves (D-041), and break up into swirling billows (D-043), with ragged edges if asked (D-051); or drawn as one volume, far cheaper with many (D-052), its light scattered many times inside it (D-054).

**Next:**
- **Airport lights:** approach lighting systems (MALSR, ALSF-2), whose layouts are in FAA JO 6850.2, and the fixtures' photometry from AC 150/5345-46 in place of today's approximate intensities.
- **Later:** cloud layers (#73), entities and the rest, as the [milestones](docs/fovea-plan.md#milestones) order them.

Each milestone's record is in `docs/milestone-*.md`. Open work is in the GitHub issues.

## Building

Fovea needs CMake 3.24 or later, Ninja, a C++20 compiler (GCC 11+, Clang 14+ or Visual Studio 2022) and a Vulkan 1.3 driver. Every other dependency is fetched by CMake, pinned by version and hash. GDAL is the exception: it is needed only for the compiler, and only on Linux.

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
```

- **Linux:** the build also needs `libx11-dev libxext-dev libxrandr-dev` for SDL. The compiler is built only when GDAL 3.8 or later is installed (`libgdal-dev`).
- **Windows:** configure from a 64-bit Visual Studio developer prompt (`vcvars64.bat`, or `Launch-VsDevShell.ps1 -Arch amd64`; the default developer shell targets x86, which CMake refuses) with `CC=cl CXX=cl`, using the same presets.

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
./build/release/apps/fovea/fovea --check config/<file>.toml               # read and check a config, nothing opened (D-056)
./build/release/apps/fovea/fovea --overlay config/khsv.toml               # the KHSV database, with a timing overlay
./build/release/tools/cigi-test-host/cigi-test-host tools/cigi-test-host/scripts/khsv-approach.toml
./build/release/apps/fovea/fovea --headless --frames 600 --dump out config/khsv.toml  # offscreen, a PNG per channel
./build/release/apps/fovea/fovea --headless --seconds 30 --dump out config/cigi-test-host.toml  # driven by a host, offscreen (D-044)
```

A headless run without `[host]` stands the views at `[reference_scene]`'s origin and writes the last of its frames, so give it enough frames (`--frames`) for the finer tiles to page in. With `[host]` (or the offscreen presenter with `[host]`, without `--headless`), the host drives it (D-044): the same loop as on screen (CIGI, Start of Frame, HAT/HOT, particles, `--stats`) with no windows, for `--seconds` or until SIGINT or SIGTERM, then each channel's last frame is written with `--dump`: what the IG drew for the host's last pose, for a host's own tests.

A config file describes the display channels, the environment, the database and the host link. `config/channels.example.toml` documents its keys.

`cigi-test-host` is a scripted CIGI host for trying Fovea without a simulator. Its scripts are in `tools/cigi-test-host/scripts/`.

## Driving Fovea from a host

See the [integration guide](docs/integration-guide.md): the CIGI packets Fovea handles, the config a host needs, the height datum, HAT/HOT, and particle volumes.

## Documentation

- **[`docs/fovea-plan.md`](docs/fovea-plan.md):** the design and the milestones.
- **[`docs/architecture.md`](docs/architecture.md):** how the code is laid out, and the rules between libraries.
- **[`docs/decisions.md`](docs/decisions.md):** design decisions (D-xxx) and the alternatives considered.
- **[`docs/integration-guide.md`](docs/integration-guide.md):** connecting a host.
- **[`ATTRIBUTION.md`](ATTRIBUTION.md):** the data sources and the credit each asks for.

## Data and license

Fovea uses public data and generic models only. The data stays its providers'. Credit them as `ATTRIBUTION.md` lists wherever a database built from it is shown or shared.

Fovea's code is licensed under the [Apache License 2.0](LICENSE) (D-004). The libraries it builds with keep their own licenses, all compatible, listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
