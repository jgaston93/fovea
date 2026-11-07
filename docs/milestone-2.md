# Milestone 2: CIGI eyepoint

The goal from `fovea-plan.md`: build a small scripted CIGI test host, receive its eyepoint over CIGI, fly a camera over a test grid, and log end-to-end latency. The structure follows `architecture.md`: `libs/net` holds the UDP transport, the CIGI codec and the CIGI adapter, the adapter is the only writer of `SceneState`, and nothing in `render` sees a packet (D-011).

Like milestone 1, each step is its own pull request, reviewed and merged once CI passes. Steps 1 to 3 and 5 to 6 need no display and run in WSL and CI (loopback UDP, lavapipe). Steps 4 and 7 need the Windows machine.

## Decisions (accepted 2026-11-06)

The packet layouts come from the CIGI 3.3 ICD and its errata, fetched by `tools/fetch-cigi-icd.sh` into `docs/reference/` (git-ignored, hash-checked), never from memory.

- **CIGI version (D-013: 3.3).** CIGI 3.3 is the version most hosts and IGs implement, and its interface control document is freely available. CIGI 4.0 (SISO-STD-013-2014) changes packet sizes and IDs and is less widely supported. The codec keeps version-specific layouts in one place, so 4.0 can be added later beside 3.3.
- **Codec (D-014: Fovea's own).** Only a handful of packets are needed for this milestone. The Boeing CIGI Class Library is LGPL, old C++ and allocates per packet. Fovea's codec is a few hundred lines of fixed-size encode and decode, tested byte for byte against the ICD's layouts.
- **UDP sockets.** A thin non-blocking UDP socket (Winsock or BSD sockets) in one file of `libs/net`, the only platform-specific code there. No networking library.

## Step 1: CIGI 3.3 codec

- `libs/net`: encode and decode, without allocation, for the packets this milestone uses. Host to IG: IG Control (frame counter, timestamp, IG mode), Entity Control (entity 0's position and attitude). IG to host: Start of Frame. Other packets are skipped by their size field.
- Byte order from the IG Control's byte-swap magic number, as the ICD specifies.
- **Done when:** tests cover each packet against hand-built byte layouts from the ICD, round trips, both byte orders, and messages that are short, mis-sized or have unknown packets (rejected or skipped, never read out of bounds). The decoder is fuzzed with libFuzzer in the nightly run.
- **Result:** `fovea/net/cigi.hpp` encodes and decodes IG Control, Entity Control and Start of Frame, and `MessageReader` walks a message's packets (checking the first packet, major version and byte swap magic number). Each packet's layout is copied into a comment from its ICD figure, with the page. The test bytes are written by hand from the figures, for big- and little-endian senders, with neighbouring flag bits set differently; nine planted codec bugs (wrong bit, wrong offset, a missing byte swap) each fail 2 to 4 tests. Reading and writing allocate nothing. `tests/fuzz/fuzz_cigi.cpp` (the `fuzz` preset) also checks that every decoded packet survives encode, decode, encode; 13.6 million inputs in 60 s found nothing, and a planted 8-byte overread was found within the minute. The nightly run fuzzes for 5 minutes.

## Step 2: UDP and the network thread

- A non-blocking UDP socket, and a network thread that receives host messages, decodes them, and publishes the latest entity state to `SceneState` with two timestamps: the host's (from IG Control) and the local receive time.
- The IG sends Start of Frame to the host once per rendered frame, from the render thread through an SPSC ring (the render thread never touches a socket).
- Asynchronous mode first: the IG runs on its own 60 Hz clock and draws the latest state. Synchronous mode (the host steps the IG) comes later if a host needs it.
- **Done when:** loopback tests send packets and see them in `SceneState`, the network thread's steady state allocates nothing, and a flood of packets or garbage on the port can't stall or crash it.
- **Result:** `UdpSocket` (one file, BSD sockets or Winsock), `CigiAdapter` (the `HostAdapter` for CIGI) and `HostThread` in `libs/net`; `FrameStartRing` in `libs/core`, which `FrameLoop` fills and the adapter drains into Start of Frame. A `[host]` config section turns it on. From the ICD: IG Mode starts at Reset/Standby, where host data is ignored (including the rest of the message that asks for Operate, ICD 2.8), and entering it destroys every entity (p. 214); timestamps are unwrapped across their 32-bit rollover, a message stamped up to a second early arrived out of order and is dropped, and a larger step back is a restarted host clock. After a stall only the newest Start of Frame goes out. The network thread wakes every millisecond so Start of Frame leaves within about 1 ms of its frame: measured, about 1% of one core while idle. Top-level entities become ECEF poses (sea-level altitude plus `geoid_height_m`, a constant until a geoid model arrives with terrain); attached entities are counted for milestone 7. Measured: a burst of 300 datagrams sent before the adapter reads overflows Linux's default receive buffer (about 240 fit; the rest are dropped by the system), so the socket asks for 4 MB (Linux grants 416 KB), and `poll()` handles at most 64 messages before sending the next Start of Frame. Views don't ride the entities yet (step 3).

## Step 3: Views ride entities

- The scene gets a world origin (a geodetic point in config, KHSV for now). Each frame, every view's eye is placed from its entity's pose (geodetic position, yaw, pitch and roll) plus the channel's eye offset and view rotation, in double precision. The GPU gets camera-relative floats.
- The reference grid becomes fixed to the world (a local level plane at the origin), so moving the entity flies the camera over it. The horizon follows the eye's height.
- Without a host the entity stays at a configurable default pose, so the milestone 1 behaviour and golden images are unchanged.
- **Done when:** tests check that moving the entity moves the view as expected (translation, each rotation axis, the eye offset riding the body axes), and that the camera-relative path keeps sub-millimetre precision 100 km from the origin.
- **Result:** `[reference_scene] origin` (sea-level altitude, like a host's; `[host] geoid_height_m` applies) sets where the grid is. The grid follows the Earth's curve at the origin's altitude, as ground does: a view's eye height is its altitude above the origin's, its attitude is taken against its own local level, and its grid position comes from latitude and longitude (scaled by the curvature radii at the origin). `ride_entities` (render) places each view's eye from its entity's latest pose plus the channel's offset and rotation, in double precision; the GPU gets the rotation, the height and the eye's offset from the nearest grid crossing, so nothing it gets grows with distance. `FrameLoop` takes a `ViewRig` and updates its views each frame (1.1 us for three views, no allocation); the app uses one when `[host]` is set. A view keeps its last view while its entity is missing or inactive (the milestone 1 pose until it first appears), and eyes at or below the grid are counted in the run summary. Without a host the golden images are unchanged. The review caught that a first version measured from the origin's flat tangent plane: at a constant 300 m, the eye sank to 79 m above the grid 20 km out and to the ground beyond 62 km; a test now flies level at 20 to 300 km. Views use the entity's latest sample; smoothing between samples is for the latency work (step 5).

## Step 4: The CIGI test host

- `tools/cigi-test-host`: a small program that flies scripted motion (a TOML script of segments: hold, straight and level, climb, turn, roll, a figure eight) at 60 Hz over UDP, and prints the Start of Frame round trip.
- **Done when:** the host flies Fovea over the grid on the three monitors, smooth to the eye, with no missed frames in a 5-minute run.
- **Result:** `tools/cigi-test-host` (a library of flight, script and session, plus `main`) flies entity 0 through TOML scripts (`scripts/circuit.toml`, `scripts/gentle.toml`), asynchronously at the script's rate or once per Start of Frame (`--sync`), following the ICD's startup (IG Control alone until Start of Frame shows Operate), sends on a drift-free schedule that skips (and counts) missed deadlines rather than bursting, and reports the Start of Frame round trip. `config/cigi-test-host.toml` flies the desk; `[host] listen_address = "127.0.0.1"` keeps the link on this computer, so Windows Firewall doesn't ask. Tests: the flight's kinematics (distance, climb, coordinated bank, full turns, figure eight, rolls), script errors with lines, and the host flying Fovea's adapter in-process over loopback. On the desk (40 s, screen locked): 1,921 messages, none rejected; Start of Frame every frame; round trip mean 11.8 ms, max 32 ms, timed as each Start of Frame arrives (a first version timed them at the next send and reported 17.2 ms; asynchronously a message waits on average half a frame for the next frame start); 2,398 frames with 3 missed, at startup and at 20.0 s and 39.5 s: the 59.95 Hz / 60.00 Hz monitor beat (one frame in 20 s), recorded in #21. **5-minute run (owner watching, desktop unlocked):** 18,001 messages, none rejected; 18,701 frames, none missed between startup and the host stopping (2 at startup, 3 after); round trip mean 10.6 ms, max 33 ms; 0 validation messages. Smooth overall, but the owner saw slight jaggedness in banked turns. The stats show why: asynchronously, the host's 60 Hz and the display's drift against each other, so 6.3% of frames got no new position (a repeat) and 5.9% got two (a jump), about 7 hitches a second, visible when the whole view rotates. With `--sync` (one message per Start of Frame) 99.2% of frames got exactly one position (0.8% repeats, no jumps) at a steady one-frame round trip (16.7 ms), and the owner saw it as much smoother. Smoothing asynchronous hosts' samples (interpolating or extrapolating to each frame's time) is for step 5.

## Step 5: End-to-end latency

- Each frame records: host send time to IG receive, receive to the frame that first draws it, and that frame's start to present (present wait). The host and IG on one machine share a monotonic clock, so these add up exactly. Across machines, the round trip from Start of Frame gives an estimate.
- Written to the per-frame stats CSV and the end-of-run summary, and shown in the overlay.
- **Done when:** the numbers are recorded for a run on the Windows machine, with each part explained. Display latency (present to light) is the display sync test's job.
- **Added after step 4:** smoothing. Asynchronously the host's and the display's 60 Hz drift against each other, so frames got 0 or 2 new samples (seen by the owner as slight jaggedness in turns; ICD 2.2.1 describes exactly this and suggests the IG extrapolate).
- **Result:** the CIGI adapter maps each sample's host timestamp onto this computer's clock (`Pose::sample_t`): the host clock's zero is placed where the fastest message puts it, creeping later at 100 ppm for drift between machines. Where the host allows it (Extrapolation/Interpolation Enable in IG Control and the entity's Entity Control), `ride_entities` draws the entity where its last two samples put it at the frame's start (`pose_at`): position and rotation continued at their velocity, at most 100 ms past the newest sample. The test host sets both flags (`--no-smoothing` clears them). Each frame's stats record the newest sample's age at frame start (on the host's clock mapped across, and since arrival) and the first view's heading; the run summary and the overlay show the age while the host is live.
  - **The parts, measured on the desk** (host and Fovea on one machine, asynchronous at 60 Hz, 45 s each): host send to arrival is under a millisecond on loopback (the two ages agree to 0.01 ms); the newest sample is a mean **11.2 ms** old at frame start (max 32 ms), and smoothing extrapolates across that; a frame starts right after a vsync (present wait) and is on screen at the next one, **16.7 ms** later; the display's own delay to light is the sync test's (#21). Round trip host to Start of Frame: mean 10.5 ms.
  - **Smoothness:** in the circuit's turns, frames whose heading step changed by more than half from the frame before: **20.8%** without smoothing, **0.4%** with it. A simulation test (a 60 Hz host drawn at 59.95 Hz) shows the same: steps of 0 or 2 samples without, even steps with.
  - **Still a little jagged to the eye**, though: smoothing then drew each frame for its *start*, and starts jitter (15 to 18.6 ms apart, about +-0.9 ms) while vsyncs don't, so heading steps still varied by about 13%. `DisplayClock` fits a steady vsync grid to the frame starts (following the display's real rate, 59.95 Hz on the Dell), and moving entities are now drawn for the vsync each frame will appear at. Tested against simulated wake-up jitter: display steps even to 0.3 ms. On the desk (45 s each; a first attempt ran while a game shared the GPU, 9% missed frames, and was discarded), the frame-to-frame change of the view's heading step in turns:

    | | median | 95th percentile |
    |---|---|---|
    | asynchronous, no smoothing | 5.8% | 89% |
    | asynchronous, smoothed for the display time | **0.25%** | **0.99%** |
    | synchronous (`--sync`) | 0.28% | 0.98% |

    The owner saw both of the last two as very smooth. Each run: 3,117 frames, 1 missed, 0 validation messages.
  - **Hardening from the review:** the host clock's mapping restarts with Reset/Standby and with a restarted host clock; IG Control's flag is scene-wide (`SceneSnapshot::smoothing_allowed`); a jump faster than 1000 m/s or 720 deg/s is a reposition, not extrapolated; extrapolation never goes back past the earlier sample; samples on different clocks (Timestamp Valid toggling) aren't combined.

## Step 6: Record and replay

- **Status:** deferred so milestone 3 could start (2026-11-06): #29.

- `fovea --record <file>` writes received CIGI messages with their receive times. A replay adapter writes them back into `SceneState` at their original timing, so a recorded flight can run headless.
- **Done when:** a recorded flight replays headless into golden images, frame for frame, in CI.

## Step 7: Hardware check

- **Status:** mostly measured in steps 4 and 5; the write-up is deferred (2026-11-06): #30.

- Fly the scripted routes on the three monitors with the overlay and stats on, and record frame timing and latency in the pull request.
- **Done when:** the routes fly at a locked 60 Hz with the latency breakdown recorded, and anything unexpected is either fixed or written up as an issue.

## Inputs needed from you

| Needed by | Input |
|---|---|
| Step 1 | ~~Approve D-013 (CIGI 3.3) and D-014 (own codec)~~ accepted |
| Step 4 | A list of manoeuvres you care about, if any beyond the defaults |
| Later | A real host to connect to, if you have one in mind (a simulator or your own program), and its CIGI version |
