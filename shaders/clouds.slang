// The cloud layers (milestone 6 step 6, D-034): drawn after the resolve
// and the stars into the HDR target, one full-screen triangle a layer, the
// farthest first, tested against the resolved scene's depth so terrain
// nearer than a cloud shows in front. Two models, chosen by config
// ([display] clouds): fs_clouds draws each layer as a surface at its base;
// fs_clouds_volume marches each ray through the layer, base to top. Both
// shape it from cloud_noise.slang's volumes (Perlin-Worley shapes, Worley
// detail eroding their edges), thresholded where the layer's coverage puts
// it, and both light it by the sun, moon and sky at the eye, then show it
// through the air and the fog.

#define FRAME_SET 0
#include "include/frame.slang"

// The noise (cloud_noise.slang): the shape volume, tiling every kShapeCells
// of the weather's noise cells (12 km), and the detail volume, every half
// cell (1.5 km); sampled linearly, repeating. And the shape's quantiles.
[[vk::binding(0, 1)]] Sampler3D<float4> cloud_shape;
[[vk::binding(1, 1)]] Sampler3D<float4> cloud_detail;
[[vk::binding(2, 1)]] StructuredBuffer<uint> cloud_table;
static const float kDetailCells = 0.5;
static const float kShapeSize = 128.0;

// Mirrors CloudPush in clouds.cpp.
struct CloudPush {
    float4x4 clip_to_rel;  // the view's clip space to points relative to the eye (ECEF)
    float4x4 rel_to_clip;  // and back (SceneView's ecef_to_clip)
};
[[vk::push_constant]] ConstantBuffer<CloudPush> push;

struct CloudPixel {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 ndc;
    [[vk::location(1)]] nointerpolation uint layer;
};

struct CloudOut {
    float4 colour : SV_Target;
    float depth : SV_Depth;
};

[shader("vertex")]
CloudPixel vs_clouds(uint id: SV_VulkanVertexID, uint instance: SV_VulkanInstanceID) {
    // One triangle over the screen.
    const float2 ndc = float2(float((id << 1) & 2), float(id & 2)) * 2.0 - 1.0;
    CloudPixel o;
    o.position = float4(ndc, 0.0, 1.0);
    o.ndc = ndc;
    o.layer = instance;
    return o;
}

// A layer's density at a point (its noise cells, as cloud_cells gives
// them), 0 to 1: the shape above the coverage's quantile, so a share
// `coverage` of the layer is cloud, rising toward each cloud's core; its
// edges eroded by the detail. `lod` the shape volume's mip by the pixel's
// footprint: as it coarsens, the density tends to its mean, so distant
// broken cloud reads as the thin layer it averages to.
float shape_density(float coverage, float3 cells, float lod, bool erode, float widen) {
    if (coverage <= 0.0) {
        return 0.0;
    }
    const float base = cloud_shape.SampleLevel(cells / kShapeCells, lod).r;
    float d = cover_density(cloud_table, coverage, base, widen);
    if (erode) {
        // Two octaves of Worley, the finer one weaker: broken, wispy edges.
        const float4 e = cloud_detail.SampleLevel(cells / kDetailCells, max(lod - 2.0, 0.0));
        const float4 f =
            cloud_detail.SampleLevel(cells / (kDetailCells * 0.25), max(lod - 4.0, 0.0));
        const float erosion = 0.45 * e.r + 0.3 * f.g;
        d = saturate((d - erosion) / (1.0 - erosion));
    }
    // About the mean density of a layer of this coverage.
    return lerp(d, coverage * 0.35, saturate((lod - 2.0) / 3.0));
}

// The shape volume's mip for a pixel spanning `footprint` noise cells.
float shape_lod(float footprint) {
    return max(log2(max(footprint / kShapeCells * kShapeSize, 1e-6)), 0.0);
}

// The layer's base where the ray (from the eye, direction d, zenith cosine
// mu) meets it: from below, the far root ahead; from above, the near one
// where the ray comes down to it. x the distance, y 1 if it does.
float2 base_hit(float4 l, float mu) {
    const float2 span = sphere_span(lighting.eye_height_m, mu, l.x);
    const float dist = lighting.eye_height_m < l.x ? span.y : span.x;
    return float2(dist, span.x <= span.y && dist > 0.0 ? 1.0 : 0.0);
}

// The light on a cloud's top, as on level ground there (up its up).
float3 light_on_top(ViewSky s, float3 up) {
    return s.sun_irradiance * saturate(dot(lighting.sun, up)) +
           s.moon_irradiance * saturate(dot(lighting.moon, up)) + s.sky_irradiance;
}

// A cloud's light (premultiplied: radiance times alpha, before exposure)
// and alpha, `dist` metres along a ray of zenith cosine mu, through the air
// and the fog to the eye, exposed; and its depth.
CloudOut finish(float4 position, float3 lit, float alpha, float dist, float3 rel, float mu,
                ViewSky s) {
    const float4 air = aerial_perspective(position.xy, dist * 0.001);
    float3 seen = lit * air.a + air_light(air, s) * alpha;
    if (lighting.layer_count > 0) {
        const float t = band_transmittance(mu, dist);
        seen = seen * t + band_radiance(s) * alpha * (1.0 - t);
    }
    CloudOut o;
    o.colour = float4(min(seen * s.exposure, float3(60000.0)), alpha);
    const float4 clip = mul(push.rel_to_clip, float4(rel, 1.0));
    o.depth = saturate(clip.z / clip.w);
    return o;
}

// Opaque where it lets less than a thousandth through: NVIDIA blends a
// half-float target in half floats, where 1 - alpha can't go below about
// 5e-4, and the sun's disc behind (some 60,000) then shone through even a
// full overcast. A thinner cloud dims the disc, as cloud does.
float opaque_alpha(float transmittance) {
    return transmittance < 1e-3 ? 1.0 : 1.0 - transmittance;
}

// Henyey-Greenstein's phase function.
float henyey_greenstein(float g, float c) {
    return (1.0 - g * g) / (4.0 * kPi * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5));
}

// The layer as a surface at its base.
[shader("fragment")]
CloudOut fs_clouds(CloudPixel i) {
    const float4 l = lighting.layer[i.layer];
    // Any depth gives the direction; 1 is the near plane (reverse-Z).
    const float4 p = mul(push.clip_to_rel, float4(i.ndc, 1.0, 1.0));
    const float3 d = normalize(p.xyz / p.w);
    const float mu = dot(d, lighting.up);
    const float2 hit = base_hit(l, mu);
    // Its noise cells, and how many a pixel spans: taken for every pixel,
    // outside the branch (a pixel that misses gives its neighbours nothing
    // useful, which only coarsens their detail).
    const float3 rel = d * (hit.y > 0.0 ? hit.x : 0.0);
    const float3 cells = cloud_cells(rel, i.layer);
    const float footprint = max(length(ddx(cells)), length(ddy(cells)));
    if (hit.y == 0.0) {
        discard;
    }
    const float lod = shape_lod(footprint);
    const float density = shape_density(l.w, cells, lod, true, kLayerWiden);
    // Its depth mottled by the detail's finer octave, so a base isn't one
    // flat grey.
    const float mottle =
        cloud_detail.SampleLevel(cells / (kDetailCells / 3.0), max(lod - 1.0, 0.0)).g;
    const float tau = cloud_depth(l) * density * lerp(0.6, 1.4, mottle);
    const float alpha = opaque_alpha(exp(-tau));
    if (alpha < 1e-3) {
        discard;
    }
    const ViewSky s = view_sky[lighting.view];
    float3 up;
    (void)point_height(rel, up);
    const bool below = lighting.eye_height_m < l.x;
    // The light, the sun's or the moon's, and the cloud toward it across
    // the layer, 300 m on: a high light reaches a part through the cloud's
    // own depth, a low one through the cloud on its side, so the edges
    // that face it are bright and the far ones grey.
    const bool by_moon = lighting.shadow_light == 2;
    const float3 to_light = by_moon ? lighting.moon : lighting.sun;
    const float3 light = by_moon ? s.moon_irradiance : s.sun_irradiance;
    const float light_up = saturate(dot(to_light, up));
    const float3 across = to_light - up * dot(to_light, up);
    const float across_len = length(across);
    const float3 toward = across_len > 1e-3 ? across / across_len * 300.0 : float3(0.0);
    const float ahead = shape_density(l.w, cloud_cells(rel + toward, i.layer), lod + 1.0, false, kLayerWiden);
    // Shaded by the cloud around a part (its density four mips coarser, a
    // kilometre or so, mottled), not by the part alone: an edge is thin,
    // and lit by its own depth every edge shone as a rim.
    const float body = shape_density(l.w, cells, lod + 4.0, false, kLayerWiden);
    const float tau_body = cloud_depth(l) * max(body, density) * lerp(0.5, 1.5, mottle);
    const float tau_light = lerp(cloud_depth(l) * max(ahead, body), tau_body, light_up);
    float3 colour;  // its radiance, before alpha
    if (below) {
        // What the cloud lets down, scattered: of the light, through its
        // depth toward it (half goes down); of the sky, through its own.
        // Thicker parts are darker, edges facing the light brightest.
        colour = (light * 0.5 * cloud_diffuse(tau_light) +
                  s.sky_irradiance * cloud_diffuse(tau_body)) *
                 kInvPi;
    } else {
        // What it reflects up: more where it is thick, its sides facing the
        // light lit more than those away from it.
        const float reflect = 1.0 - cloud_diffuse(tau_body);
        const float facing = lerp(0.7, 1.15, saturate(0.5 + (density - ahead) * 2.0));
        colour = (light * light_up * facing + s.sky_irradiance) * reflect * kInvPi;
    }
    float3 lit = colour * alpha;
    // Seen from below toward the sun, its thin edges glow with the light
    // scattered forward through them (single scattering, optical depth
    // tau: at most where tau is 1): only near the sun (a narrow lobe), and
    // only at the cloud's true edges (where its body is thin), else every
    // edge shone as a rim.
    if (below) {
        lit += light * henyey_greenstein(0.85, dot(d, to_light)) * tau * exp(-tau) *
               saturate(1.0 - body * 2.0);
    }
    return finish(i.position, lit, alpha, hit.x, rel, mu, s);
}

// How a cloud thins toward its base and rounds toward its top, by its
// height through the layer (0 base, 1 top), its own top at `top` (a share
// of the layer's thickness).
float height_shape(float y, float top) {
    return saturate(y / 0.1) * saturate((top - y) / (0.35 * top));
}

// How high a cloud grows, a share of the layer's thickness, by the column
// it stands in (`rel` any point in it, `y` its height through the layer):
// its core's density there (a big, dense cloud reaches the layer's top, a
// small one stops a third of the way), and a broad noise, 48 km a tile, so
// clouds of a size differ too. Cumulus share their base, where the air
// condenses, but not their tops.
float cloud_top(float4 l, float3 rel, float3 up, float y, uint layer, float lod) {
    const float3 base = rel - up * (y * (l.y - l.x));
    const float3 cells = cloud_cells(base, layer);
    const float core = shape_density(l.w, cells, lod + 2.0, false, 1.1);
    const float broad = cloud_shape.SampleLevel(cells / (kShapeCells * 4.0), lod).g;
    return lerp(0.3, 1.0, saturate(core * 1.3)) * lerp(0.55, 1.0, saturate(broad * 1.5 - 0.25));
}

static const uint kVolumeSteps = 48;
static const float kVolumeReachM = 40000.0;  // the farthest a ray marches in a layer
static const float kCloudExtinction = 0.04;  // per metre at density 1 (renderer.cpp's)

// The layer as a volume: the ray marched from where it enters the layer to
// where it leaves (or kVolumeReachM on), kVolumeSteps steps, each lit by
// the light it gets through the cloud toward the sun or moon (four steps
// that way) and by the sky, scattered with a forward and a back lobe;
// integrated as Hillaire's (2016) energy-conserving step.
[shader("fragment")]
CloudOut fs_clouds_volume(CloudPixel i) {
    const float4 l = lighting.layer[i.layer];
    const float4 p = mul(push.clip_to_rel, float4(i.ndc, 1.0, 1.0));
    const float3 d = normalize(p.xyz / p.w);
    const float mu = dot(d, lighting.up);
    const float h = lighting.eye_height_m;
    // Where the ray is within the layer: [t0, t1].
    const float2 bottom = sphere_span(h, mu, l.x);
    const float2 top = sphere_span(h, mu, l.y);
    const bool meets_bottom = bottom.x <= bottom.y;
    float t0;
    float t1;
    if (h < l.x) {
        t0 = bottom.y;
        t1 = top.y;
    } else if (h > l.y) {
        t0 = top.x;
        t1 = meets_bottom && bottom.x > 0.0 ? bottom.x : top.y;
        if (top.x > top.y || top.y < 0.0) {
            t1 = -1.0;
        }
    } else {
        t0 = 0.0;
        t1 = meets_bottom && bottom.x > 0.0 ? bottom.x : top.y;
    }
    t0 = max(t0, 0.0);
    t1 = min(t1, t0 + kVolumeReachM);
    // The detail a pixel resolves where the ray enters, for every pixel
    // (outside the branch).
    const float3 enter = d * (t1 > t0 ? t0 : 0.0);
    const float3 enter_cells = cloud_cells(enter, i.layer);
    const float footprint = max(length(ddx(enter_cells)), length(ddy(enter_cells)));
    if (t1 <= t0) {
        discard;
    }
    const ViewSky s = view_sky[lighting.view];
    const bool by_moon = lighting.shadow_light == 2;
    const float3 to_light = by_moon ? lighting.moon : lighting.sun;
    const float3 light = by_moon ? s.moon_irradiance : s.sun_irradiance;
    const float c = dot(d, to_light);
    const float phase = lerp(henyey_greenstein(0.8, c), henyey_greenstein(-0.3, c), 0.4);
    // Steps staggered by pixel (interleaved gradient noise) so their bands
    // dissolve into a fine grain.
    const float jitter =
        frac(52.9829189 * frac(dot(i.position.xy, float2(0.06711056, 0.00583715))));
    const float dt = (t1 - t0) / float(kVolumeSteps);
    const float thickness = l.y - l.x;
    float transmittance = 1.0;
    float3 lit = float3(0.0);
    float first = -1.0;
    for (uint k = 0; k < kVolumeSteps; ++k) {
        const float t = t0 + (float(k) + jitter) * dt;
        const float3 rel = d * t;
        // The step's distance coarsens its detail, from the entry's.
        const float lod = shape_lod(footprint * max(t / max(t0, 1.0), 1.0));
        float3 up;
        const float y = (point_height(rel, up) - l.x) / thickness;
        const float top = cloud_top(l, rel, up, y, i.layer, lod);
        if (y > top) {
            continue;
        }
        const float density = shape_density(l.w, cloud_cells(rel, i.layer), lod, true, 1.1) *
                              height_shape(y, top);
        if (density <= 1e-3) {
            continue;
        }
        if (first < 0.0) {
            first = t;
        }
        // Toward the light: four steps, growing, through the shape alone.
        float toward = 0.0;
        float along = 0.0;
        for (uint j = 0; j < 4; ++j) {
            const float step = 30.0 * float(1u << j);
            along += step;
            const float3 q = rel + to_light * along;
            float3 q_up;
            const float qy = (point_height(q, q_up) - l.x) / thickness;
            toward += shape_density(l.w, cloud_cells(q, i.layer), lod + 1.0, false, 1.1) *
                      height_shape(qy, top) * step;
        }
        const float od = toward * kCloudExtinction;
        // Beer's law, with a softer tail standing in for the light scattered
        // many times (it reaches deeper than the direct beam).
        const float through = max(exp(-od), 0.7 * exp(-0.25 * od));
        // The light from around: the clear sky's in the gaps, and in a
        // layer that covers the sky, the grey light the cloud itself
        // scatters (the clear sky's blue lit an overcast's base dark and
        // blue); less toward the cloud's base.
        const float3 around = lerp(s.sky_irradiance, light_on_top(s, up) * 0.5, l.w);
        const float3 ambient = around * kInvPi * (0.5 + 0.5 * saturate(y));
        const float sigma = kCloudExtinction * density;
        const float3 source = sigma * (light * phase * through + ambient);
        const float step_through = exp(-sigma * dt);
        lit += transmittance * source * (1.0 - step_through) / sigma;
        transmittance *= step_through;
        if (transmittance < 1e-3) {
            transmittance = 0.0;
            break;
        }
    }
    if (first < 0.0) {
        discard;
    }
    const float alpha = opaque_alpha(transmittance);
    // Its light, as the blend takes it: what it scattered, alpha's worth.
    return finish(i.position, lit * (alpha / max(1.0 - transmittance, 1e-6)), alpha, first,
                  d * first, mu, s);
}
