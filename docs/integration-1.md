# Integration 1: Fovea under a real simulator

The goal: plug Fovea into the team's simulator over CIGI 3.3. The simulator is EYAS, with a glue layer (Aerie) between it and the IG that handles controls and data output. The team's review of the code found two gaps:

- **No HAT/HOT.** The simulator asks the IG for the height above terrain (HAT) and the height of terrain (HOT), and nothing answers.
- **The height datum.** CIGI altitudes are above mean sea level (MSL), and Fovea turns them into ellipsoidal heights with a single constant.

## What the simulator sends (from the team, 2026-11-09)

- **Geoid.** Aerie uses no geoid model. It uses one configured constant, `ellipsoid_minus_msl_m = h_ellipsoid − h_msl`, which is 0 m in its sample config. For the KHSV deployment it is −28.25 m, documented as GEOID18 at KHSV. That is the same value as Fovea's `config/khsv.toml` (`geoid_height_m = -28.25`), so the two agree on MSL exactly, everywhere. Since D-037 moved the database to WGS-84, 1.30 m below NAD83(2011) at KHSV, both must be −29.55 m (step 1). A geoid grid in Fovea would make them disagree away from KHSV, so it isn't wanted for this simulator (#19 stays open for hosts that use a geoid model).
- **HAT/HOT.**
  - One HAT/HOT Request (ID 24, 32 bytes) per Aerie frame, asking for the Extended Response (Request Type 2).
  - Each request has its own 16-bit ID, and the point is the ownship's current latitude, longitude and MSL altitude.
  - Aerie's frame is 10 ms by default, so about 100 requests a second, or under two per 60 Hz IG frame. One more request is sent at startup.
  - Update Period is 0 (one-shot), and heights are of the terrain only.
- **Network.** Messages are in network byte order (big-endian). The ports are configurable, and default to the usual CIGI pair: the ICD names none, so these are the 8004 (IG) and 8005 (host) Fovea's configs and test-host scripts already use.

This milestone goes ahead of the rest of milestone 6. Step 8's airport lights wait on the light files, and step 9 waits on this milestone. The plan's milestone 9 had "terrain queries answered over CIGI"; HAT/HOT moves here, while line of sight and the other entity work stay in milestone 9.

As before, each step is its own pull request, written from the ICD and its errata, with each packet's section and page cited.

## Where things stand (checked 2026-11-09)

- **Altitudes.** `CigiAdapter` adds `[host] geoid_height_m`, one constant with a default of 0, to every CIGI altitude: entity positions, and weather layer bases since milestone 6 step 7 (`cigi_adapter.hpp`). `config/khsv.toml` sets it to −28.25 m, GEOID18 at KHSV, which is Aerie's own constant (−29.55 m since step 1).
- **The database.** The compiler turns 3DEP's NAVD88 heights into ellipsoidal heights with NOAA's GEOID18 (D-020). D-020 says the database also carries the geoid over its area, but it doesn't yet: the manifest has no geoid layer, and only the compiler reads GEOID18.
- **Horizontal datum.** The compiler lays tiles out in NAD83(2011), and GEOID18 is relative to the NAD83 ellipsoid. The runtime treats both as WGS-84. They differ by about a metre at KHSV (#37).
- **Terrain queries.** `libs/terrain` already answers `height_at(lat, lon)` (ellipsoidal, from the finest tile that reads cleanly), ray intersection and line of sight, on the CPU with no Vulkan (`terrain_query.hpp`). Nothing calls them over CIGI.
- **CIGI.** The codec handles IG Control, Entity Control, Start of Frame, and the environment packets (Celestial Sphere, Atmosphere, Weather Control). The IG sends Start of Frame each frame and nothing else.

## Decisions (in `decisions.md`)

### D-035 MSL is the host's: one constant, shared (accepted)

A CIGI altitude is converted to and from ellipsoidal height with `[host] geoid_height_m`, the same constant the host uses (Aerie's `ellipsoid_minus_msl_m`). This applies to entity positions, weather layer bases, and HAT/HOT altitudes and heights. Fovea and the host then agree on MSL everywhere, which matters more than either agreeing with the true geoid. A geoid grid (#19) waits for a host that uses a geoid model; the database would then carry that model's grid, as D-020 intended.

### D-036 HAT/HOT are answered off the render thread, from the database's finest data (accepted)

- **Where.** A query thread holds its own `TerrainDatabase` reader. The network thread passes requests to it and gets answers back, all without locks, and sends them in the next IG message. The render thread is never involved, keeping it free of disk I/O and allocation.
- **The surface.** Answers come from the finest tile that reads cleanly, which is the surface the renderer draws close up. Far away, the renderer draws coarser levels, so a far point's answer can differ from what it shows. That is the conventional choice: the host wants the true ground, not what a level of detail shows.
- **Terrain only, for now,** as the team asked: models (buildings, hangars) aren't included.

### D-037 The database is WGS-84 (accepted)

The compiler converts positions and ellipsoidal heights from NAD83(2011) to WGS-84 (realization G2139), at an epoch written in the manifest, using PROJ through GDAL. The runtime and hosts are WGS-84 already. Closes #37.

## Step 1: NAD83(2011) to WGS-84 in the compiler (D-037)

- **Done when:**
  - Tests compare against NGS's HTDP or NCAT outputs at known points, to a centimetre.
  - The manifest records the frame and the epoch.
  - `check-elevation.py` compares against WGS-84 reference points (the FAA's survey converted the same way) and still passes.
  - The KHSV database is recompiled, and the golden terrain images are unchanged or explained.
  - Closes #37.

### Step 1 results (#77, 2026-11-09)

- **The shift.** PROJ 9.4 takes NAD83(2011) to ITRF2014 (NGS's time-dependent Helmert) to WGS 84 (G2139), stated to 1 cm; the ballpark transformation is refused. At KHSV in 2026.9 a point moves about 0.75 m north and 0.91 m west, and heights drop 1.30 m. The compiler takes it on a 0.01° grid and interpolates.
- **Tests.** NCAT doesn't do ITRF or WGS-84, and HTDP wasn't run, so the tests check against PROJ's `cs2cs` at known points, to 1 mm, and check that the epoch matters (by more than 0.2 m over the test's span). The manifest's `[frame]` (`WGS 84 (G2139)`, epoch) is written and checked; the runtime warns on a database without one, and the check tools refuse a frame they don't know.
- **KHSV.** Recompiled at epoch 2026.9 (3 minutes). `check-elevation.py` passes: the sources to 7 mm RMS, the runways within 2 cm of the fit, the FAA's survey points within 2 cm, all converted the same way. With its conversion turned off, every comparison fails by 1.30 m. `check-imagery.py` passes at 42 to 48 dB.
- **Golden images** are unchanged: test databases are compiled without a shift.
- **MSL.** The 1.30 m drop broke D-035's agreement with Aerie, so `config/khsv.toml`'s constant is now −29.55 m above WGS-84. GEOID18 converted to WGS-84 ranges from −29.49 to −29.85 m over the runway ends. Aerie's constant must change with it.

## Step 2: The HAT/HOT packets, byte order and ports

- **The packets.** In `libs/net`, from the ICD's figures:
  - HAT/HOT Request (4.1.24, Figure 86, p. 152, ID 24, 32 bytes);
  - HAT/HOT Response (4.2.2, Figure 110, p. 217, ID 102, 16 bytes);
  - HAT/HOT Extended Response (4.2.3, Figure 112, p. 220, ID 103, 40 bytes).
  - The errata are checked for each.
- **Byte order.** The codec writes either order, and `[host] byte_order` (`"network"`, the default, or `"native"`) chooses which the IG sends. Fovea already reads either, by the Byte Swap Magic Number (ICD 2.5, p. 7). The test host gets the same option.
- **Ports.** `[host] port` defaults to 8004, and the reply goes to `host_port`, default 8005, at `host_address`, or at the address the host sends from without it. All are still configurable.
- **Done when:**
  - Tests check bytes in both byte orders. Reserved bits are sent as zero and ignored on receive (ICD 2.11, rule 4: a later version may use them), and undefined values (Request Type 3) are rejected.
  - The fuzz target and corpus cover all three packets.
  - Start of Frame and the responses are byte-checked big-endian, and Fovea answers a big-endian host in big-endian.

### Step 2 results (2026-11-09)

- **The packets** are in `cigi.hpp`, written from Figures 86, 110 and 112, which the errata leave unchanged. Bit positions were read off the field boxes in the PDF. Tests check each one byte for byte in both orders, every bit field set apart from its neighbours, with reserved bits zero on sending and ignored on receipt. Request Type 3 is refused.
- **Byte order.** Every encoder takes a `swap`, so either order can be written. The adapter and the test host send in network order unless told `byte_order = "native"`. A test checks that a big-endian host's IG Control gets a big-endian Start of Frame back, and another that Start of Frame in network order is Figure 109's bytes.
- **Ports.** `[host] port` defaults to 8004 and `host_port` to 8005, and `host_address` no longer needs `host_port` with it. Without an address, answers go to `host_port` at the address the host sends from, or, with `host_port = 0`, to the port it sends from too (as before this step). The test host's `[host] port` defaults to 8005 to match.
- **Fuzzing.** The target decodes and round-trips all three packets in both orders, and the corpus gains a message of each kind. 60 s and 7.2 million inputs found nothing.
- Nothing answers requests yet: that is step 3.

## Step 3: Answering them (D-036)

- **Requests:**
  - **One-shot** (Update Period 0): answered once.
  - **Periodic** (Update Period n): answered every nth frame until the period is set to 0, carrying the Host Frame Number LSN.
  - **Geodetic points:** latitude, longitude, and MSL altitude through the shared constant (D-035). This is what Aerie sends.
  - **Entity points:** X, Y and Z offsets in the entity's body frame, from its pose in scene state.
- **Responses:**
  - **HAT** is the altitude less the terrain height; **HOT** is the terrain less the geoid (MSL).
  - **Extended** adds both, plus the normal's azimuth and elevation from the finest grid's slope. The material code stays 0 until terrain material maps exist (milestone 14).
  - **Valid is 0** outside the database, or where no tile reads.
- **Done when:**
  - Each kind is checked against the terrain query on a test database, and HAT plus HOT equals the MSL altitude.
  - A periodic request keeps its period and stops on 0.
  - An entity point follows its entity.
  - Each answer arrives in the IG's next message (Aerie sends about two requests a frame), measured. It holds up under 100 requests a frame, with the render thread untouched (`AllocationCounter`, frame timing).
  - Stats count requests, answers and invalid answers.

## Step 4: The test host and the real simulator

- **The test host.** `cigi-test-host` can send HAT/HOT requests from a script, logging the answers. Its default is Aerie's pattern: Extended, at the ownship, once per host frame at 100 Hz. One-shot and periodic requests at fixed points are also possible. A KHSV script lands on 36L and checks the HAT at touchdown against the gear height.
- **At the desk, with the team's simulator:**
  - it connects;
  - its MSL altitudes put the aircraft on the runway;
  - its HAT/HOT requests are answered;
  - latency is measured.
- Results are recorded, and anything unexpected becomes an issue.

## Inputs needed from you and the team

| Needed by | Input |
|---|---|
| Step 1 | Aerie's `ellipsoid_minus_msl_m` set to −29.55 m at KHSV, with Fovea's |
| Step 2 | Confirm 8004 and 8005 are Aerie's ports |
| Step 4 | Time with EYAS and Aerie at the desk, and the other packets Aerie sends besides IG Control, Entity Control and HAT/HOT |
