# Milestone 6: Environment

The goal from `fovea-plan.md`: the sky, the light and the weather, which the first milestones left as a placeholder. That means:

- a physically based sky, with haze over distant terrain
- the sun and moon placed from the date, time and place, through dawn, dusk and night, with stars
- shadows
- airport lights and lit windows at night
- visibility, fog and cloud layers
- all of it set by the host over CIGI, or in config without one

Everything after it (close-range detail, cultural features, effects) is judged under this light.

As before, each step is its own pull request. Steps 1 to 6 and 8 run in WSL and CI on lavapipe; each also gets a look at the desk, and step 9 is the desk's.

## Where things stand (checked 2026-11-08)

- **Sky.** It is a fixed gradient: the reference scene's fullscreen triangle (`shaders/reference_scene.slang`), drawn first in each scene pass, with the terrain and models over it.
- **Sun.** It is fixed at 45° elevation, azimuth 135° (`terrain.cpp`, `sun_direction()`), and pushed to the terrain and model shaders. Terrain is shaded `albedo * (0.75 + 0.25 N·L)`, because the imagery already holds its own shading. Models are shaded `colour * (0.35 + 0.65 N·L)`. There is no specular term, no emissive and no shadows.
- **Tone mapping.** ACES (Narkowicz's fit) at a fixed exposure, set from config as `reference_scene.exposure_ev`.
- **Passes.** Each view draws its scene pass multisampled, then the weighted resolve (D-028; it also resolves the depth, nearest sample), then the tone map. Everything is pushed as constants; there is no per-frame buffer.
- **Environment state.** `fovea::EnvironmentState` (`libs/core/scene.hpp`) holds only `time_of_day_h`, which nothing reads or writes.
- **CIGI.** The codec handles IG Control, Entity Control and Start of Frame, and skips everything else. The ICD's environment packets are listed under step 7.
- **Lights.** D-018 names a `lights` layer for later; none exists. The runways' FAA ends and widths are in `data/airports/*.toml` (D-022). The models' materials carry a base colour, a texture and a material ID, but no emissive.

## Decisions (accepted 2026-11-08, in `decisions.md`)

### D-029 Environment state is scene state

- **The state.** Date and time (UTC, as CIGI has it), whether time runs (the ICD's ephemeris model), visibility, and the global weather layers. They are protocol-neutral scene state in `libs/core`, written by the host adapter or by config (`[environment]`), and read by the render thread from the snapshot like the eyepoint.
- **Derived each frame.** The renderer computes the sun's and moon's directions and the lighting from that state, then writes them with the atmosphere's parameters into one per-frame uniform buffer per frame in flight. Push constants (128 bytes, already used for matrices) can't hold them.
- **Ephemeris.** The sun from the *Astronomical Almanac*'s low-precision formulas (about 0.01°); the moon from Meeus's truncated lunar theory (*Astronomical Algorithms*, ch. 47, about 10 arc-seconds; the Almanac's low-precision moon is only good to about 0.3°), corrected for the eye's parallax (up to 1° for the moon); both in double precision on the CPU, the moon's phase from the same. The directions are geometric, unrefracted: refraction near the horizon (about 0.5°) bends only the sun's and moon's discs, drawn in step 2, not the light.

### D-030 A physically based sky and aerial perspective, from precomputed tables

- **The method.** The sky and the haze over distance use Hillaire's method ("A Scalable and Production Ready Sky and Atmosphere Rendering Technique", 2020).
- **The tables.** Small lookup tables, rebuilt by compute shaders when the sun or atmosphere changes:
  - transmittance
  - multiple scattering
  - a sky-view table per view
  - an aerial-perspective volume per view, along its frustum
- **Model and distance shading.** Rayleigh and Mie scattering and ozone absorption, over a spherical Earth (WGS84 mean radius) centred under the eye, the eye's altitude in it taken from its ellipsoidal height, not from its distance to the Earth's centre (which is 7 km more at the equator and 14 km less at the poles). The terrain and model shaders apply the aerial perspective per sample, inside the multisampled pass. A pass after the resolve would read one depth per pixel and haze edges wrongly.
- **Visibility.** It sets the aerosol (Mie) density at the ground through Koschmieder's relation (extinction 3.912 / V for visibility V), so the host's visibility means what an observer means by it: a dark object fades to 2% contrast at V.
- **Why this method.** Bruneton's precomputed 4D tables hold every sun angle, so time running costs nothing, but rebuilding them when the atmosphere changes (the host's visibility, a weather layer) takes far longer than a frame, and their resolution shows at the horizon; Hillaire's tables rebuild within a frame. An analytic sky (Preetham, Hosek-Wilkie) has no aerial perspective and no twilight.
- **Cost.** About 0.2 ms a view on a desktop GPU in the paper's figures. The desk measures it.

### D-031 One exposure for all channels, from the light, not the image

- **The rule.** Exposure follows the scene's illuminance at the eye (sun, moon and sky, from the same model), smoothed over about a second, and is the same in every view. A config offset (today's `exposure_ev`) shifts it.
- **Why.** Exposure from each view's own histogram would let neighbouring channels disagree: a view looking at the sun would darken while the one beside it doesn't, and the seam between the displays would show. It would also change with what each view sees.
- **Range.** Night needs about 15 EV less exposure than noon. HDR rendering has the range; the exposure moves.

### D-032 Cascaded shadow maps, per view

- **The maps.** The sun (or the moon, when it is up and the sun isn't) casts shadows through cascaded shadow maps: 4 cascades, fitted to each view's frustum, 2048² each in 32-bit depth, filtered (PCF).
- **What casts.** Models cast onto terrain and onto each other. Terrain casts onto models and onto itself, for hills at a low sun.
- **Terrain at a high sun.** The terrain's imagery already holds the shadows of the day it was taken (NAIP's, mostly near noon). Shadowing the imagery again doubles them, so terrain receives only the part of a shadow its imagery doesn't already have: models' shadows, and terrain shadows at a low sun.
- **Why per view.** Each channel's frustum differs. Sharing one map would waste resolution in all of them.
- **Cost.** About 64 MB and four depth passes a view: 192 MB and 12 passes a frame for the desk's three channels. The desk measures it.

### D-033 Airport lights are a tile layer, drawn as points after the resolve

- **Building the layer.** The compiler builds the `lights` layer (D-018) for each airport from:
  - its runways' FAA geometry (D-022)
  - a committed light file per airport, which names each runway's systems (edge, threshold and end, centreline, touchdown zone, approach lighting such as MALSR or ALSF-2, PAPI or VASI) from the FAA's published data
  - extra fixtures with positions, if any
- **Placing the fixtures.** Fixtures are laid out by the FAA's standards (AC 150/5340-30: spacing, colours and directions), in code, not by hand.
- **Drawing them.** Each light's point is drawn after the resolve, single-sampled, as D-028 (accepted) says, GPU-driven: the layer's lights culled and drawn indirectly. It is at least a pixel across, with its intensity falling off by inverse square and through the atmosphere's transmittance, and its colour and beam direction from its type, so a runway's lights look different from each end.
- **Clustered lighting.** It is for the light airport lights shed on surfaces (apron floodlights, entity landing lights later), in the scene pass. `fovea-plan.md` had the airport lights drawn with it; since D-028 their points are drawn after the resolve, and clustered lighting keeps the many-lights role foundational decision 4 gives it for the light they cast. This milestone reserves it, since no light here lights the ground yet.
- **Lit windows.** Models' emissive materials (glTF's `emissiveFactor`, `emissiveTexture` and `KHR_materials_emissive_strength`), read by `libs/models` and set in the model scripts.

### D-034 Clouds are 2D layers first; precipitation is visibility for now

- **Heights.** The ICD gives layers' base elevations above mean sea level; the database is ellipsoidal (D-020), so they are converted with the database's geoid height (issue #19), as the host's altitudes are.
- **Cloud layers.** Each global cloud layer (the ICD's layers 1 to 3) is a textured, lit layer at its base elevation: procedural noise shaped by its coverage, scattered by the sun through the same atmosphere, and shading the ground below it in the shadow pass.
- **Fog layers.** Ground fog (layer 0) is a band of extra density in the atmosphere, set by its visibility.
- **Precipitation.** Rain, snow, sleet and hail are layers too (4 to 7), each with its own visibility range, which is all they change for now. The streaks themselves are particles, milestone 10's.
- **Later.** Volumetric clouds, regional weather (Environmental Region Control) and wet or snow-covered ground, once the frame shows room.

### D-014, amended

The environment adds three packets: Celestial Sphere Control, Atmosphere Control and Weather Control (global layers). Their layouts come from the ICD (step 7). Environmental Region Control, and Weather Control's regional layers and weather entities, are accepted and ignored for now. That is the same as other packets not yet supported, with a count in the stats.

## Step 1: Environment state, the per-frame buffer and the sun

- **State and config.** `EnvironmentState` takes date and time (UTC) as an epoch and the host time it was set at, whether time runs, and visibility. Config takes `[environment]`: `date`, `time`, `time_runs`, `visibility_m`. When time runs, each frame's time is the epoch plus the host time elapsed since it was set, so every channel (and later every machine) agrees, and a packet that leaves the time alone leaves it alone.
- **Ephemeris.** The sun's and moon's directions in ECEF, from the location of each view's eye (D-029).
- **Buffer and shaders.** The per-frame uniform buffer replaces the pushed sun. The terrain and model shaders take the sun from it, with its colour and the ambient as they are now.
- **Done when:**
  - The sun's elevation and azimuth at KHSV on several dates and times (the solstices and equinoxes, sunrise and sunset) match NOAA's solar calculator's geometric (unrefracted) values within 0.05°.
  - The moon's topocentric position matches the US Naval Observatory's within 0.05°, near the horizon included, and its illuminated fraction within 1%.
  - Golden images change only where the sun moved.
  - Drawing still allocates nothing.

### Step 1 results (2026-11-08)

- **Against JPL Horizons.** Checked at KHSV at 16 times from 2000 to 2040 (sunrise and sunset, the solstices, the 2024 eclipse, new and full moons). Horizons stands in for NOAA's calculator and the USNO: it is one source for both bodies, geometric (airless), from JPL's ephemerides.
  - The sun is within 0.006°.
  - The moon is within 0.003° seen from the ground, against 1.0° without the parallax.
  - The moon's lit fraction is within 0.8%. Horizons' is seen from KHSV and ours from the Earth's centre: the parallax shifts the phase angle by up to a degree.
- **The lighting follows the time.** A white box's top at KHSV on the June solstice reads 231 at local noon, 217 at 8 am, and 186 at night, the same at two times of night.
- **Only the sun above the horizon lights.** At 1 am, with the sun 25° under the horizon in the north, the box's north wall is as dark as its top (186; 229 without the horizon's cut-off).
- **The buffer.** Each view's lighting is a uniform block in a per-frame buffer (set 1 of the terrain and model pipelines, at the view's dynamic offset), so the push constants drop to 68 bytes.
- **Goldens.** `terrain_hills` and `models_on_hills` were regenerated: the sun moved from the fixed 45°/135° to the default date and time's (2026-03-20, 15:30 UTC), visible only on the boxes' faces and the hills' slopes. The reference scene's goldens are unchanged.

## Step 2: The sky

- **The sky.** The transmittance, multiple-scattering and sky-view tables (D-030) replace the gradient, with the sun's disc in it.
- **The light.** The sun's colour reaching the ground comes from the transmittance, and the ambient from the sky.
- **Exposure.** It follows the light (D-031). The reference scene's grid stays below the horizon for its tests.
- **Done when:**
  - A GPU test finds the sky blue overhead and paler at the horizon at noon, and reddened toward the sun at sunset.
  - The sun's disc is where step 1 puts it, within a pixel.
  - The tables against a reference computation on the CPU (a ray-marched integral) agree within 1% in transmittance and 3% in sky radiance.
  - Exposure is the same in every view of a frame.
  - Goldens regenerated and looked at.

### Step 2 results (2026-11-08)

- **The tables against the CPU.** A CPU integral of the same model (`tests/support/atmosphere_reference.cpp`) uses 2,000 steps per ray for transmittance and 576 directions of 60 steps for multiple scattering. Against it, on lavapipe and NVIDIA alike, the GPU's transmittance table is within 0.86%, and its sky-view table within 0.4 to 0.8% (with the sun 74° and 14° up).
- **A bug the reference found.** At exactly ground level, `r^2 (mu^2 - 1) + R^2` cancels to nothing in floats, so rays from the ground missed it. The GPU's multiple scattering came out 35% high near the ground, the sky 16% too blue at the horizon, and the reference passed some directions through the Earth. Both now take the discriminant as `(r mu)^2 - (r - R)(r + R)` and test the horizon by its angle. This test takes 1.7 s in Release, so it is on CI's slow list.
- **Colours.** At noon the zenith is 3.7 times bluer than red, and the horizon 1.3. At sunset the sky toward the sun is 45 times redder than blue near the horizon, while overhead stays blue (2.4).
- **The sun's disc** lands within 0.07 px of where the view's matrix puts the sun.
- **Exposure.** Every view takes the same exposure. Light on level ground is 9,300 at noon, 4,000 at 8 am, and none at night: exposure rises as the light falls, until night's floor.
- **Pre-exposure.** Exposure is applied where light is shaded (sky, ground, terrain, models), so the HDR target and the multisampling resolve (D-028) hold display-range values whatever the light. Its key takes a level surface's light to pi, so a white one shows as 1 before tone mapping. Its floor is a ten-thousandth of the sun's illuminance; step 4 tunes it for night. Config's `exposure_ev` still shifts the tone map.
- **Lighting.** Terrain is lit by the sun through the air, mostly as level ground (the imagery keeps its own shading), plus the sky. Models are Lambertian under both. At 10:30 local the terrain is as bright as before: a mean of 211 against 206 (out of 255) over the middle display's ground. Models' shaded sides, lit by the sky alone, are darker and bluer than the old fixed ambient.
- **On the desk.**
  - The sky's passes (the tables once; each frame, three views' sky-view tables, light and the exposure) cost about 0.07 ms a frame. They are counted in output 0's GPU time: 1.49 ms over the airport route's first minute, against 1.43 before.
  - Every run held 60 Hz, its missed frames in the first 0.12 s (#41), with no validation messages.
  - The sky matches across the seam between the two portrait displays.
- **Goldens.** All five were regenerated and looked at. The reference scene has a blue sky; the terrain is unchanged; the boxes' shaded sides take the sky's blue. NVIDIA differs from them on at most 0.41% of pixels by more than 4.

## Step 3: Aerial perspective and visibility

- **The haze.** Terrain and models fade into the sky with distance through the aerial-perspective volume. Visibility sets the haze (D-030).
- **Done when:**
  - A GPU test of a dark box against the sky at distances around the set visibility finds its contrast near 2% at V (within a factor of 1.5, the tone curve bending it).
  - Doubling visibility moves that distance with it.
  - At 100 km visibility, a terrain golden still shows the far hills.
  - Where terrain levels of detail meet in the distance, the seam softens (looked at on the desk).

### Step 3 results (2026-11-08)

- **Koschmieder's relation holds.** Along a level ray from sea level, the air's transmittance falls to 2% at 20.3 km with visibility set to 20 km, and at 42.2 km with it set to 40 km, on lavapipe and NVIDIA alike. That is read from the aerial-perspective volume (32 x 32 froxels a view, 64 slices spaced as their index squared, out to 300 km): 8-bit images can't resolve a 2% contrast.
- **What is drawn fades.** From 300 m up, ground about 5 km away stands 87 levels apart from the sky above the horizon at 200 km visibility, and 6 (9 on NVIDIA) at 500 m.
- **Visibility sets the aerosols.** Koschmieder's extinction, 3.912 / V in green light at sea level, less Rayleigh's, gives the Mie density (`with_visibility`). A change rewrites the atmosphere's uniform buffer in the frame's own commands (a frame still in flight reads the old values) and remakes the tables. The transmittance table now takes 160 steps: hazy air made the old 40 off by 6.5% on grazing rays, against the CPU integral. The sky-view table goes back to 32 steps: step 2 had raised it to 96 while chasing what turned out to be the ground test, and 32 meets the 3% the plan asks for. The CPU check of the tables now uses the default 50 km visibility: transmittance within 0.39%, the sky within 1.3%.
- **Visibility changes under 1% are let go**, so a host easing it a little every frame doesn't remake the tables (most of a millisecond) every frame. A visibility that isn't a positive number is ignored. The volumes, 64 slices a view, are stacked in one 3D image: a GPU at Vulkan's minimum 3D size (256) takes 4 views, and more is refused at start-up with the reason.
- **Terrain's haze is looked up per vertex.** In the terrain's fragment shader, any image of the frame set makes lavapipe (Mesa 25.2.8, LLVM 20) crash compiling it, though its limits are a million images a stage. Terrain triangles are a few pixels across, so the vertex shader looks the haze up and interpolates it. Models and the reference ground look it up per pixel, inside the multisampled pass.
- **The default 50 km is a hazy day.** The sky is paler than step 2's, which had Hillaire's aerosols, about 220 km. A deployment sets `visibility_m`.
- **A test made tolerant.** A view raised 8 m by its offset and one placed 10 m up now differ by up to a level on NVIDIA: the two eyes, reached by different arithmetic, differ in their last bits, and the haze is computed from each view's eye. `ReferenceScene.EyeOffsetMovesTheEye` allows that level.
- **On the desk.**
  - The volumes and the per-pixel lookups cost 0.02 to 0.06 ms an output over the routes' first minute (output 0: 1.52 ms on the airport route, 1.94 on KHUA, against 1.49 and 1.88 at step 2).
  - Every run held 60 Hz, with no validation messages.
  - The ridge east of KHUA turns blue-grey with distance, and the far terrain melts into the sky, the same across all three displays.
  - **The owner's look** at the KHUA route at noon, at dusk (7:45 pm, the sun low in the west) and in 10 km haze found two faults, both fixed here.
    - Dusk glared, everything bleached. Exposure had followed the light on level ground (step 2), which falls toward nothing as the sun sets while the air toward the sun stays bright. It now follows the light on a surface facing the sun, plus the sky's (D-031's illuminance at the eye): noon barely changes, and dusk is warm and golden, the horizon orange toward the sun.
    - A thin white band at the horizon, in every run, was the reference scene's horizon line, drawn at eye level above the terrain's dipped horizon. With terrain loaded, the grid and the line are no longer drawn. Below the horizon, where no terrain covers it, the sky-view table now holds the ground, lit by the sun through the air (the CPU check has the same term).
    - The owner found the result better. The owner hasn't noticed seams between terrain's levels of detail in the distance, without looking for them closely; a closer look is left for the hardware check (step 9). A test that grid lines continue across the seam between two displays now looks where the lines are resolved: nearer the horizon they are thinner than a pixel, and the new exposure left one edge column seeing a line the other just missed.
- **Goldens.** All five were regenerated and looked at: the sky hazier, the reference ground and the hills' far edge fading. NVIDIA differs from them on at most 0.34% of pixels by more than 4.

## Step 4: Night

- **The night sky.** The sky through twilight into night, the moon with its phase (and moonlight, through the same atmosphere), and stars from the Yale Bright Star Catalogue (public domain; about 9,000 stars to magnitude 6.5, drawn as points after the resolve, their brightness by magnitude).
- **Exposure.** It follows into night (D-031).
- **Done when:**
  - GPU tests at noon, sunset, civil twilight and midnight find the sky's brightness falling in that order, the stars visible only after twilight, and the moon's lit side toward the sun.
  - A run through dusk at the desk shows no jump.

### Step 4 results (2026-11-08)

- **Night falls.** Over KHSV on the June solstice, the sky's light on level ground is 1,256 at noon, 74 at sunset, 0.51 at the end of civil twilight and 1.25e-5 (the airglow) at midnight with the moon down. In a view 50 degrees up, no stars show at noon or at sunset, and 132 at midnight.
- **The moon.** It is lit on the side toward the sun: a waxing half moon 31 degrees up, its disc's light leans toward the sun across the sky (alignment 1.00, 14 px off its middle in a one-degree view). Its brightness follows Allen's magnitude law, 2.1e-6 of the sun's six degrees from full. It lights the sky through its own layer of the sky-view table (a second layer a view, its azimuth from the moon's), and the land and models through its own irradiance, and adds to the haze. The disc is Lambertian (albedo 0.18, calibrated to the full moon's illuminance), seen through the air.
- **Stars.** The Yale Bright Star Catalogue, 5th revised edition (9,096 stars; `tools/fetch-stars.py` makes the table from the Harvard copy, its SHA-256 pinned, and the table is committed). Positions are J2000, turned to the date by precession (Meeus 21.3) and the Earth's rotation: Polaris stays within 0.75 degrees of the latitude through a day, and Sirius crosses the meridian due south at its declination. They are drawn after the resolve as single-pixel points (D-028), by magnitude and through the air, where the resolved depth is still the sky's, so terrain hides them, and below the horizon not at all. A day's exposure loses them without being told to.
- **Airglow.** A moonless sky has a faint glow of its own (about 2e-4 lux on level ground), so night isn't black.
- **Exposure at night.** Exposure adapts fully down to a twentieth of the sun's illuminance (an overcast day, a low sun). Below that, it rises three quarters of a stop for each stop the light falls: deep dusk shows about 2 stops darker than full adaptation, a moonlit night about 4, a moonless one about 6. This replaces step 2's floor (a ten-thousandth, then a square root). On the desk, the first try washed dusk out and left a moonless midnight black.
- **On the desk.** The owner looked at deep dusk (the sun 3 degrees down), a half-moon night and a moonless midnight, from KHUA, turning in place once round (`khua-spin.toml`, new, for looking all round the sky). They looked alright, and nothing odd stood out. Deep dusk is a lavender pink all round, the moon's glow passes in the south-west, and a moonless midnight is a dark grey sky over faint land, with the stars sharp single pixels.
  - Cost over the KHUA route's first minute: output 0 1.99 ms (1.94 at step 3), the others 0.01 to 0.02 ms more. That is the moonlit layer and the moon in the haze, and the stars.
  - Every run held 60 Hz, with no validation messages.

## Step 5: Shadows

- **The maps.** Cascaded shadow maps (D-032) from the sun or moon.
- **Done when:**
  - A GPU test of a box on flat ground finds its shadow where the sun's direction puts it (its edge within two texels of the cascade) and of the right length at 30° and 60° elevation.
  - No shadow acne on lit flat ground.
  - A hill at a low sun shades the ground behind it.
  - The desk's GPU time is measured with and without (step 9).

### Step 5 results (2026-11-08)

- **The maps.** Four cascades a view, 2048 square, 32-bit depth, out to 6 km; their splits three-quarters logarithmic. Each slice is bounded by a sphere, so its texel doesn't change as the view turns, seen from the light along an orthographic box reaching 3 km beyond it toward the light for casters outside the view. The box's centre is snapped to whole texels in the world: a fixed point moves across its map only by whole texels as the eye moves (unit-tested, with every slice's corners and its casters inside its map). The light is the sun while any of it is up, else the moon.
- **Casters.** Models always (cut-outs through a masked fragment shader). The terrain only with the light under 30 degrees; higher, its imagery holds its shadows (D-032). The terrain casts the tiles the view picked, so hills outside the view don't cast into it (a later step can widen that).
- **Placement and length.** A post 20 m tall, seen from above, casts its shadow tip 34.83 m away where the geometry puts it at 35.21 m (the sun 30 degrees up), and 11.91 m against 12.15 m (60 degrees). That is within two texels and a pixel (0.64 m).
- **No acne.** The sunlit ground beside the shadow is even to 3 levels at 30 degrees. That needed a receiver offset toward the light, scaled by how much it grazes the surface. Along the light's own ray, that offset doesn't move a shadow's edge. A slope-scaled depth bias in the maps was the first try: it pushed the post's sides (which a low sun grazes) back, and shortened its shadow by 3 m.
- **Terrain casting.** A hill 60 m high, the sun 10 degrees up, shades the ground 130 m behind it to 41 against 70 before it.
- **lavapipe again.** In the terrain's fragment shader, lavapipe (Mesa 25.2.8, LLVM 20) crashes compiling a Load, a SampleLevel or a SampleCmp of an image, though not a Sample, and any image of the frame set (step 3). So the maps are also bound in the terrain's own set, read with Sample and a nearest sampler, and compared in the shader (3 x 3). The models use the same lookup, so shadows look alike on both.
- **On the desk.**
  - Shadows cost 0.25 to 0.6 ms an output over the routes' first minute. Output 0 is 1.95 ms on the airport route (1.52 at step 3) and 2.57 on KHUA (1.99 at step 4).
  - GPU memory rose 225 MiB (12 maps of 16 MiB). CPU work rose 0.3 ms a frame (the fitting, and culling models for each cascade).
  - Every run held 60 Hz, with no validation messages.
  - Mid-morning, the tower casts its shadow north-west across the apron.
  - **The owner's look** (the airport route at 8 am, the sun 25 degrees up, and at 10 am) found the shadows good, with two faults, both fixed here.
    - At a low sun the terrain casts, and each cascade drew all the view's tiles: 5.5 ms on output 0, and 868 frames of 3,900 missed. Each cascade now draws only the tiles that reach its box, in its own region of records: 3.8 ms on output 0 (2.7 and 3.0 on the others, about 9.5 of the frame's 16.7 in all), and 60 Hz held. Terrain casting is still the heaviest thing on the screen; coarser casters would cut it, if step 9 finds the budget short.
    - Far off, the tower's thin shadow flickered as the view moved, until it was near enough to be wide. A distant pixel covers tens of metres of ground looking along it, and one lookup a pixel sampled a shadow a few metres wide. The lookup is now bilinear, and its taps spread over the pixel's footprint in the map (from the screen derivatives of the point, up to eight texels apart). The owner found it better, for 0.08 ms an output.
- **Goldens.** `models_on_hills` was regenerated and looked at: the boxes cast their shadows away from the sun. NVIDIA matches it as before.
- **From the review.**
  - The maps are made with the terrain, not without it (225 MiB that nothing else reads).
  - Every view's maps are made readable once, before the first frame binds them. A view's layers were otherwise still undefined while another view drew (validation reports it with three views; a test now draws three).
  - Without a light casting (night with no moon, or a moon all but new), no maps are drawn.
  - Casters beyond a box toward the light are clamped onto it (depth clamp), not clipped.
  - The receiver's offset toward the light is capped at four texels, so shadows don't start detached from their casters.
  - Models dropped from a cascade for want of room are counted.

## Step 6: Weather layers and fog

- **The layers.** Global cloud layers (D-034): base, thickness, coverage and type. Ground fog. Precipitation layers as reduced visibility within them.
- **Setting them.** Config sets them (`[[environment.layer]]`) until step 7 brings the host.
- **Done when:**
  - A GPU test above and below a full overcast finds the sun hidden and the ground lit only by the sky below it, and the layer seen from above.
  - Scattered coverage lets about that fraction of sky through.
  - A fog layer hides terrain within it and not above it.

### Step 6 results (2026-11-08)

- **The state.** `EnvironmentState` carries up to six `WeatherLayer`s: cloud, fog, rain or snow, with base and thickness above the ellipsoid, a cloud's coverage and a band's visibility. Config's `[[environment.layer]]` gives heights above sea level, made ellipsoidal with `[host]`'s geoid height, as the host's altitudes are. The renderer takes them each frame (`set_weather`).
- **Fog, rain and snow** are bands of extinction (3.912 over the visibility), integrated exactly along each ray, per pixel, between two spheres (`weather.slang`). They aren't put into the atmosphere's tables, where a band 60 m thick would fall between their rows. Fog's air scatters the light on level ground at the eye. Terrain, models, the sky and its discs, and the stars are all seen through them.
- **Rain and snow fall from an overcast,** though no cloud is drawn: below a band's top the ground and models have only the overcast's light, no sun, moon or shadows (an eye under it draws no shadow maps), the atmosphere's haze turns grey, fog lit by the overcast too, and the band's air scatters that light. Looking up from under it, the sky is the overcast's base: no sun, moon or stars. Rain's is a dark blue-grey fifth of the light on level ground (thick nimbostratus), snow's nearer half and whiter. On the desk both held 60 Hz with no validation messages; rain at 3 km visibility shows the ground fading into grey, snow at 1 km from 300 to 500 m up is nearly all grey, as it should be.
- **What precipitation waits for,** so it reads as weather and not only haze (the owner's call, 2026-11-09: do what step 6 can, and keep to this):
  - **Falling rain and snow:** streaks and flakes around the eye, from the weather layers: particles, milestone 10.
  - **A cloud ceiling above it:** with the clouds, issue #73.
  - **Wet and snow-covered ground:** milestone 14, with the material maps.
- **Cloud layers are not drawn yet.** Config and the state take them, ready for the host's Weather Control packets (step 7), but the renderer leaves them out. Three approaches were built and looked at on the desk, and none landed well enough to keep (the owner's call), so they wait for issue #73, kept on the branch `wip/clouds`:
  - **A 2D layer at the cloud base,** shaped first by value-noise fBm, then by Perlin-Worley noise eroded by Worley detail (baked once on the GPU, thresholded by its quantiles so coverage c covers about c), shaded by thickness and by the cloud toward the sun. About 0.5 ms an output. Even so, it read as a flat sheet.
  - **A volumetric layer:** 48 steps a ray through the layer, 4 toward the sun, varied tops over shared bases. It looked better, but cost 1.5 to 2 ms more an output and missed about 3% of frames on the desk under broken cloud. It wants half-resolution marching and better lighting of its tops.
  - **A skybox painted from the same noise.** Not what the owner meant (a photographed sky); dropped. Photographed CC0 skies (Poly Haven) are an option noted on #73.
- **Found on the way, kept.** The sky pass clamps its output within half floats: the sky plus the sun's disc could pass 65,504 and store an infinity on NVIDIA.
- **Tests.** Config parsing and its errors. On the GPU, fog 60 m deep hides a post's foot as it does the ground beside it, while 100 m up the post matches clear air. Under rain a post's shadow is gone and the ground is darker than in the sun; under snow it is lit brighter than under rain. Looking at the sun from under thin rain, no disc shows.
- **Done when,** revised: the fog test stands; the overcast and coverage tests go with the clouds to #73.

## Step 7: The host sets the environment

- **The packets.** The CIGI codec and adapter take three packets, each from the ICD and tested byte for byte, then fuzzed:
  - Celestial Sphere Control: ICD 4.1.9, p. 82, packet 9, 16 bytes
  - Atmosphere Control: 4.1.10, p. 85, packet 10, 32 bytes
  - Weather Control: 4.1.12, p. 100, packet 12, 56 bytes, global layers
- **Other packets.** Region packets (Environmental Region Control, 4.1.11, p. 88, packet 11) and regional layers are counted and ignored (D-014 amended).
- **The test host.** It gains script segments that set time, visibility and layers, and a script that flies a dawn approach into KHSV through a cloud layer.
- **Done when:**
  - Codec round trips against the ICD's layouts.
  - An end-to-end test (test host to headless IG) changes the time and visibility and finds the image change.
  - Config sets the environment until the host sends a packet, and each packet then replaces what it carries: Celestial Sphere Control's date and time only when its Date/Time Valid flag is set, Atmosphere Control's every field, Weather Control's layer as a whole (Weather Enable off removes it). The ICD has no flag for one field alone.

### Step 7 results (2026-11-09)

- **The packets,** in `libs/net` (`cigi.hpp`), each written from the ICD's figure and tested byte for byte in both byte orders, every bit field set apart from its neighbours: Celestial Sphere Control (4.1.9, Figure 53, p. 82), Atmosphere Control (4.1.10, Figure 54, p. 85) and Weather Control (4.1.12, Figure 65, p. 101). The errata change none of them. Bit positions were read off the figures' label positions in the PDF. Values the ICD leaves undefined reject the packet: Hour over 23, Minute over 59, Weather Control's Scope 3 and Severity 6 or 7. The fuzz target decodes and round-trips all three (60 s, 14.8 million inputs, nothing found), and the corpus gains a message holding them.
- **The adapter** writes each into the scene's environment, which config set until then:
  - **Celestial Sphere Control** sets the date and time only with Date/Time Valid, and whether the clock runs (Ephemeris Model Enable) always. Without Date/Time Valid the time carries on from where it was, and Hour and Minute aren't checked. A time within the minute the clock is already in leaves it running, so a host may send its time every frame without holding the clock to the minute. A date that doesn't exist, or falls outside 1950 to 2050 (the years the sky is computed for), rejects the packet. Sun, moon and star enables and star intensity aren't taken.
  - **Atmosphere Control** sets the global visibility; none (0) or a negative one is rejected, as config rejects them. Humidity, temperature, wind and pressure change nothing drawn yet.
  - **Weather Control** sets a global layer as a whole, or removes it with Weather Enable off. Layer IDs 0 to 5 (fog, three clouds, rain, snow) are taken; their base above sea level is made ellipsoidal with the geoid's height. Sleet, hail, sand, dust and the IG's own layers, regional layers and weather entities are counted (`weather_ignored`) and ignored; Environmental Region Control is skipped by size, counted as other unhandled packets are (D-014 amended).
- **Reset/Standby** puts config's environment back, as ICD 2.7.3 (p. 13) asks of environmental and weather conditions.
- **Layer identity.** `WeatherLayer` carries an id, numbered as hosts number them: fog 0, clouds 1 to 3, rain 4, snow 5. Config numbers its layers by kind the same way, so a host's packet replaces config's layer of that kind. Config now allows one fog, rain and snow layer each and up to three cloud layers.
- **The test host.** A segment can set the date and time, whether the clock runs, the visibility and weather layers (`[[segment.layer]]`), sent once in that segment's first message (`script.hpp`). `khsv-dawn.toml` flies the KHSV approach at dawn (11:45 UTC on 20 March, the clock running) through ground fog that lifts over the threshold. It goes through fog, not the planned cloud layer, since clouds aren't drawn yet (#73).
- **Tests.** Codec bytes, byte orders, undefined values and reserved bits. The adapter: the time set only when valid, impossible dates refused, the visibility, layers replaced, added and removed, and ignored packets counted. Config's layer ids and limits. The test host's environment segments. End to end on the GPU: the test host's session to the adapter over loopback, to a headless frame loop; the host's visibility changes the image by 89 levels on average, and its time by 41.

## Step 8: Airport lights and lit windows

- **The lights.** Each airport's light systems (from the FAA's NASR data, below), the compiler's `lights` layer (D-033), and the lights drawn after the resolve.
- **Lit windows.** Emissive materials in `libs/models` and the landmark scripts (lit windows on the terminal and the tower).
- **Done when:**
  - A compiler test lays out a runway's edge lights at the standard's spacing, and its threshold and end lights at its ends, green outward and red inward.
  - A GPU test at night finds each light at least a pixel, on its point within a pixel, dimmer with distance and haze, and its colour from the side the standard gives.
  - Emissive materials glow at night and don't by day.
  - At the desk: a night approach into KHSV (looked at).

### Step 8 results, lit windows (2026-11-09)

The step is split: lit windows are done; the airport lights wait on the light files (the owner's check of the FAA data), and the step is parked while the simulator integration milestone goes first.

- **Emission.** `libs/models` reads glTF's emissive factor, `KHR_materials_emissive_strength` and emissive texture (sRGB, the first texture coordinates), as luminance in cd/m² (glTF leaves its units open). The model shader adds it, the same by day and night, before the air and the weather bands, so fog and haze dim it. By day the exposure (D-031) hides it.
- **The landmarks.** `airport_landmarks.py` gives the KHSV terminal, terminal hall, concourse and tower base lit windows (30 cd/m², warm; most panes lit, some dark, from a random stream of their own so the panes' tints are unchanged), and the tower's cab a faint glow (3 cd/m²; cabs are kept dim at night). Only those five models changed.
- **The tone map keeps hue.** On a moonless night the exposure is about 2,000, so any lit window is far past the curve's top, and the ACES fit applied to each channel clipped them to white. It is now applied to a colour's brightest channel, the others scaled with it: windows stay warm (255, 231, 188), the sun's disc is a warm white (255, 242, 223), mid-tones a little paler. The golden images changed by up to 36 levels (looked at).
- **Tests.** Emission read, with and without its own texture, and a negative strength refused. On the GPU: a box with 40 cd/m² walls at midnight shows (255, 231, 188) beside a plain box's 1; at noon the two differ by a level.
- **At the desk:** a night flyover held 60 Hz (1 missed frame, 0 validation messages). The owner's look at the warm windows is still to do.
- **Noticed, not yet taken up:** the dawn run (step 7) showed a strong pink cast before sunrise and a hard line where the fog's top meets the horizon.

### Step 8, the airports' light systems (2026-11-09)

The light systems come from the FAA's NASR airport data (cycle 2026-10-29) rather than being drafted by hand for the owner to check: its runway and runway-end records code each system. `data/airports/khsv.toml` and `khua.toml` carry them as NASR codes them, and `tools/check-airports.py` checks every field of both files (positions to a centimetre, elevations, widths, lengths, lights) against a NASR cycle, so a new cycle's changes show up.

| Runway end | Approach | VGSI | Other |
|---|---|---|---|
| KHSV 18R | ALSF-2 | none | centreline, touchdown zone |
| KHSV 36L | MALSR | none | centreline, touchdown zone |
| KHSV 18L | MALSR | PAPI, 4 boxes, right | glide path 3°, TCH 73 ft |
| KHSV 36R | MALSR | PAPI, 4 boxes, right | glide path 3°, TCH 72 ft, threshold displaced 200 ft |
| KHUA 17 | MALSR | PAPI, 4 boxes, left | glide path 3°, TCH 46 ft |
| KHUA 35 | none | PAPI, 4 boxes, left | glide path 3°, TCH 50 ft |

All three runways have high-intensity edge lights (HIRL). The second source agrees:

- **KHSV:** the instrument approach plates (d-TPP cycle 2611) give ALSF-2 on 18R and MALSR on the others, HIRL on both runways, and TDZL and RCLS on 18R and 36L.
- **KHUA:** its plates carry no text to check against, so the check is NASR's lighting remarks, which name MALSR on 17, PAPI on 17 and 35, and HIRL.

No REILs are listed at either airport. The layouts themselves (spacing, colours, the approach systems' bars) come from the FAA's standard (AC 150/5340-30) in code, next.

### Step 8, the layouts (2026-11-10)

`apps/fovea-compiler/lights.cpp` lays each runway's lights out in its own frame from AC 150/5340-30J (*Design and Installation Details for Airport Visual Aids*, 2018), which `tools/fetch-faa-ac.sh` puts in `docs/reference/` (git-ignored, hash-checked), citing its paragraphs and figures as the CIGI code cites the ICD. Each light carries the colour it shows each way along the runway.

- **Edge lights** (2.3.1): 10 ft outside the pavement (the AC's choice for jets), uniform and mirrored, at most 200 ft apart, spaced from the threshold lights' lines. Yellow over the last 2,000 ft (or half the runway) seen toward the far end of an instrument end, which the end's NASR markings code gives (all six are PIR, added to the files and the check). Three threshold lights a side on visual ends, four on instrument ends.
- **Threshold and end lights** (2.3.2, Figure A-3): four a side, 10 ft before the end, green outward and red inward. At a displaced threshold (Figure A-5): red at the runway's end, red edge lights toward the approach before it, and the threshold lights at the threshold, outward from the edge line, green.
- **Centreline** (3.3.1, Figure A-34): every 50 ft from 75 ft in; seen toward either end, alternating red and white over its last 3,000 ft and red over the last 1,000.
- **Touchdown zone** (3.3.2, Figure A-35): 30 bars of three a side from 100 ft past the threshold, 36 to 46 ft from the centreline, seen only from the approach. White: the AC names the fixture (L-850B), whose colour is in AC 150/5345-46, not fetched.
- **PAPI** (7.4.4, 7.5.4): on the side named, 50 ft from the edge, units 30 ft apart, aimed per Table 7-2 or 7-3, sited at TCH x cot(the lowest on-course signal) from the threshold. The AC disagrees with itself on that signal (7.5.4.3.3 says 10' below the glide path, Table 7-2 aims the unit 15' below); the siting paragraph's is used, and a test checks its worked example (909.26 ft). An ILS runway's PAPI is properly sited at the glide slope's touchdown point (7.5.4.2), which NASR doesn't give.
- **Not laid out:** the approach lights (ALSF-2, MALSR), whose layouts are in FAA JO 6850.2, not this AC; they are listed as skipped. No REILs are listed.
- **Tests:** each system on made-up runways (spacing, positions, colours each way, the caution zone, a displaced threshold with centreline and touchdown zone lights, short runways), and both airport files laid out whole. Deliberate mistakes in the layout code (spacing, offsets, colours, the PAPI's side and angle, the displaced threshold's blanking and offsets) each fail them.
- **Review fixes:** the displaced area's edge lights are yellow the other way on any runway, not only in an instrument end's caution zone; visual ends get three threshold lights a side whatever the edge lights; the PAPI is sited 10' below the glide path, not 15'; overlapping displaced thresholds are refused.

### Step 8, the lights layer (2026-11-10)

The compiler bakes the layouts into the database's `lights` layer (D-018, D-033), with a `[lights]` section in the compile settings (`data/compile/khsv.toml` has one).

- **Format** (`tile_file.hpp`): 36 bytes a light, giving its latitude, longitude and ellipsoidal height, the runway's true heading there, its type, its colour each way along that heading, and a PAPI unit's angle. The reader refuses a value out of range, a light dark both ways, a PAPI angle on another type, and bytes left over. The manifest gains `[layers.lights] level`.
- **Where:** the root tiles, as for placements (#48). Each light stands on the finest level's ground as built (the smoothed runway on the pavement), sampled as placements are (`finest_heights.hpp`). A fixture's own height (centimetres) isn't added. The heading is taken at each light, in its own north, so it differs from the runway's midpoint heading by about 0.01° a kilometre as the meridians converge.
- **Report:** the compiler prints how many lights went into how many tiles, and each system it didn't lay out.
- **Tests:**
  - The layer round-trips and refuses every truncation and bad value.
  - A runway's points and headings back on the Earth.
  - A synthetic compile with a north-east runway across both root tiles: every root tile's lights add up to the layout. Each light sits on the finest ground to a micrometre and faces along the runway, checked against the axis in its own north and east. The edge lights are 10 ft outside the pavement, and the PAPI is on the left, its highest unit nearest the runway. A placement shares a tile with lights and both layers read back. A runway whose lights reach past the database is refused.
  - Breaking the heading (the midpoint's, or the axes swapped) or the height fails them.
- **Review fixes:** a missing finest tile under a light gets its own error, not "outside the database". `[lights]` refuses unknown keys, a non-table and a non-integer level.

Next: drawing the points after the resolve (D-028), then the night approach at the desk.

### Step 8, drawing the lights (2026-11-10)

`libs/render/src/vk/lights.cpp` and `shaders/lights.slang` draw the database's lights layer after the resolve, beside the stars (D-028, D-033).

- **Loading:** the lights come from the root tiles with the placements (#48), into one storage buffer of offsets from their mean point, placed relative to the eye in double on the CPU.
- **Each light, in its vertex shader:**
  - **Colour:** the layer's colour for the side the eye is on: colour[0] when the eye is behind the light along its runway's heading. A PAPI unit is white above its angle and red below, blended over 3′.
  - **Brightness:** its candela toward the eye over the distance squared, through the aerial perspective and the fog, rain and snow bands, into its pixels' radiance (the stars' units, cos³ off-axis so it matches across a seam).
  - **Beam:** about 15° (edge and threshold lights) or 10° either side of the runway's line, centred 4° up; edge lights keep 5% all round for circling.
  - **Size:** at least a pixel, or as many as a 0.4 m fixture covers when near (a disc, its light shared among its pixels), with `largePoints` where the GPU has it.
- **Approximate photometry:** the AC gives positions and colours, not intensities, which are in AC 150/5345-46 (not used yet). The candela (2,500 edge and threshold, 1,000 in-pavement, 6,000 PAPI) and the colour filters' transmission are approximate, for a middle step at night.
- **Depth:** tested against the resolved depth from a point moved toward the eye, so it stays on its pixel. The resolved depth is a pixel's nearest sample, and at a grazing angle a pixel of ground reaches far nearer than the light: from 6 m up, a pixel at 450 m spans 380 to 550 m. A fixed margin hid lights 450 m off in the first test. The margin is now 1 m plus 2% of the distance plus the pixel's reach over the ground (two pixel widths over the sine of the angle the eye looks down at it), and never more than half the distance, so a hill between still hides a light.
- **The stars' point pipeline** and pixel solid angle are shared with the lights (`make_point_pipeline`, `pixel_solid_angle` in `stars.hpp`).
- **Test** (`TerrainRender.AirportLightsShowAtNightWhereAndAsTheyShould`, on lavapipe with validation), at midnight on flat ground:
  - each of six lights within a pixel of its point;
  - a threshold light green from the approach and red from beyond;
  - a PAPI unit white from 4.2° and red from 0.8°;
  - a light 30 m off covering at least 9 pixels, far ones 1 or 2;
  - in fog, each light dimmer the farther it is, and the farthest (3.5 km) lost.
  The night's exposure saturates clear-air lights to white in 8 bits, so the inverse square shows only through the fog. Swapping the sides, the old fixed depth margin, inverting the PAPI, leaving out the fog, or keeping every light one pixel each fails it.
- **Not yet:** the desk look at a night approach into KHSV, which needs the database rebuilt with the lights layer.

## Step 9: Hardware check

- **On the desk.** GPU time and memory with each part on and off (sky and tables, aerial perspective, shadows, clouds, lights) on the KHSV and KHUA routes, at noon, at dusk and at night.
- **The owner's look.** A dawn approach and a night approach, the seams between the channels at sunrise, and shadows on the landmarks.
- Results recorded; anything unexpected becomes an issue.

## Inputs needed from you

| Needed by | Input |
|---|---|
| Plan | ~~D-029 to D-034, and D-014's amendment~~ accepted |
| Step 8 | ~~The airports' light systems, checked by you~~ Taken from the FAA's NASR data and checked against a second source instead (below); a look at a night approach once they're drawn |
| Steps 2 to 9 | Time at the desk: the sky, dusk, night, shadows and the lights |
