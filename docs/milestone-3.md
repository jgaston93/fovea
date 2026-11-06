# Milestone 3: First terrain

The goal from `fovea-plan.md`: the compiler builds a small KHSV database (3DEP elevation, NAIP imagery, levels of detail), and the runtime streams it at a locked 60 Hz. Real ground under the CIGI eyepoint of milestone 2, replacing the reference grid.

As before, each step is its own pull request. Steps 1 to 4 run in WSL and CI; steps 5 to 7 need the Windows machine for the display.

## The data (checked 2026-11-06)

For a first area of about 20 x 20 km around KHSV (34.55-34.72 N, 86.90-86.65 W):

- **Elevation, 1 m:** USGS 3DEP lidar DEMs cover it, from several projects (2019 North Alabama, 2020 17-County, 2023-24 11-County). GeoTIFFs in 10 x 10 km tiles, NAD83 / UTM zone 16N, heights NAVD88 (orthometric). Listed by the TNM Access API (`tnmaccess.nationalmap.gov/api/v1/products`, dataset "Digital Elevation Model (DEM) 1 meter"); files on the public `prd-tnm` store. About 38 tiles across the projects in the box, 8-321 MB each.
- **Elevation, backdrop:** 3DEP 1/3 arc-second (about 10 m), the 1 x 1 degree tile `n35w087` (about 470 MB).
- **Imagery:** USDA NAIP, from the Microsoft Planetary Computer STAC (collection `naip`, free, no account): **2023 at 0.6 m** (newest), 2021 at 0.3 m, 2019 at 0.6 m; 20 quarter-quad COGs per year over the box, RGB + near-infrared, NAD83 / UTM zone 16N.
- **Vertical datum:** NAVD88 heights become ellipsoid heights with NOAA's GEOID18 (h = H + N), published as grids by NGS and available to PROJ.
- **Volume:** about 1.6 GB of 1 m elevation and 4.4 GB of 0.6 m imagery uncompressed for 20 x 20 km; the compiled database is smaller (compressed textures).
- All US Government works (public domain), with credits requested; `ATTRIBUTION.md` lists each.

## Decisions (accepted 2026-11-06, in `decisions.md`)

- **D-015 Tiling: a geographic quadtree.** Tiles split latitude and longitude in half at each level from two root tiles (east and west hemispheres), so the scheme covers the whole Earth and matches how sources are indexed. Each tile holds a square grid of heights (for example 257 x 257, edges shared with neighbours) and an imagery texture, with vertices relative to the tile's own ECEF origin (camera-relative on the GPU, as now). The alternative, UTM grids, is simpler for one small area but has seams between zones.
- **D-016 Level of detail: chunked quadtree by screen-space error.** Each frame, a tile splits into its four children when its geometric error would show as more than about a pixel on screen. Cracks between levels are hidden with skirts. Popping as tiles change level is measured on the desk; if it shows, morphing between levels follows (CDLOD-style). Clipmaps are the alternative: smooth, but harder to mix with the paging of mixed-resolution sources.
- **D-017 Imagery: compressed GPU textures with full mip chains, the format chosen per database.** BC7 (sRGB, about 1 byte per texel, 3-4 MB per km² of 0.6 m imagery with mips) by default; BC1 (half the size, blockier up close) and uncompressed RGBA8 as options. The compiler takes it as a build setting, the manifest and each tile's layer header record it, and the runtime reads whichever it finds, so one database can even mix them by region. Encoders from a pinned library (chosen in step 4, licence checked).
- **D-018 Database: a manifest and one file per tile.** `manifest.toml` names the schema version, the area, the layers and the sources; each tile is one binary file with a small header (magic, schema version, tile key) and a table of named layers (elevation, imagery; later materials, placements, lights), so new layers don't need a rebuild of the rest (foundational decision 2). The simulation industry's own standard, OGC CDB, was considered: it covers far more (models, features, many levels) but assumes a large toolchain; a CDB importer or exporter can come later (#33).
- **D-019 The compiler runs on Linux (WSL, CI).** GDAL is the compiler's only big dependency: on Ubuntu 24.04 it is one package (`libgdal-dev`, GDAL 3.8.4); on Windows it means building GDAL and PROJ through vcpkg or a conda environment. Databases are plain files that any runtime reads, so a Windows compiler isn't needed to fly on Windows; it can follow if wanted.
- **D-020 Heights in the database are ellipsoidal (WGS-84),** converted from NAVD88 by the compiler with GEOID18. The database also carries the geoid height over its area, so the runtime converts host sea-level altitudes properly there (issue #19).

## Step 1: Sources and fetching

- `data/sources/khsv.toml`: the area, and for each layer its source, product, year and priority. `tools/fetch-sources.py` lists the files through the TNM API and the Planetary Computer STAC, downloads them into the git-ignored `data/`, and checks sizes and hashes into a lock file that is committed. `ATTRIBUTION.md` lists each source and its credit.
- **Done when:** a fresh clone fetches the KHSV sources with one command, and a second run downloads nothing.
- **Result:** `python3 tools/fetch-sources.py data/sources/khsv.toml` (standard library only) fetched 49 files, about 15 GB, in 10 minutes: 27 1 m DEM tiles from four 3DEP projects (5.7 GB), the newest 1/3 arc-second tile (n35w087, 2026 edition), 20 NAIP 2023 quarter-quads (8.2 GB; four-band, so larger than the 4.4 GB estimate), and GEOID18 (`us_noaa_g2018u0.tif`, 16.7 MB). `data/sources/khsv.lock.json` (committed) pins each file's URL, size and SHA-256. A second run downloads nothing; `--verify` re-hashes everything (a byte changed in one file was found and re-downloaded); `--update` asks the services again, and a file whose hash changed upstream is an error until accepted. The services' occasional HTTP 500s are retried. `ATTRIBUTION.md` credits each source.

## Step 2: The database format and its reader

- `libs/terrain`: tile keys and the quadtree (geodetic bounds, neighbours, parents), the tile file format (D-018) with a writer and a reader, and a `TerrainQuery` over a database: height at a point (bilinear in the finest loaded tile) and ray intersection.
- **Done when:** tests round-trip tiles through files, reject damaged ones, and answer height and ray queries on synthetic databases exactly; no GDAL, no Vulkan.
- **Result:** `fovea/terrain/tile_key.hpp` (the quadtree: bounds, the tile at a point, parent, children, neighbours across the antimeridian), `tile_file.hpp` (the tile file: a 32-byte header, a 48-byte entry per named layer with its CRC-32, 8-byte-aligned data; the elevation layer as a float32 grid of ellipsoid heights, south-west first, edges shared; atomic writes), and `database.hpp` (`manifest.toml`, `tiles/<level>/<x>/<y>.fvt`, and `DatabaseTerrain`: height from the finest tile that reads cleanly, falling back to coarser ones and listing the problems; rays marched in half-height steps and bisected to a millimetre). Tests on synthetic databases written by the same code: a tilted plane comes back to 0.2 mm (float32) from either level, a damaged fine tile falls back to the coarse one and is reported, rays onto flat terrain match the exact ellipsoid intersection to a centimetre, and a ridge blocks the line of sight. A planted bug in the bilinear weights, the level order or the checksum check each fails a test.

## Step 3: The compiler: elevation

- `apps/fovea-compiler` (GDAL): mosaics the 1 m projects (newest first) over the 10 m backdrop, converts NAVD88 to ellipsoid heights (GEOID18), resamples into each tile's grid at each level, and records each tile's geometric error for D-016. Writes the database.
- **Done when:** compiled heights match the source DEM (through GDAL, independently) within a stated tolerance at sampled points, including across tile edges; KHSV's field elevation comes out where published.

## Step 4: The compiler: imagery

- NAIP mosaicked, reprojected into each tile's texture, colour-balanced across the quarter-quads only if seams show, mipmapped and BC7-encoded.
- **Done when:** tiles decode back to the source imagery within a stated error (PSNR), with no seams at tile edges.

## Step 5: Drawing terrain

- The renderer draws loaded tiles: meshes generated from each tile's height grid on the GPU, textured, camera-relative, with skirts; tiles chosen each frame by screen-space error (D-016). All tiles of the small area loaded up front for this step. The reference grid stays available for tests.
- **Done when:** golden images of fixed views over KHSV match in CI (lavapipe), and on the desk the terrain lines up with known landmarks (the runways) seen from the CIGI eyepoint.

## Step 6: Paging

- A paging thread loads tiles from disk and uploads them on a transfer queue, from requests the render thread pushes (predicted from the view's motion); the render thread draws what's resident and never waits (architecture.md). Memory budgets, eviction, and per-frame stats (tiles resident, uploads, misses).
- **Done when:** a flight over the whole area (the test host) keeps 60 Hz with no missed frames, and no frame shows a missing tile where a coarser one could be drawn.

## Step 7: Hardware check

- Fly the scripted routes over KHSV on the desk: frame timing, paging stats, popping between levels, and the look from low altitude. Results recorded; anything unexpected becomes an issue.

## Inputs needed from you

| Needed by | Input |
|---|---|
| Step 1 | ~~Approve D-015 to D-020; the area and imagery~~ accepted: 20 x 20 km, NAIP 2023 at 0.6 m (0.3 m around the airport may follow) |
| Step 1 | About 10 GB of disk in WSL for sources and the database |
| Step 7 | An hour at the desk |
