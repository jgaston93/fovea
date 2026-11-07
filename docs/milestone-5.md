# Milestone 5: Anti-aliasing

The goal from `fovea-plan.md`: anti-aliasing on every view, chosen and measured against the frame budget. Without it, runway edges, building edges, the tower's antenna, thin roads and the horizon crawl as the eye moves, on every display; it comes before the environment because everything after it is judged by eye.

As before, each step is its own pull request. Steps 1 and 2 run in WSL and CI (lavapipe supports 4x multisampling); step 3 needs the desk.

## Where things stand (checked 2026-11-07)

- Each view draws one pass into a 16-bit floating-point HDR target (`R16G16B16A16_SFLOAT`) with a 32-bit reverse-Z depth buffer: the sky (a fullscreen triangle), then the terrain, then the models. A second pass tone-maps it onto the output.
- The desk's views render at 2560 x 1440 and two of 1080 x 1920 (render resolution, which the architecture keeps independent of output resolution). GPU time is 1.0 to 2.3 ms an output at most (milestone 4 step 7), against a 16.7 ms frame: there is room.
- Texture aliasing (terrain imagery, model textures shrinking with distance) is already handled by full mip chains and anisotropic filtering. What's left is geometry: edges, and things thinner than a pixel.

## Decisions (proposed, in `decisions.md`)

- **D-027 Multisampling first.** The scene pass renders with multisampling (MSAA), 4 samples by default, the count set per deployment in config (`[display] msaa = 1, 2, 4 or 8`, as the GPU allows). It smooths every geometric edge and thin geometry seen from any angle, in every view alike, with no history: nothing smears when the eye moves fast, and the channels stay consistent with each other. The alternatives: temporal anti-aliasing (TAA) also smooths shading and is cheaper per sample, but blends earlier frames, so it needs motion vectors for every moving object (entities, milestone 9) and blurs or ghosts under the fast, steady motion of a simulator, which is the case that matters most here; post-process filters (FXAA, SMAA) are cheapest but only guess at edges from the final image, and crawl on thin geometry. MSAA's cost is memory and bandwidth: at 4 samples, 48 bytes a rendered pixel for colour and depth (about 180 MB for the 2560 x 1440 view and 100 MB for each portrait one at the desk's render resolution; more where a deployment renders above its output), plus the single-sampled targets the resolve writes; the desk measures it. TAA or SMAA can be added later, as an option, if shading aliasing shows.
  Alpha-masked materials (models' cut-outs, milestone 4 step 3, and later vegetation, milestone 7) use alpha to coverage under multisampling, so their edges are smoothed too rather than cut pixel by pixel.
- **D-028 The samples are resolved before tone mapping, weighted.** Each pixel's colour is the samples' weighted mean in linear HDR, c = sum(c_i w_i) / sum(w_i) with w_i = 1 / (1 + L_i), L_i the sample's luminance (the weights are never zero, so neither is their sum): a plain average lets one very bright sample (the sun's glint off a surface) swamp an edge, so it stays jagged after tone mapping, where the weighting keeps the edge's gradient. A resolve shader of Fovea's own does this, before the tone mapping pass, which stays as it is. Point lights (airport lights, milestone 6) are not drawn into the multisampled scene: a distant light covers one sample of four, which the weighting would all but erase, and it would flicker as it crossed sample positions; they are drawn after the resolve, single-sampled, at least a pixel across. The depth is resolved too (the sample nearest the eye where the GPU can, else the first), so later passes (haze, milestone 6) read a single-sampled depth.

## Step 1: Multisampled scene pass

- The scene pass renders into multisampled colour and depth targets; the weighted resolve (D-028) writes the single-sampled HDR target the tone mapping pass reads, and the depth is resolved beside it. `[display] msaa` in config (default 4; 1 is the pass as now); a count the GPU can't do for both the HDR colour format and the depth format (`framebufferColorSampleCounts` and `framebufferDepthSampleCounts`, and the formats' own limits) is refused at start-up, with the counts it can. The sky, terrain and model pipelines take the sample count.
- **Done when:** a GPU test draws a known edge (a model's side against the sky) and finds pixels along it between the two colours, in proportion to their cover, where `msaa = 1` gives only the two; the resolve keeps a bright sample from swamping its pixel and never divides by zero (a unit test on made-up samples, a luminance of 1e6 included); the golden images are regenerated at 4 samples on lavapipe and looked at (their edges smooth, nothing else changed), and NVIDIA's are compared with them as the terrain golden is (at most 1% of pixels past a difference of 4, edges being where GPUs' sample positions differ; the measured figure recorded); no validation messages; drawing still allocates nothing.

## Step 2: Alpha to coverage

- Masked materials draw with alpha to coverage when multisampled: the fragment shader then doesn't discard below the cutoff (which would cut every edge hard), but sharpens alpha about the material's own cutoff, a' = (a - cutoff) / max(fwidth(a), 1e-4) + 0.5, so the cut-out falls where its author put it and its edge spans about a pixel, on any GPU; single-sampled, it discards as now. Blend materials stay drawn opaque for now (milestone 4 step 3; the landmarks have none).
- **Done when:** a GPU test of a masked model shows partial coverage along its cut-out edge at 4 samples, a hard edge at 1, and the edge where the cutoff puts it (within a pixel) either way.

## Step 3: Hardware check

- On the desk: GPU time and memory at 1, 2, 4 and 8 samples on the KHSV and KHUA routes, the default chosen from them (4 unless the cost says otherwise), and the owner's look at crawling edges (runway edges and markings, buildings, the tower's antenna, the horizon) against `msaa = 1`. Results recorded; anything unexpected becomes an issue.

## Inputs needed from you

| Needed by | Input |
|---|---|
| Plan | Approve D-027 (multisampling first, 4 by default) and D-028 (a weighted resolve before tone mapping) |
| Step 3 | Time at the desk: comparing edges with and without |
