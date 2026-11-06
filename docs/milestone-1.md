# Milestone 1: Skeleton

The goal from `fovea-plan.md`: three borderless windows on three monitors, per-channel off-axis frustums from config, a locked 60 Hz frame clock, frame-time logging, and a measured display sync test. The structure follows `architecture.md`, and the reasons behind choices are in `decisions.md`.

The milestone is split into eight steps, each its own pull request (step numbers are not GitHub PR numbers), merged once CI passes. Steps 1 to 3 need no GPU or display and can be done entirely in WSL and CI. Steps 4 to 8 need the Windows machine (Visual Studio 2022, the Vulkan SDK, three monitors).

## Testing without a GPU

GitHub's CI runners have no GPU. From step 5 onward, the Linux CI jobs install Mesa's software Vulkan driver (lavapipe) and run the renderer headless through `OffscreenPresenter`. Window and present behavior can only be checked on real hardware, so those checks are manual, done on the Windows machine and written up in the pull request.

## Step 1: `libs/core` foundations (done, #3)

- Math: a `double` vector, quaternion and matrix library. Geodetic ↔ ECEF conversion (WGS-84), local ENU/NED frames at a point, and body-frame yaw/pitch/roll ↔ quaternion conversion using CIGI's axis conventions.
- Time: a monotonic clock, `LocalTime`, and a fixed-period tick helper.
- Logging: a lock-free log that the render thread can write to without allocating. A background thread drains it to the console and a file.
- Containers: `TripleBuffer<T>` (wait-free latest value), `SpscRing<T, N>`, `FrameArena` (a linear allocator reset every frame) and `FixedArray<T, N>`.
- `tests/support/alloc_counter` from eyas, adapted for MSVC (`_aligned_malloc`). From here on, tests check that render-path code allocates nothing.
- **Done when:** unit tests cover the geodesy round trip and known reference points (KHSV, the equator, the poles), the axis conventions, and the containers under two threads. This includes a stress test of the triple buffer, which runs under ASan and under a new `tsan` preset (ThreadSanitizer, added to the nightly run).
- **Math library:** glm 1.0.1 (D-010).

## Step 2: Config (done)

- Load `config/channels.example.toml` (the schema in `architecture.md`) into plain structs, using toml++ (D-002).
- Validation, reported with file and line: unknown keys, missing fields, duplicate channel names, frustum angles that can't produce a valid frustum, and render extents that are zero or too large. Monitor indices are checked later, at window creation, because that's when the monitors are known.
- Off-axis projection: build a reverse-Z projection with an infinite far plane from the frustum angles. This is CPU-only and tested here.
- **Done when:** tests cover the example config, every validation error, and the projection's corner rays matching the configured angles.

## Step 3: Interfaces and library layout (done)

- Create `libs/core`, `libs/terrain`, `libs/platform`, `libs/net` and `libs/render` as CMake targets, linked as the dependency graph in `architecture.md` allows. Add `apps/fovea` as a binary that loads the config and exits.
- Stub interfaces: `SceneSnapshot` and its writer and reader, `TerrainQuery` (with a flat-earth implementation for tests), `View`/`ViewDesc`, `Presenter`, `Module`.
- **Done when:** everything builds on all three compilers. A ctest check (`cmake/CheckIncludeRules.cmake`) fails if `core` or `terrain` includes Vulkan or SDL headers, or if `render` and `net` include each other's headers.

## Step 4: Windows on every monitor (Windows hardware, done)

- `libs/platform` with SDL3: list monitors with their bounds, create one borderless window per channel on its configured monitor, pump events on the main thread, and handle Esc to quit.
- Fail at startup with a clear message when a channel's monitor index doesn't exist.
- **Done when:** three windows open on three monitors with nothing drawn. CI only builds the code, and a manual check is recorded in the pull request.
- **Result (2026-11-05):** on the development desk (Dell P2724DE 2560×1440 landscape, two Samsung LF24T35 1080×1920 portrait; `config/channels.landscape-two-portrait.toml`), Windows reported each Fovea window exactly covering its monitor, borderless and visible. `fovea --identify` showed red, green and blue on the left, middle and right monitors as configured. Empty windows look like nothing changed on Windows (the old screen contents stay), hence `--identify`.

## Step 5: Vulkan bring-up (done)

- Vulkan 1.3 through volk and vk-bootstrap, with VMA for memory and validation layers in debug builds. The dynamic rendering and synchronization2 features are required. Descriptor indexing is turned on now and used in step 6.
- `MultiSwapchainPresenter`: one surface and swapchain per channel, with the present mode from config. It handles window resize and minimize, and recreates swapchains when they go out of date.
- `OffscreenPresenter`: renders to images, used for headless runs and CI.
- Clear each output to a different color.
- **Done when:** three windows each show their color with no validation errors. Headless mode runs on lavapipe in CI and checks the cleared pixel values.
- **Result (2026-11-06):** on the RTX 5070 Ti with validation required, the three windows ran 604 frames in 10 s (FIFO, about 60 Hz) with 0 validation messages, all presented in one `vkQueuePresentKHR`. Offscreen clears read back exactly, on the NVIDIA card and on lavapipe. All three monitors are wired to the NVIDIA card (their PnP parent is it; the AMD iGPU has no active output), so there's no cross-adapter copy. On-screen colours still need checking with `tools/check-identify.ps1` on an unlocked desktop.
- **Note:** `Renderer` owns its presenters for now (`libs/render/src/vk`). The public `Presenter` interface is the shape to converge on in step 7, when the frame loop separates drawing from presenting.

## Step 6: Reference scene

- Shaders written in Slang and compiled by `slangc` as a build step (D-003). The SPIR-V files are build outputs and aren't committed.
- Each view uses a camera-relative, reverse-Z projection built from its channel's config. A flat ground grid lies at a configurable eye height, with a horizon line and a sky gradient. The eyepoint is fixed, since there's no CIGI until milestone 2.
- Linear HDR render target per view, followed by a tone-map pass into the output (foundational decision 3).
- **Done when:** headless golden-image tests on lavapipe, one image per channel, compare within a tolerance. On real hardware, a manual check confirms the horizon and grid lines are continuous across the three screens.

## Step 7: Frame loop and stats

- A render thread on a fixed 60 Hz clock with two frames in flight. Once per frame it takes the latest SceneState, records every view, submits, and presents back to back.
- Stats: CPU frame time, GPU time per view (timestamp queries), present interval, and missed frames (an interval more than 1.5× the period). Present timing comes from `VK_KHR_present_wait`/`present_id` when available.
- Stats go to a CSV log file through the lock-free log. The Dear ImGui overlay on the debug channel shows the frame time and a missed-frame graph. D-006 must be accepted first.
- A debug-build check that the render loop allocates nothing after warm-up.
- **Done when:** a 10-minute run on the Windows machine shows no missed frames in the CSV, and the zero-allocation check passes.

## Step 8: Headless option and the display sync test

- `fovea --headless --frames N --dump <dir>` writes each channel's image as PNG.
- `docs/display-sync-test.md`: a procedure in which a flashing marker, drawn on all three channels in the same frame, is filmed with a high-speed camera. It compares three setups: FIFO on every output, FIFO on one output with the others on MAILBOX, and the driver's display spanning.
- **Done when:** the procedure has been run on the real displays and its results are recorded as a decision in `decisions.md`.

## Inputs needed from you

| Needed by | Input |
|---|---|
| Step 1 | ~~Approve D-010 (math library)~~ glm, accepted |
| Step 4 | Visual Studio 2022 and the Vulkan SDK installed on Windows; the three monitors connected |
| Step 6 | Physical screen geometry and the eye-point position, which replace the placeholder frustum values (they can come later) |
| Step 7 | Approve D-006 (Dear ImGui) |
| Step 8 | A high-speed camera (a phone's slow-motion mode is probably enough to start); the GPU model and display resolution |
| Before going public | Approve D-004 (license) |
