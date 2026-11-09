# Fovea Architecture

This document turns `fovea-plan.md` into concrete structure: the library boundaries, the core interfaces, threading and memory rules, and the channel config schema. Design choices and their reasons are recorded in `decisions.md`.

## Repository layout

```
apps/
  fovea/              runtime IG executable
  fovea-compiler/     offline terrain compiler (only consumer of GDAL)
libs/
  core/               math, time, logging, config, lock-free containers, SceneState
  terrain/            database reader + TerrainQuery (CPU-only, no Vulkan)
  models/             glTF (.glb) model reader (CPU-only, no Vulkan; D-023)
  platform/           SDL3 windows, monitors, input, high-res timers, thread setup
  net/                UDP transport + CIGI codec + CIGI→SceneState adapter
  render/             Vulkan device, Views, Presenters, frame graph, shaders
tools/
  cigi-test-host/     scripted CIGI host for testing
config/               example runtime and channel configs
shaders/              Slang sources, compiled to SPIR-V at build time
docs/
```

`libs/terrain` is an addition to the plan's layout. It keeps foundational decision 1 (shared terrain queries) as a real library boundary, not just a convention.

## Library dependency graph

Arrows mean "depends on". The graph has no cycles, and CMake enforces it through target links.

```
core      ← (nothing; std, glm, toml++ and stb_image only)
terrain   ← core
models    ← core            (+ cgltf, private)
platform  ← core            (+ SDL3, private)
net       ← core
render    ← core, platform, terrain, models  (+ volk, VMA, vk-bootstrap, Dear ImGui, private; the terrain paging thread, which loads models too)

apps/fovea           ← core, terrain, platform, net, render
apps/fovea-compiler  ← core, terrain, models  (+ GDAL)
tools/cigi-test-host ← core, net
```

Rules:
- `core`, `terrain` and `models` never include Vulkan, SDL or GDAL headers. That lets a host link `terrain` to answer height-of-terrain queries from the same database the IG displays, and tests read models without a GPU. The renderer takes on `models` when it draws them (milestone 4 step 3).
- `net` never includes renderer headers. It writes only `SceneState`.
- `render` never includes `net` headers. It reads only `SceneState`. That boundary is what keeps the renderer protocol-neutral.
- Third-party dependencies are linked `PRIVATE` wherever their types don't appear in public headers.

## Coordinates and precision

- World positions are **ECEF in `double`** (WGS-84) on the CPU. Geodetic ↔ ECEF conversions live in `core`.
- **Body frame** (entities and the viewpoints on them): x forward, y right, z down, the common aerospace and vehicle convention (also CIGI's). Orientation is yaw, pitch and roll in degrees at the interface and quaternions internally.
- **GPU positions are camera-relative `float`.** Each frame, each view's render origin is its own eye's ECEF position. Tiles store vertices relative to a per-tile `double` origin, and the per-draw offset `(tile_origin - eye)` is computed in `double` and then narrowed.
- **Reverse-Z depth:** a `D32_SFLOAT` depth buffer cleared to 0, with an infinite far plane.

## Threads and data handoff

```
 Network thread ──writes──▶ SceneState (triple buffer) ──reads──▶ Render thread
                                                                    ▲   │
 Paging thread ◀──requests (SPSC ring)──────────────────────────────┼───┘
       │                                                            │
       └──GPU uploads (transfer queue, timeline semaphore)──completions (SPSC ring)

 Network thread ──height requests (SPSC ring)──▶ Query thread ──answers (SPSC ring)──▶ Network thread
```

- **SceneState handoff** uses a wait-free triple buffer of a fixed-size snapshot. The writer publishes complete snapshots, and the render thread takes the latest one at the start of each frame. Neither side ever waits on the other.
- **Snapshot contents:** a fixed-capacity entity table (D-011: no entity is special; views attach their viewpoints to entities or fix them in the world), and environment state (time of day, and later weather). Every sample carries two timestamps: host time from the protocol and local receive time from a monotonic clock. Each entity keeps its last two samples, so the renderer can interpolate or extrapolate without the network thread knowing about frames.
- **Paging** (milestone 3 step 6): the render thread pushes tile requests (from each view's tile selection, and where its eye is heading) into an SPSC ring, having reserved each a slot in a fixed-size tile cache. The paging thread does the file I/O, decoding, image creation and staging, and records the copies. Completions come back through another SPSC ring with their command buffers, which the render thread submits ahead of its own frame, in the same submit (no transfer queue yet, D-021: the render thread waits for nothing, and no queue-ownership transfers are needed); a timeline semaphore that every frame signals tells the paging thread when staging memory and command buffers are free again. Evicted slots are reused only once the frames that drew them have finished. The same thread loads the models the database places (milestone 4 step 3), each once, between tiles, through the same staging ring and batches, handing them over through a third ring; their GPU memory is shared buffers and a bindless image array, filled and never freed while the database is loaded.
- **Terrain queries** (Integration 1 step 3, D-036): a host's HAT/HOT requests go from the network thread to a query thread through an SPSC ring (`HeightQueries`, protocol-neutral, in core), and the answers come back through another, to leave in the next message to the host. The query thread has its own reader of the database (`DatabaseTerrain`, in terrain), so a tile read from disk delays only the answers waiting on it. It sleeps on an atomic until requests are posted. The render thread is never involved.
- **Control and stats:** stats are written into a separate SPSC ring that a stats or control thread drains. Milestone 1 writes them to a log file. A later milestone exposes them, together with control commands, over a local socket for the external control UI. That traffic stays separate from CIGI.

## Render path rules

The render path means everything the render thread does between frame start and the last present:
- **No heap allocation.** Per-frame scratch memory comes from a linear arena that's reset every frame. Long-lived containers are given their size at init. Debug builds hook the global allocator to assert on allocations during the frame.
- **No disk I/O and no blocking waits** except the one deliberate wait: the frame fence or present pacing at the top of the frame.
- **Vulkan objects** (pipelines, descriptor heaps, pools) are created at init. Streamed resources are created on the paging thread.
- **GPU-driven rendering:** bindless descriptors through descriptor indexing, one large descriptor set per resource kind, indirect draws, and a clustered light list. Milestone 1 builds the bindless setup even though the grid scene doesn't need it, so later work doesn't have to retrofit it.

## Core interfaces

The headers are the reference; these are their shapes, as built in milestone 1 step 3. Keep this in step with them.

```cpp
// libs/core/scene.hpp: protocol-neutral scene state
struct Pose { DVec3 ecef; DQuat orientation; HostTime host_t; LocalTime recv_t; };
struct EntityState { EntityId id; ModelId model; Pose cur, prev; FixedArray<Articulation, 8> parts; };
struct SceneSnapshot {
  FixedArray<EntityState, kMaxEntities> entities;   // no special "ownship" (D-011)
  EnvironmentState environment;
  uint64_t sequence;
};
class SceneState {                       // one writer thread, one reader thread
  SceneSnapshot& edit();                 // writer: working copy, kept between publishes
  void publish();                        // writer: copies it to the reader
  const SceneSnapshot& latest();         // reader: valid until the next latest()
};
const EntityState* find_entity(const SceneSnapshot&, EntityId);

// libs/net/host_adapter.hpp: CIGI first, other protocols later; the only SceneState writer
class HostAdapter { virtual bool poll(SceneState&) = 0; };

// libs/terrain/terrain_query.hpp: CPU-only spatial queries
class TerrainQuery {
  virtual std::optional<double> height_at(double lat, double lon) const = 0;  // m above WGS-84
  virtual std::optional<TerrainHit> intersect(const DRay&, double max_distance_m) const = 0;
  bool line_of_sight(const DVec3& a, const DVec3& b) const;  // via intersect, 0.1 m end tolerance
};

// libs/render/view.hpp: a View is the general rendering unit
struct ViewDesc {
  std::string name;
  PoseSource pose;            // riding on an entity (offset, rotation), or fixed in the world
  Projection projection;      // asymmetric frustum (angles) + near plane
  RenderMode mode;            // visual now; NVG/IR/lidar later
  PostChain post;             // tone map + per-view effects
  OutputTarget target;        // a Presenter output, or an offscreen image
  uint32_t render_width, render_height;  // independent of the output resolution
};
ViewDesc view_from_channel(const ChannelConfig&, const DisplayConfig&, size_t index);

// libs/render/presenter.hpp: hides "3 swapchains" vs "1 spanned surface" vs "offscreen"
class Presenter {
  virtual std::span<const OutputDesc> outputs() const = 0;
  virtual void acquire(FrameContext&) = 0;                 // an image for every output
  virtual void present(FrameContext&) = 0;                 // back-to-back presents
  virtual PresentTiming last_timing(uint32_t output) const = 0;  // missed-frame and sync stats
};
// Implementations, from step 5: MultiSwapchainPresenter, SpannedPresenter, OffscreenPresenter

// libs/render/module.hpp: compiled-in modules, enabled through config
class Module {
  virtual void init(const ModuleContext&) = 0;
  virtual void update(const FrameContext&) = 0;            // the scene is frame.scene
  virtual void record(CommandRecorder&, const ViewDesc&) = 0;
  virtual void shutdown() = 0;
  virtual GpuBudget budget() const = 0;                    // declared; actual cost reported via stats
};
```

Views write to images. Each output gets its view's render-resolution image, which is tone-mapped and scaled into it. A sensor view works the same way, with an offscreen target and no output.

## Frame pacing and display sync

- The frame clock is locked to the display. Each frame: wait until the previous frame is on screen (`VK_KHR_present_wait`, on the first output), take the latest SceneState, update modules, record all views, submit once, then present to every output in one call. Starting right after a vsync gives even frame spacing and the least latency. Without present wait, FIFO blocking paces the loop, but unevenly (measured: frames bunched in pairs). Offscreen and non-FIFO outputs are paced by the clock at `target_hz`.
- The present mode is set per output in config. The default is FIFO on all outputs. The milestone 1 sync test measures FIFO on every output against FIFO on one output with the others set to MAILBOX/IMMEDIATE, plus the driver's display-spanning mode through `SpannedPresenter`. Results go into `decisions.md`.
- A missed frame is any frame whose present interval is more than 1.5× the target period. Present timing comes from `VK_KHR_present_wait`/`present_id` when available, and from CPU timestamps otherwise.
- Stats recorded every frame: CPU frame time, GPU time per view and per module (timestamp queries), present interval, and missed frames. End-to-end latency (host timestamp to present) is added in milestone 2.

## Channel config schema

Each `[[channel]]` defines one display view and its output. Angles are in degrees and offsets in meters, in the body frame.

```toml
# config/channels.example.toml
[display]
presenter = "multi_swapchain"   # or "spanned", "offscreen"
target_hz = 60
near_m = 0.1                    # reverse-Z, infinite far

[[channel]]
name = "left"
monitor = 0                     # platform monitor index
present_mode = "fifo"
render_extent = [1920, 1080]
eye_offset = { x = 0.0, y = 0.0, z = 0.0 }
view_rotation = { yaw = -60.0, pitch = 0.0, roll = 0.0 }
frustum = { left = -30.0, right = 30.0, top = 17.0, bottom = -17.0 }

[[channel]]
name = "center"
monitor = 1
present_mode = "fifo"
render_extent = [1920, 1080]
eye_offset = { x = 0.0, y = 0.0, z = 0.0 }
view_rotation = { yaw = 0.0, pitch = 0.0, roll = 0.0 }
frustum = { left = -30.0, right = 30.0, top = 17.0, bottom = -17.0 }

[[channel]]
name = "right"
monitor = 2
present_mode = "fifo"
render_extent = [1920, 1080]
eye_offset = { x = 0.0, y = 0.0, z = 0.0 }
view_rotation = { yaw = 60.0, pitch = 0.0, roll = 0.0 }
frustum = { left = -30.0, right = 30.0, top = 17.0, bottom = -17.0 }
```

- The frustum half-angles may be asymmetric, which is how the horizon stays continuous across bezels. The example values are placeholders until the physical screen geometry is measured.
- Planned alternative: `screen = { lower_left, lower_right, upper_left }` corners in the eye frame. The frustum is derived from them, which is easier for the measured setup to get right.
- Config loading checks that monitor indices exist, that the angles produce a valid frustum, and that channel names are unique. It fails at startup with a clear message.

## Headless and replay

`OffscreenPresenter` renders the same views to images without a window. Combined with a replay adapter that writes recorded CIGI traffic into SceneState at its original timing, it enables golden-image comparisons and repeatable performance runs. Milestone 1 provides the offscreen presenter and a `--headless --frames N --dump <dir>` option. Replay arrives with CIGI in milestone 2.

## Milestone 1 build order

1. CMake skeleton, presets, dependency fetching, warnings, CI on GCC, Clang and Windows MSVC (D-008, D-009; done).
2. `core`: math (doubles, ECEF/geodetic), monotonic time, logging, triple buffer, SPSC ring, frame arena, config structs and TOML loading with validation, plus unit tests.
3. Interface stubs: `SceneState`, `TerrainQuery`, `View`, `Presenter`, `Module`.
4. `platform`: monitor enumeration, borderless window per monitor, event pump.
5. `render`: Vulkan 1.3 device (with validation in debug builds), bindless setup, `MultiSwapchainPresenter`, per-channel off-axis projection, the grid-and-horizon reference scene in Slang, and tone mapping.
6. 60 Hz frame loop, timestamp queries, stats log, ImGui frame-timing overlay.
7. `OffscreenPresenter` and the headless dump option, followed by the display sync test procedure and its results.
