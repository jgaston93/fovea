# Integration guide: driving Fovea from a host

This guide is for connecting a host program, such as a flight simulator, to Fovea over CIGI 3.3. It covers what Fovea handles today and the config it needs on both sides. Fovea's CIGI code is written from the CIGI 3.3 ICD and its errata. The section and page numbers below are the ICD's; `tools/fetch-cigi-icd.sh` downloads it into `docs/reference/`.

The design behind it: [D-011](decisions.md) (protocols are adapters), [D-013](decisions.md) (CIGI 3.3 first), [D-014](decisions.md) (Fovea's own codec), [D-035](decisions.md) (the height datum), [D-036](decisions.md) (HAT/HOT) and [D-037](decisions.md) (WGS-84). `docs/integration-1.md` records the first integration, with the team's simulator.

## Connecting

Fovea listens when its config has a `[host]` section. Without one, nothing listens, and the views stand at the config's `[reference_scene]` origin.

```toml
[host]
protocol = "cigi"
port = 8004                  # where the host's messages arrive (8004 if left out)
listen_address = "127.0.0.1" # optional: listen on one address only
host_address = "127.0.0.1"   # where Fovea's messages go (left out: wherever the host sends from)
host_port = 8005             #   and at which port (8005 if left out; 0: the one it sends from)
byte_order = "network"       # what Fovea sends: "network" (big-endian, the default) or "native"
geoid_height_m = -29.55      # the geoid above the WGS-84 ellipsoid: must match the host's
```

`config/channels.example.toml` documents the keys, and `config/khsv.toml` is a working example.

- **`geoid_height_m` defaults to 0.** Left out, MSL altitudes are taken as ellipsoidal heights, about 30 m off at KHSV.
- **Without `host_address`, nothing is sent until the host's first message.** A host that waits for a Start of Frame before sending must have `host_address` set.
- **A loopback `listen_address`** (127.x.x.x) with a `host_address` on another computer is refused: Start of Frame couldn't leave this one.

- **Transport:** UDP, one CIGI message per datagram, each way.
- **Ports:** the ICD names none. 8004 for the IG and 8005 for the host are the usual pair, and the defaults here.
- **Byte order:** Fovea reads either order, telling them apart by the Byte Swap Magic Number in IG Control and Start of Frame (2.5, p. 7). `byte_order` sets the order it sends.
- **Version:** Fovea sends Major Version 3, Minor Version 2, as the 3.3 ICD gives it (IG Control p. 44, Start of Frame p. 215). It accepts any minor version.

### Startup and frames

- **Every host message starts with IG Control** (4.1.1, p. 42). A message that doesn't is rejected whole.
- **Startup:** the IG starts in Reset/Standby and ignores everything else until IG Control asks for Operate. The rest of the message that asks for Operate is ignored too. The host should send IG Control alone until a Start of Frame shows Operate, then start sending data (2.8, p. 13).
- **Entering Reset/Standby** destroys every entity, ends every periodic HAT/HOT request, and puts the config's environment back (2.7.3, p. 13; p. 214).
- **Start of Frame** (4.2.1, p. 212) goes to the host once a rendered frame, at 60 Hz. Any HAT/HOT answers ready by then ride in the same message.
- **Either CIGI mode works.** A synchronous host answers each Start of Frame; an asynchronous host sends at its own rate. The render thread never waits for the host, and each frame draws the newest state that has arrived.
  - **Asynchronous hosts:** set Extrapolation/Interpolation Enable (IG Control and Entity Control) so the IG smooths samples to its frame times. At the desk, a 60 Hz asynchronous host without smoothing showed visible hitches in turns.
- **Timestamps** are 10 µs ticks (p. 44), used when IG Control's Timestamp Valid is set. Smoothing then follows the host's clock; without it, it follows when messages arrive, which network jitter disturbs, so asynchronous hosts should set it. The clock may roll over or restart. A stamped message up to a second earlier than the last one is taken as out of order and dropped.

## What Fovea handles

| Packet | ICD | Direction | What Fovea does |
|---|---|---|---|
| IG Control (1) | 4.1.1, p. 42 | host → IG | IG mode, frame number, timestamp, smoothing |
| Entity Control (2) | 4.1.2, p. 48 | host → IG | positions entities; entity 0 carries the views |
| Celestial Sphere Control (9) | 4.1.9, p. 82 | host → IG | date and time, and whether time runs |
| Atmosphere Control (10) | 4.1.10, p. 85 | host → IG | global visibility |
| Weather Control (12) | 4.1.12, p. 100 | host → IG | global fog, cloud, rain and snow layers |
| HAT/HOT Request (24) | 4.1.24, p. 152 | host → IG | terrain heights, answered in the next message |
| Start of Frame (101) | 4.2.1, p. 212 | IG → host | every frame |
| HAT/HOT Response (102) | 4.2.2, p. 217 | IG → host | answers to Request Type 0 and 1 |
| HAT/HOT Extended Response (103) | 4.2.3, p. 219 | IG → host | answers to Request Type 2 |

Every other packet is skipped by its size field. A host can send its full set without breaking anything. A packet Fovea handles but whose values are out of range is rejected: see each below.

### Entities and the eyepoint

- **Entity Control** gives an entity's geodetic position, with altitude above mean sea level, and its yaw, pitch and roll. The first one creates the entity; Entity State Destroyed removes it.
- **The views ride entity 0** by default. Each channel adds its own eye offset and view rotation in the entity's body axes (`[[channel]] eye_offset`, `view_rotation`). A channel's `entity` key picks another entity.
- **Other entities are tracked but not drawn yet** (milestone 9). Their HAT/HOT entity-point requests work.
- **Attached (child) entities** are counted but not supported yet (#20). A top-level entity that becomes attached stops being drawn or ridden.
- **The entity table holds 1,024.** New entities beyond that are dropped.

### Time and weather

Until a host sets them, the date, time, visibility and weather layers come from the config's `[environment]`.

- **Celestial Sphere Control:**
  - The date and time are taken when Date/Time Valid is set. Ephemeris Model Enable decides whether the clock runs.
  - A time within the minute the IG's clock is already in leaves the clock running, so a host may send its time every frame.
  - Dates from 1950 to 2050 are accepted. A date outside them, an hour over 23 or a minute over 59 rejects the whole packet, its Ephemeris Model Enable included.
  - The sun, moon and star enables and the star intensity are ignored: the sky always draws them.
- **Atmosphere Control** sets the global visibility; a visibility of 0 or less rejects it. Humidity, temperature, wind and pressure change nothing drawn yet.
- **Weather Control** sets or removes a whole global layer:
  - **Layers handled:** Layer IDs 0 to 5 (fog, three cloud layers, rain, snow). Each takes its base (above MSL), thickness, visibility and coverage. A negative thickness or visibility rejects the packet.
  - **What's drawn:** fog as a band of reduced visibility; rain and snow as visibility within them. Cloud layers aren't drawn yet (#73).
  - **Ignored:** sleet, hail, sand, dust, the IG's own layer IDs, regional layers, weather entities and Environmental Region Control.

## Heights and the datum

Fovea's database is WGS-84 (D-037). CIGI altitudes are above mean sea level. Fovea converts between the two with a single constant, `[host] geoid_height_m`: the geoid's height above the WGS-84 ellipsoid (D-035).

- **The same constant applies everywhere:** entity positions, weather layer bases, HAT/HOT altitudes, and the HOT answers.
- **It must match the host's own constant** (EYAS's Aerie calls it `ellipsoid_minus_msl_m`). Then the two agree on MSL exactly. A mismatch shifts the whole world up or down by the difference.
- **At KHSV it is −29.55 m.** That is GEOID18's −28.25 m less the 1.30 m from NAD83(2011) to WGS-84.
- **Fovea doesn't use a geoid grid.** A host that uses a geoid model will drift from the constant away from the area it was set for (#19).

## HAT/HOT

A host asks for the terrain's height under a point with HAT/HOT Request (4.1.24, p. 152). The answer comes back with a Start of Frame: usually the next one, or a frame later when the query thread hasn't finished it in time.

- **Points:**
  - **Geodetic:** latitude, longitude, and altitude above MSL.
  - **Entity:** X, Y, Z in an entity's body axes, from its latest pose.
- **Request Types:**
  - **0, HAT:** the point's height above the terrain.
  - **1, HOT:** the terrain's height above MSL.
  - **2, Extended:** both, plus the surface normal's azimuth and elevation.
  - **3:** undefined, and rejected.
- **Update Period:**
  - **0:** one answer.
  - **n > 0:** an answer every nth frame, the first at once, until a request with the same ID sets the period to 0, the entity is destroyed (a last answer with Valid 0), or the IG enters Reset/Standby.
  - **Table size:** up to 64 periodic requests are held. More are answered once.
- **The answers:**
  - **Source:** the finest terrain that reads cleanly at the point, not whatever level of detail a distant view draws (D-036).
  - **Scope:** terrain only. Models and placed buildings aren't included.
  - **Material code:** 0 until terrain materials exist.
  - **Host Frame Number:** every answer carries the low 4 bits of the last one received when it was asked.
- **Valid is 0** outside the database, where no tile reads, for a latitude beyond ±90° or a coordinate that isn't a number, for an entity that doesn't exist or is inactive, or when Fovea has no database.
- **Rate:** they're answered on their own thread, away from rendering. A test sends 100 requests in one message and gets all of them back within the next two.

## Trying it without a simulator

`cigi-test-host` is a scripted CIGI host. It flies entity 0 through a script's segments, asynchronously at the script's rate or once per Start of Frame (`--sync`), and reports the Start of Frame round trip.

```sh
./build/release/apps/fovea/fovea --overlay config/khsv.toml
./build/release/tools/cigi-test-host/cigi-test-host tools/cigi-test-host/scripts/khsv-approach.toml
```

- **Scripts:** `tools/cigi-test-host/scripts/` has an approach to KHSV 36L, low passes over KHSV and KHUA, a dawn flight in which the host sets the time and fog, a night approach to see the airport's lights, and others. Each script's header says what it shows.
- **Matching addresses:** the script's `[ig] address` must reach Fovea. Fovea's `[host] port` must be the script's `[ig] port`, and its `host_port` the script's `[host] port`.

## Checking a connection

When Fovea exits, it prints what the host link did: messages received and rejected, messages ignored in Reset/Standby, entity updates, Start of Frame messages sent, and HAT/HOT requests, answers, invalid answers and lost answers. Packets skipped or rejected one by one, ignored weather and dropped entities are counted inside Fovea but not printed yet. Common problems:

| Symptom | Likely cause |
|---|---|
| Every message rejected | A message doesn't start with IG Control |
| Messages ignored in Reset/Standby | The host never asked for Operate |
| No Start of Frame at the host | `host_address`/`host_port` don't point at it, or a firewall blocks it |
| The aircraft floats above or sinks into the runway | `geoid_height_m` differs from the host's constant |
| HAT/HOT answers all invalid | The point is outside the database, or no database is loaded |

## Not supported yet

- **Entities:** drawing entities other than the eyepoint, attached entities and articulated parts (milestone 9, #20). Line of sight and other terrain queries over CIGI (milestone 9).
- **Weather:** drawing cloud layers (#73), and regional weather.
- **Airport lights:** they are always on, at one brightness. A host can't switch them or set their intensity step yet, and approach lighting systems aren't drawn.
- **Other packets:** view and sensor control, component control and the remaining packets, which are skipped.
- **Recording:** recording and replaying CIGI traffic (#29).
- **Control plane:** a control and stats channel separate from CIGI (milestone 12).
