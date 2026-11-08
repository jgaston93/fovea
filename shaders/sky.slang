// The sky's tables and each view's light (milestone 6 step 2, D-030, D-031):
// compute passes before the scene. The transmittance and multiple-
// scattering tables are made when the atmosphere changes; each frame, each
// view's sky-view table, then its irradiance, then the exposure all views
// share.

#include "include/atmosphere.slang"
#include "include/frame.slang"

[[vk::binding(0, 0)]] ConstantBuffer<Atmosphere> atmosphere;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> transmittance_out;
[[vk::binding(2, 0)]] Sampler2D<float4> transmittance;
[[vk::binding(3, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> multi_scattering_out;
[[vk::binding(4, 0)]] Sampler2D<float4> multi_scattering;
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> sky_view_out;
[[vk::binding(6, 0)]] RWStructuredBuffer<ViewSky> view_sky_out;
[[vk::binding(8, 0)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> aerial_out;
// [0]: the exposure carried between frames, in stops (its log2: a settled
// exposure then stays exactly the same), [1]: 1 once it has one.
[[vk::binding(7, 0)]] RWStructuredBuffer<float> exposure_state;

// Mirrors SkyPush in sky.cpp.
struct SkyPush {
    float eye_radius;  // km from the Earth's centre: the bottom radius plus the eye's height
    float mu_s;        // the sun's zenith cosine at the eye
    float sun_up;      // how much of the sun's disc is above the eye's horizon
    uint view;
    uint views;
    float adapt;  // how far exposure moves to its target this frame, 0 to 1
    float key;    // exposure times a level surface's illuminance
    float floor_illuminance;  // exposure stops rising below this (night)
    // For the aerial-perspective volume: the view's clip space to its level
    // frame (x north, y east, z down), and the sun in that frame.
    float4x4 clip_to_local;
    float3 sun_local;
    // The moon (step 4): its illuminance over the sun's, times how much of
    // its disc is above the horizon, and its direction in the level frame.
    float moon_scale;
    float3 moon_local;
    uint source;  // the sky-view pass: 0 lit by the sun, 1 by the moon
};

// A light's zenith cosine at the eye, from its direction in the level frame.
float zenith_cos(float3 local) {
    return -local.z;
}

[[vk::push_constant]] ConstantBuffer<SkyPush> push;

float3 transmittance_to_top(float r, float mu) {
    return transmittance.SampleLevel(transmittance_uv(atmosphere, r, mu), 0.0).rgb;
}

// The sun's transmittance from a point, zero where the Earth hides it.
float3 sun_transmittance(float r, float mu_s) {
    if (hits_ground(r, mu_s, atmosphere.bottom_radius)) {
        return float3(0.0);
    }
    return transmittance_to_top(r, mu_s);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_transmittance(uint3 id: SV_DispatchThreadID) {
    if (any(float2(id.xy) >= kTransmittanceSize)) {
        return;
    }
    const float2 r_mu = transmittance_r_mu(atmosphere, (float2(id.xy) + 0.5) / kTransmittanceSize);
    const float r = r_mu.x;
    const float mu = r_mu.y;
    const float path = max(distance_to_sphere(r, mu, atmosphere.top_radius), 0.0);
    const int steps = 160;
    const float dt = path / steps;
    float3 depth = float3(0.0);
    for (int i = 0; i < steps; ++i) {
        const float t = (i + 0.5) * dt;
        const float ri = sqrt(r * r + t * t + 2.0 * r * mu * t);
        depth += medium_at(atmosphere, ri - atmosphere.bottom_radius).extinction * dt;
    }
    transmittance_out[id.xy] = float4(exp(-depth), 1.0);
}

// Hillaire's multiple scattering: for each altitude and sun angle, the
// light scattered twice or more, as a geometric series of the second
// order's fraction (f_ms), all isotropic, per unit of sun illuminance.
groupshared float3 ms_light[64];
groupshared float3 ms_fraction[64];

[shader("compute")]
[numthreads(1, 1, 64)]
void cs_multi_scattering(uint3 id: SV_DispatchThreadID) {
    const float2 uv = (float2(id.xy) + 0.5) / kMultiScatteringSize;
    const float mu_s = texel_to_unit(uv.x, kMultiScatteringSize.x) * 2.0 - 1.0;
    const float r = atmosphere.bottom_radius +
                    texel_to_unit(uv.y, kMultiScatteringSize.y) *
                        (atmosphere.top_radius - atmosphere.bottom_radius);
    const float3 sun = float3(sqrt(saturate(1.0 - mu_s * mu_s)), mu_s, 0.0);
    // Four directions of 256 spread over the sphere (16 x 16, equal areas)
    // per thread.
    const uint k = id.z;
    const float uniform_phase = 1.0 / (4.0 * kPi);
    float3 light = float3(0.0);
    float3 fraction = float3(0.0);
    for (uint q = 0; q < 4; ++q) {
        const uint n = k * 4 + q;
        const float cos_t = 1.0 - 2.0 * ((n / 16) + 0.5) / 16.0;
        const float phi = 2.0 * kPi * ((n % 16) + 0.5) / 16.0;
        const float sin_t = sqrt(saturate(1.0 - cos_t * cos_t));
        const float3 dir = float3(sin_t * cos(phi), cos_t, sin_t * sin(phi));
        const float mu = dir.y;
        const float ground = distance_to_ground(r, mu, atmosphere.bottom_radius);
        const float path =
            ground >= 0.0 ? ground : max(distance_to_sphere(r, mu, atmosphere.top_radius), 0.0);
        const int steps = 40;
        const float dt = path / steps;
        float3 through = float3(1.0);
        for (int i = 0; i < steps; ++i) {
            const float t = (i + 0.5) * dt;
            const float3 p = float3(0.0, r, 0.0) + dir * t;
            const float ri = length(p);
            const Medium m = medium_at(atmosphere, ri - atmosphere.bottom_radius);
            const float3 step_t = exp(-m.extinction * dt);
            const float3 sun_t = sun_transmittance(ri, dot(p, sun) / ri);
            const float3 s = m.scattering * uniform_phase * sun_t;
            const float3 safe = max(m.extinction, 1e-9);
            light += through * (s - s * step_t) / safe;
            fraction += through * (m.scattering - m.scattering * step_t) / safe;
            through *= step_t;
        }
        if (ground >= 0.0) {
            // The ground's bounce, lit by the sun.
            const float3 p = float3(0.0, r, 0.0) + dir * ground;
            const float mu_g = dot(normalize(p), sun);
            light += through * sun_transmittance(atmosphere.bottom_radius, mu_g) * saturate(mu_g) *
                     atmosphere.ground_albedo / kPi;
        }
    }
    ms_light[k] = light / 256.0;
    ms_fraction[k] = fraction / 256.0;
    GroupMemoryBarrierWithGroupSync();
    if (k == 0) {
        float3 l = float3(0.0);
        float3 f = float3(0.0);
        for (uint j = 0; j < 64; ++j) {
            l += ms_light[j];
            f += ms_fraction[j];
        }
        multi_scattering_out[id.xy] = float4(l / (1.0 - f), 1.0);
    }
}

// The sky seen from the eye in every direction, for this frame's sun:
// the sun's disc left out, the ground below the horizon lit by the sun.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_sky_view(uint3 id: SV_DispatchThreadID) {
    if (any(float2(id.xy) >= kSkyViewSize)) {
        return;
    }
    const float r = push.eye_radius;
    const float2 angles = sky_view_angles(atmosphere, r, (float2(id.xy) + 0.5) / kSkyViewSize);
    const float zenith = angles.x;
    const float azimuth = angles.y;
    const float3 dir = float3(sin(zenith) * cos(azimuth), cos(zenith), sin(zenith) * sin(azimuth));
    // The light this layer is lit by, at azimuth 0 of its table: the sun,
    // or (step 4) the moon, scaled by its illuminance.
    const float mu_l = push.source == 0 ? push.mu_s : zenith_cos(push.moon_local);
    const float scale = push.source == 0 ? 1.0 : push.moon_scale;
    const float3 sun = float3(sqrt(saturate(1.0 - mu_l * mu_l)), mu_l, 0.0);
    const float mu = dir.y;
    const float ground = distance_to_ground(r, mu, atmosphere.bottom_radius);
    const float top = distance_to_sphere(r, mu, atmosphere.top_radius);
    const float path = ground >= 0.0 ? ground : max(top, 0.0);
    const float c = dot(dir, sun);
    const float pr = rayleigh_phase(c);
    const float pm = mie_phase(atmosphere.mie_g, c);
    const int steps = 32;
    float3 through = float3(1.0);
    float3 light = float3(0.0);
    float t_prev = 0.0;
    for (int i = 0; i < steps; ++i) {
        // Steps growing with distance: the air is densest near the eye.
        const float f = (i + 1.0) / steps;
        const float t_next = path * f * f;
        const float dt = t_next - t_prev;
        const float t = (t_prev + t_next) * 0.5;
        t_prev = t_next;
        const float3 p = float3(0.0, r, 0.0) + dir * t;
        const float ri = length(p);
        const float mu_s = dot(p, sun) / ri;
        const Medium m = medium_at(atmosphere, ri - atmosphere.bottom_radius);
        const float3 step_t = exp(-m.extinction * dt);
        const float3 sun_t = sun_transmittance(ri, mu_s);
        const float3 ms = multi_scattering.SampleLevel(multi_scattering_uv(atmosphere, ri, mu_s), 0.0).rgb;
        const float3 s = (m.rayleigh * pr + m.mie * pm) * sun_t + m.scattering * ms;
        const float3 safe = max(m.extinction, 1e-9);
        light += through * (s - s * step_t) / safe;
        through *= step_t;
    }
    if (ground >= 0.0) {
        // The ground where no terrain is drawn over it (beyond a database's
        // edge): its albedo, lit by the sun through the air, seen through
        // the air between.
        const float3 g = float3(0.0, r, 0.0) + dir * ground;
        const float mu_g = dot(normalize(g), sun);
        light += through * sun_transmittance(atmosphere.bottom_radius, mu_g) * saturate(mu_g) *
                 atmosphere.ground_albedo / kPi;
    }
    sky_view_out[uint3(id.xy, push.view + push.source * push.views)] =
        float4(light * atmosphere.sun_illuminance * scale, 1.0);
}

// The view's irradiance: the sun's through the air to the eye, and the
// sky's on level ground (the sky-view table over the upper hemisphere,
// cosine-weighted; it is symmetric about the sun's azimuth, so the half
// table counts twice).
groupshared float3 sky_sum[64];

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_irradiance(uint3 id: SV_DispatchThreadID) {
    const uint k = id.x;
    const float r = push.eye_radius;
    // Ring k of 64 in zenith angle, 0 to 90 degrees; 32 azimuths each.
    const float dz = 0.5 * kPi / 64.0;
    const float zenith = (k + 0.5) * dz;
    const float da = kPi / 32.0;
    float3 ring = float3(0.0);
    for (uint j = 0; j < 32; ++j) {
        const float azimuth = (j + 0.5) * da;
        const uint2 at = uint2(sky_view_uv(atmosphere, r, zenith, azimuth) * kSkyViewSize);
        // The sunlit layer and the moonlit (step 4), each symmetric about
        // its own light's azimuth.
        ring += sky_view_out[uint3(at, push.view)].rgb +
                sky_view_out[uint3(at, push.view + push.views)].rgb;
    }
    // L cos(z) sin(z) dz da, both halves.
    sky_sum[k] = ring * cos(zenith) * sin(zenith) * dz * da * 2.0;
    GroupMemoryBarrierWithGroupSync();
    if (k == 0) {
        float3 sky = float3(0.0);
        for (uint j = 0; j < 64; ++j) {
            sky += sky_sum[j];
        }
        ViewSky s;
        s.sun_irradiance = atmosphere.sun_illuminance * sun_transmittance(r, push.mu_s) * push.sun_up;
        s.moon_irradiance = atmosphere.sun_illuminance * push.moon_scale *
                            sun_transmittance(r, zenith_cos(push.moon_local));
        // The airglow's radiance from the whole upper hemisphere: pi L.
        s.sky_irradiance = sky + kAirglow * kPi;
        s.exposure = 1.0;
        s.unused = 0.0;
        s.unused1 = 0.0;
        view_sky_out[push.view] = s;
    }
}

// The exposure all views share (D-031), from the first view's light: the
// sun's on a surface facing it, and the sky's. Not the light on level
// ground: that falls toward nothing as the sun sets, while the air toward
// the sun stays bright, and an exposure following it washed dusk out.
// Moved toward its target by `adapt`, in stops. Below the floor (deep
// twilight, about 10 lux) it keeps rising, but half as fast in stops as the
// light falls: a moonlit night (0.25 lux) shows at a sixth of a day's
// brightness, a moonless one at a hundredth, dark but not black, as
// night vision adapts (step 4).
[shader("compute")]
[numthreads(1, 1, 1)]
void cs_exposure(uint3 id: SV_DispatchThreadID) {
    const ViewSky s = view_sky_out[0];
    const float light = luminance(s.sun_irradiance + s.moon_irradiance + s.sky_irradiance);
    const float floor_light = push.floor_illuminance;
    const float adapted =
        light >= floor_light ? light : floor_light * sqrt(max(light, 1e-12) / floor_light);
    const float target = push.key / adapted;
    float stops = log2(target);
    if (exposure_state[1] != 0.0) {
        // Not lerp(): a + (b - a) t leaves a settled exposure exactly as it was.
        stops = exposure_state[0] + (stops - exposure_state[0]) * push.adapt;
    }
    exposure_state[0] = stops;
    exposure_state[1] = 1.0;
    const float exposure = exp2(stops);
    for (uint v = 0; v < push.views; ++v) {
        view_sky_out[v].exposure = exposure;
    }
}

// The view's aerial-perspective volume (step 3, D-030): along each froxel
// column's ray, the light scattered toward the eye and the transmittance
// from the eye to each slice's far edge, as cs_sky_view integrates the sky.
// The atmosphere's frame is y up; the level frame's is z down.
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_aerial(uint3 id: SV_DispatchThreadID) {
    if (any(float2(id.xy) >= kAerialSize.xy)) {
        return;
    }
    const float2 ndc = (float2(id.xy) + 0.5) / kAerialSize.xy * 2.0 - 1.0;
    const float4 p = mul(push.clip_to_local, float4(ndc, 1.0, 1.0));
    const float3 ray = normalize(p.xyz / p.w);
    const float3 dir = float3(ray.x, -ray.z, ray.y);
    const float3 sun = float3(push.sun_local.x, -push.sun_local.z, push.sun_local.y);
    const float r = push.eye_radius;
    const float mu = dir.y;
    // The air ends at the ground, or at the top of the atmosphere.
    const float ground = distance_to_ground(r, mu, atmosphere.bottom_radius);
    const float top = max(distance_to_sphere(r, mu, atmosphere.top_radius), 0.0);
    const float end = ground >= 0.0 ? ground : top;
    const float c = dot(dir, sun);
    const float pr = rayleigh_phase(c);
    const float pm = mie_phase(atmosphere.mie_g, c);
    // And the moon's (step 4), scaled by its illuminance.
    const float3 moon = float3(push.moon_local.x, -push.moon_local.z, push.moon_local.y);
    const float cm = dot(dir, moon);
    const float pr_m = rayleigh_phase(cm);
    const float pm_m = mie_phase(atmosphere.mie_g, cm);
    float3 through = float3(1.0);
    float3 light = float3(0.0);
    float t_prev = 0.0;
    const uint depth = uint(kAerialSize.z);
    for (uint k = 0; k < depth; ++k) {
        // Two steps a slice, none past the end of the air.
        const float t_far = min(aerial_distance(float(k + 1)), end);
        for (uint h = 0; h < 2; ++h) {
            const float t_next = lerp(t_prev, t_far, 0.5 * float(h + 1)) ;
            const float dt = t_next - t_prev;
            if (dt > 0.0) {
                const float t = t_prev + 0.5 * dt;
                const float3 q = float3(0.0, r, 0.0) + dir * t;
                const float ri = length(q);
                const float mu_s = dot(q, sun) / ri;
                const Medium m = medium_at(atmosphere, ri - atmosphere.bottom_radius);
                const float3 step_t = exp(-m.extinction * dt);
                const float3 sun_t = sun_transmittance(ri, mu_s);
                const float3 ms =
                    multi_scattering.SampleLevel(multi_scattering_uv(atmosphere, ri, mu_s), 0.0).rgb;
                float3 s = (m.rayleigh * pr + m.mie * pm) * sun_t + m.scattering * ms;
                if (push.moon_scale > 0.0) {
                    const float mu_m = dot(q, moon) / ri;
                    const float3 moon_t = sun_transmittance(ri, mu_m);
                    const float3 ms_m = multi_scattering
                                            .SampleLevel(multi_scattering_uv(atmosphere, ri, mu_m), 0.0)
                                            .rgb;
                    s += ((m.rayleigh * pr_m + m.mie * pm_m) * moon_t + m.scattering * ms_m) *
                         push.moon_scale;
                }
                const float3 safe = max(m.extinction, 1e-9);
                light += through * (s - s * step_t) / safe;
                through *= step_t;
            }
            t_prev = t_next;
        }
        aerial_out[uint3(id.xy, push.view * depth + k)] =
            float4(light * atmosphere.sun_illuminance, dot(through, float3(1.0 / 3.0)));
    }
}
