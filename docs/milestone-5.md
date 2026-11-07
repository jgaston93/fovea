# Milestone 5: Anti-aliasing

The goal from `fovea-plan.md`: anti-aliasing on every view, chosen and measured against the frame budget. Without it, runway edges, building edges, the tower's antenna, thin roads and the horizon crawl as the eye moves, on every display; it comes before the environment because everything after it is judged by eye.

As before, each step is its own pull request. Steps 1 and 2 run in WSL and CI (lavapipe supports 4x multisampling); step 3 needs the desk.

## Where things stand (checked 2026-11-07)

- Each view draws one pass into a 16-bit floating-point HDR target (`R16G16B16A16_SFLOAT`) with a 32-bit reverse-Z depth buffer: the sky (a fullscreen triangle), then the terrain, then the models. A second pass tone-maps it onto the output.
- The desk's views are 2560 x 1440 and two of 1080 x 1920. GPU time is 1.0 to 2.3 ms an output at most (milestone 4 step 7), against a 16.7 ms frame: there is room.
- Texture aliasing (terrain imagery, model textures shrinking with distance) is already handled by full mip chains and anisotropic filtering. What's left is geometry: edges, and things thinner than a pixel.

## Decisions (proposed, in `decisions.md`)

- **D-027 Multisampling first.** The scene pass renders with multisampling (MSAA), 4 samples by default, the count set per deployment in config (`[display] msaa = 1, 2, 4 or 8`, as the GPU allows). It smooths every geometric edge and thin geometry seen from any angle, in every view alike, with no history: nothing smears when the eye moves fast, and the channels stay consistent with each other. The alternatives: temporal anti-aliasing (TAA) also smooths shading and is cheaper per sample, but blends earlier frames, so it needs motion vectors for every moving object (entities, milestone 9) and blurs or ghosts under the fast, steady motion of a simulator, which is the case that matters most here; post-process filters (FXAA, SMAA) are cheapest but only guess at edges from the final image, and crawl on thin geometry. MSAA's cost is memory (at 4 samples, about 180 MB for the 2560 x 1440 view and 100 MB for each portrait one) and bandwidth; the desk measures it. TAA or SMAA can be added later, as an option, if shading aliasing shows.
- **D-028 The samples are resolved before tone mapping, weighted.** An HDR pixel's samples are averaged in linear light, but each weighted by 1 / (1 + its luminance), and the result unweighted again: a plain average lets one very bright sample (the sun's glint, a light at night) swamp an edge, so it stays jagged after tone mapping, where the weighting keeps the edge's gradient. A resolve shader of Fovea's own does this, before the tone mapping pass, which stays as it is.
- Alpha-masked materials (D-024: cut-outs in models, and later vegetation, milestone 7) use alpha to coverage under multisampling, so their edges are smoothed too rather than cut pixel by pixel.

## Step 1: Multisampled scene pass

- The scene pass renders into multisampled colour and depth targets; the weighted resolve (D-028) writes the single-sampled HDR target the tone mapping pass reads. `[display] msaa` in config (default 4; 1 is the pass as now); a count the GPU can't do is refused at start-up, with the counts it can. The sky, terrain and model pipelines take the sample count.
- **Done when:** a GPU test draws a known edge (a model's side against the sky) and finds pixels along it between the two colours, in proportion to their cover, where `msaa = 1` gives only the two; the weighted resolve keeps a bright sample from swamping its pixel (a unit test of the resolve on made-up samples); the golden images are regenerated at 4 samples on lavapipe and looked at (their edges smooth, nothing else changed); no validation messages; drawing still allocates nothing.

## Step 2: Alpha to coverage

- Masked materials draw with alpha to coverage when multisampled; their cut-out edges come out smooth. Blend materials stay drawn opaque for now (D-024's models have none).
- **Done when:** a GPU test of a masked model shows partial coverage along its cut-out edge at 4 samples, and a hard edge at 1.

## Step 3: Hardware check

- On the desk: GPU time and memory at 1, 2, 4 and 8 samples on the KHSV and KHUA routes, the default chosen from them (4 unless the cost says otherwise), and the owner's look at crawling edges (runway edges and markings, buildings, the tower's antenna, the horizon) against `msaa = 1`. Results recorded; anything unexpected becomes an issue.

## Inputs needed from you

| Needed by | Input |
|---|---|
| Plan | Approve D-027 (multisampling first, 4 by default) and D-028 (a weighted resolve before tone mapping) |
| Step 3 | Time at the desk: comparing edges with and without |
