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

## Step 4: Night

- **The night sky.** The sky through twilight into night, the moon with its phase (and moonlight, through the same atmosphere), and stars from the Yale Bright Star Catalogue (public domain; about 9,000 stars to magnitude 6.5, drawn as points after the resolve, their brightness by magnitude).
- **Exposure.** It follows into night (D-031).
- **Done when:**
  - GPU tests at noon, sunset, civil twilight and midnight find the sky's brightness falling in that order, the stars visible only after twilight, and the moon's lit side toward the sun.
  - A run through dusk at the desk shows no jump.

## Step 5: Shadows

- **The maps.** Cascaded shadow maps (D-032) from the sun or moon.
- **Done when:**
  - A GPU test of a box on flat ground finds its shadow where the sun's direction puts it (its edge within two texels of the cascade) and of the right length at 30° and 60° elevation.
  - No shadow acne on lit flat ground.
  - A hill at a low sun shades the ground behind it.
  - The desk's GPU time is measured with and without (step 9).

## Step 6: Weather layers and fog

- **The layers.** Global cloud layers (D-034): base, thickness, coverage and type. Ground fog. Precipitation layers as reduced visibility within them.
- **Setting them.** Config sets them (`[[environment.layer]]`) until step 7 brings the host.
- **Done when:**
  - A GPU test above and below a full overcast finds the sun hidden and the ground lit only by the sky below it, and the layer seen from above.
  - Scattered coverage lets about that fraction of sky through.
  - A fog layer hides terrain within it and not above it.

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

## Step 8: Airport lights and lit windows

- **The lights.** The light files for KHSV and KHUA (from the FAA's published data, checked with the owner), the compiler's `lights` layer (D-033), and the lights drawn after the resolve.
- **Lit windows.** Emissive materials in `libs/models` and the landmark scripts (lit windows on the terminal and the tower).
- **Done when:**
  - A compiler test lays out a runway's edge lights at the standard's spacing, and its threshold and end lights at its ends, green outward and red inward.
  - A GPU test at night finds each light at least a pixel, on its point within a pixel, dimmer with distance and haze, and its colour from the side the standard gives.
  - Emissive materials glow at night and don't by day.
  - At the desk: a night approach into KHSV (looked at).

## Step 9: Hardware check

- **On the desk.** GPU time and memory with each part on and off (sky and tables, aerial perspective, shadows, clouds, lights) on the KHSV and KHUA routes, at noon, at dusk and at night.
- **The owner's look.** A dawn approach and a night approach, the seams between the channels at sunrise, and shadows on the landmarks.
- Results recorded; anything unexpected becomes an issue.

## Inputs needed from you

| Needed by | Input |
|---|---|
| Plan | ~~D-029 to D-034, and D-014's amendment~~ accepted |
| Step 8 | The airports' light systems: I'll draft them from the FAA's published data for you to check |
| Steps 2 to 9 | Time at the desk: the sky, dusk, night, shadows and the lights |
