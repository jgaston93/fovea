# Milestone 4: Airport and models

The goal from `fovea-plan.md`: flattened runways, a glTF model library, and hand-placed KHSV landmarks. Models stand on the milestone 3 terrain, loaded on the paging thread and drawn at 60 Hz; milestone 6's entities will use the same model library.

As before, each step is its own pull request. Steps 1 and 2 run in WSL and CI; steps 3 to 5 also need the Windows machine for the desk, and step 4 its Blender.

## The data (checked 2026-11-07)

- **Runways:** the FAA's surveyed runway ends (latitude, longitude, elevation, NAVD88) are already in `tools/check-elevation.py`, from the FAA's airport data as AirNav publishes it; the widths (150 ft each, as published) are to be taken from the same data in step 1. US Government work, public domain; the FAA joins `ATTRIBUTION.md` and the database's credits.
- **Landmark positions:** read off the NAIP imagery already in the database (public domain), not taken from OpenStreetMap: OSM's licence (ODbL) asks share-alike of derived databases, and its servers couldn't be reached to check its coverage of the airport's buildings. OSM can follow for taxiways, aprons and footprints if wanted, with its notice in `ATTRIBUTION.md`.
- **Models:** none suitable is public and free of restrictions, so Fovea makes its own, generic ones (D-025). Blender 5.2 is on the Windows machine.

## Decisions (in `decisions.md`: D-023 to D-025 accepted 2026-11-07; D-022 and D-026 proposed)

- **D-022 Runways are smoothed in the compiler, from the elevation itself.** The compiler takes each runway's rectangle from a committed airport file (FAA ends and widths) and, inside it, replaces the finest level's heights with a smooth surface fitted to the 1 m lidar: smooth along the runway, keeping its grade changes, and across it, keeping its crown (a 1 to 1.5% cross slope puts the edges 0.2 to 0.35 m below the centreline), blended into the surrounding ground over the shoulders. Coarser levels are filtered from it as now, so their recorded errors stay true. This removes the lidar's noise and anything on the runway when it was flown, without moving the surface off the survey; a straight line between the surveyed ends, or a level cross-section, would not. Taxiways and aprons follow later.
- **D-023 Models are glTF 2.0 binary (`.glb`), read by `libs/models` with cgltf.** A CPU-only library like `libs/terrain` (no Vulkan), so its parsing is tested without a GPU and a host could use it too. cgltf (MIT) is one file with no dependencies, and writes glTF too (for tests). fastgltf is faster, but needs simdjson, and parsing is a small part of loading a model, done once per model on the paging thread.
- **D-024 Model textures are decoded and uploaded uncompressed, for now.** The paging thread decodes a model's PNG or JPEG textures (stb_image, now built with JPEG too: D-012 left texture formats to this choice) and builds their mips; the GPU holds RGBA8. The model files stay plain glTF, straight from Blender, and a few dozen landmarks take little of the memory budget. Compressed textures (BC7 offline, as for terrain, or `KHR_texture_basisu`) are revisited in milestone 5, when vegetation and many models make memory and uploads matter.
- **D-025 Models are Fovea's own and generic, built by committed scripts.** Each model is a Blender Python script in `data/models/` that builds it and exports a `.glb`; the scripts and the small `.glb` files are committed (own work, under the repository's licence). Landmarks are massing models: the right footprint, height and colour, not detail. Each glTF material names its Fovea material in `extras` (the plan: material IDs from day one), checked when it loads.
- **D-026 Placements are a tile layer.** A committed placement file per database lists each landmark (model, latitude, longitude, true heading). The compiler checks each model is in `data/models/` and bakes them into a `placements` layer (D-018) of the tile containing each, at a level the settings choose and the manifest records, with the lowest terrain height under the model's footprint (models carry a plinth below their ground line, so the uphill side isn't buried and the downhill side doesn't float). Placements are drawn while their tile is resident, not only while it is selected: a tile stops being drawn once it splits, which near the airport is always. For KHSV the level is 12, the pinned roots, so placements are always there; finer levels page them with their tiles when an area has more.

## Step 1: Runways in the compiler

- `data/airports/khsv.toml` (committed): each runway's ends (FAA latitude, longitude, elevation) and width, with their source; `tools/check-elevation.py` reads its runway ends from it rather than its own copy. The compiler smooths the finest level's heights inside each runway and blends the shoulders (D-022); coarser levels follow by the existing filter. The FAA is credited in the manifest and `ATTRIBUTION.md`.
- **Done when:** `tools/check-elevation.py` fits its own smooth surface to the lidar on each runway (independently, in numpy) and the database is within 10 cm of it on the runway, nowhere bumpier than a stated change of slope; the surveyed end elevations still match within 0.30 m; the shoulders meet the ground with no step; the comparison with the sources leaves out the runways and shoulders, and every coarse tile is still within its recorded error.

## Step 2: The model library

- `libs/models` (a new library: `models ← core`, and `render ← models`, in `architecture.md`): reads a `.glb` (cgltf): meshes (positions, normals, texture coordinates, indices), materials (base colour factor and texture, the Fovea material ID), the textures (PNG or JPEG) decoded with their mips. The compiler doesn't link it: it only checks a placement's model file exists. Allocates as it likes: it runs on the paging thread.
- **Done when:** tests round-trip models written with cgltf (geometry, materials, textures), reject damaged and unsupported ones (with a reason), and require a material ID.

## Step 3: Drawing models

- The renderer draws models: their meshes in shared vertex and index buffers, textures in a bindless array, each placement an instance placed relative to the eye from double precision (as tiles are), lit like the terrain. The paging thread loads a model the first time a placement needs it; the render thread draws only loaded ones and never waits.
- **Done when:** golden images of written models on written terrain match in CI (lavapipe), a model stands on the terrain where its placement puts it (within a pixel of where the view's matrix, in double, puts it), and drawing allocates nothing.

## Step 4: KHSV landmarks

- Blender scripts for the KHSV landmarks (the control tower, the terminal, hangars and a few large buildings), placed from the imagery in `data/placements/khsv.toml`; the compiler bakes them into the database (D-026).
- **Done when:** on the desk each landmark stands on its footprint in the imagery, at the right height, from the CIGI eyepoint, and the owner finds it looks right.

## Step 5: Hardware check

- Fly the KHSV routes on the desk with the landmarks: frame timing, model loading, and the look of the airport from low altitude. Results recorded; anything unexpected becomes an issue.

## Inputs needed from you

| Needed by | Input |
|---|---|
| Plan | ~~D-023 to D-025~~ accepted: cgltf, uncompressed model textures for now, own generic massing models |
| Plan | Approve D-022 (runways smoothed from the lidar, not flattened to the FAA's ends) and D-026 (placements as a tile layer, in the roots for KHSV) |
| Step 4 | Which landmarks matter most to you (the tower, the terminal, the hangars, anything else) |
| Steps 4, 5 | Time at the desk |
