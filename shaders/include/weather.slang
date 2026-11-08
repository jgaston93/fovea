#pragma once

// The weather layers (milestone 6 step 6, D-034), from each view's
// lighting block: bands of fog, rain and snow the air between the eye and a
// point passes through, and cloud layers, their coverage shaping
// procedural noise, that shade what is below them. Included by frame.slang
// with FRAME_SET; renderer.cpp fills the layers, lowest first.

// Atmosphere::bottom_radius_km, in metres: heights are above it.
static const float kGroundRadiusM = 6371000.0;
// The cloud noise's coarsest cell, metres, and its period in cells
// (renderer.cpp's kCloudCellM and kCloudPeriod): the eye's place in it
// comes as noise_origin.
static const float kCloudCellM = 3000.0;
static const int kCloudPeriod = 256;
// How far the noise's values spread (their standard deviation, measured):
// coverage maps through it, so a coverage of c clouds about c of the sky.
// Five octaves for the layers drawn, three for their shade on the ground.
static const float kCloudSpread = 0.12;
static const float kCloudShadeSpread = 0.136;
// The deepest a cloud layer is, optically (its extinction, per metre, is
// renderer.cpp's kCloudExtinctionPerM: a stratus 300 m thick is 12 deep).
static const float kCloudMaxDepth = 40.0;

// The distances along a ray, from a point at height h (metres) with zenith
// cosine mu, within the sphere of height t: x the near, y the far; x > y if
// the ray misses it. The roots of s^2 + 2 r mu s + (r^2 - (R + t)^2), taken
// so neither cancels (r is the Earth's radius plus h: its square swamps
// the heights' difference in floats).
float2 sphere_span(float h, float mu, float t) {
    const float r = kGroundRadiusM + h;
    const float b = r * mu;
    const float c = (h - t) * (2.0 * kGroundRadiusM + h + t);
    const float disc = b * b - c;
    if (disc < 0.0) {
        return float2(1.0, -1.0);
    }
    const float q = -(b + (b >= 0.0 ? 1.0 : -1.0) * sqrt(disc));
    const float other = q != 0.0 ? c / q : 0.0;
    return float2(min(q, other), max(q, other));
}

// How far a ray from the eye, of zenith cosine mu, runs between heights
// bottom and top within its first `dist` metres.
float band_length(float mu, float dist, float bottom, float top) {
    const float h = lighting.eye_height_m;
    const float2 outer = sphere_span(h, mu, top);
    const float2 inner = sphere_span(h, mu, bottom);
    const float within = max(min(outer.y, dist) - max(outer.x, 0.0), 0.0);
    const float below = max(min(inner.y, dist) - max(inner.x, 0.0), 0.0);
    return max(within - below, 0.0);
}

// What of a point `dist` metres along a ray (zenith cosine mu) the fog,
// rain and snow let through to the eye.
float band_transmittance(float mu, float dist) {
    float depth = 0.0;
    for (uint k = 0; k < lighting.layer_count; ++k) {
        const float4 l = lighting.layer[k];
        if (l.w < 0.0) {
            depth += l.z * band_length(mu, dist, l.x, l.y);
        }
    }
    return exp(-depth);
}

// The light the air between the eye and a point scatters toward it (from
// aerial_perspective): under clouds, not the clear air's sunlit glow but
// the clouds' diffuse light, as fog takes it, for the share of the sky they
// cover (clear_sky the share they don't).
float3 air_light(float4 air, ViewSky s) {
    if (lighting.clear_sky >= 1.0) {
        return air.rgb;
    }
    return air.rgb * lighting.clear_sky +
           (1.0 - air.a) * band_radiance(s) * (1.0 - lighting.clear_sky);
}

// The light a band scatters toward the eye, before exposure: as a cloud
// lit from above, most of the light on level ground at the eye (less what
// clouds above take).
float3 band_radiance(ViewSky s) {
    const float3 e = s.sun_irradiance * saturate(-lighting.sun_local.z) +
                     s.moon_irradiance * saturate(-lighting.moon_local.z) + s.sky_irradiance;
    return e * lighting.weather_light * 0.8 * kInvPi;
}

// A colour (radiance, before or after exposure as `scale` is 1 or the
// exposure) seen through the bands to a point dist metres along a ray.
float3 through_bands(float3 colour, float mu, float dist, ViewSky s, float scale) {
    if (lighting.layer_count == 0) {
        return colour;
    }
    return lerp(band_radiance(s) * scale, colour, band_transmittance(mu, dist));
}

// A PCG hash (O'Neill; Jarzynski and Olano's for shaders).
uint pcg(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// A lattice point's value, 0 to 1, wrapped to the period.
float lattice(int3 c, int period) {
    const uint3 u = uint3(((c % period) + period) % period);
    return float(pcg(u.x + pcg(u.y + pcg(u.z))) >> 8) / 16777216.0;
}

// Value noise, quintic between lattice points.
float value_noise(float3 p, int period) {
    const float3 i = floor(p);
    const float3 f = p - i;
    const float3 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    const int3 c = int3(i);
    const float x00 = lerp(lattice(c, period), lattice(c + int3(1, 0, 0), period), w.x);
    const float x10 =
        lerp(lattice(c + int3(0, 1, 0), period), lattice(c + int3(1, 1, 0), period), w.x);
    const float x01 =
        lerp(lattice(c + int3(0, 0, 1), period), lattice(c + int3(1, 0, 1), period), w.x);
    const float x11 =
        lerp(lattice(c + int3(0, 1, 1), period), lattice(c + int3(1, 1, 1), period), w.x);
    return lerp(lerp(x00, x10, w.y), lerp(x01, x11, w.y), w.z);
}

// A cloud layer's cover at a point: its density, 0 (clear) to 1. `p` the
// point in noise cells, `footprint` the cells a pixel spans there: an
// octave finer than half a pixel fades to its mean, and as detail goes the
// density tends to the coverage itself, so distant broken cloud reads as
// the thin layer it averages to rather than flickering.
float cloud_density(float coverage, float3 p, uint octaves, float spread, float footprint) {
    if (coverage >= 1.0) {
        return 1.0;
    }
    if (coverage <= 0.0) {
        return 0.0;
    }
    float sum = 0.0;
    float total = 0.0;
    float kept = 0.0;  // the variance the octaves keep, of all of it
    float all = 0.0;
    float a = 1.0;
    float f = 1.0;
    int period = kCloudPeriod;
    for (uint o = 0; o < octaves; ++o) {
        const float detail = saturate(2.0 - 2.0 * footprint * f);
        const float n = detail > 0.0 ? value_noise(p * f, period) : 0.5;
        sum += a * lerp(0.5, n, detail);
        total += a;
        kept += a * a * detail * detail;
        all += a * a;
        a *= 0.5;
        f *= 2.0;
        period *= 2;
    }
    const float n = sum / total;
    // To a uniform 0 to 1 (a logistic close to the normal's distribution),
    // then cloud where it's above 1 - coverage, its density rising over a
    // quarter beyond (squared, so the thin edge is wide: a cloud is all but
    // opaque at a tenth of its density, and a narrow edge showed as a
    // bright rim from below), close to where the coverage puts it.
    const float u = 1.0 / (1.0 + exp(-1.702 * (n - 0.5) / spread));
    const float e = smoothstep(1.0 - coverage, 1.0 - coverage + 0.25, u);
    const float d = e * e;
    return lerp(coverage, d, sqrt(kept / all));
}

// A point relative to the eye (ECEF, metres) in layer k's noise cells.
float3 cloud_cells(float3 rel, uint k) {
    return lighting.noise_origin + rel / kCloudCellM + float3(61.0, 17.0, 29.0) * float(k);
}

// Layer k's optical depth where it is whole.
float cloud_depth(float4 l) {
    return min(l.z * (l.y - l.x), kCloudMaxDepth);
}

// The diffuse light a cloud of optical depth tau lets through (two-stream,
// conservative scattering, asymmetry 0.8); it reflects the rest.
float cloud_diffuse(float tau) {
    return 1.0 / (1.0 + 0.15 * tau);
}

// A point's height (metres) and up, from its place relative to the eye:
// the difference of radii taken so it doesn't cancel.
float point_height(float3 rel, out float3 up) {
    const float r = kGroundRadiusM + lighting.eye_height_m;
    const float3 p = lighting.up * r + rel;
    const float len = length(p);
    up = p / len;
    return lighting.eye_height_m + (2.0 * r * dot(lighting.up, rel) + dot(rel, rel)) / (r + len);
}

// What the clouds above a point (relative to the eye, ECEF, metres) leave
// of its light: x the factor on the sun's and the moon's (from the clouds
// toward the shadows' light, or the sun's), and the sky's irradiance there
// in yzw: under cloud, what the cloud lets through of all the light on it.
float4 under_clouds(float3 rel, ViewSky s) {
    float direct = 1.0;
    float3 sky = s.sky_irradiance;
    if (lighting.cloud_count == 0) {
        return float4(direct, sky);
    }
    float3 up;
    const float h = point_height(rel, up);
    const float3 l = lighting.shadow_light == 2 ? lighting.moon : lighting.sun;
    const float mu_l = dot(l, up);
    const float3 level = s.sun_irradiance * saturate(dot(lighting.sun, up)) +
                         s.moon_irradiance * saturate(dot(lighting.moon, up));
    // The highest first: each passes on what it lets through.
    for (int k = int(lighting.layer_count) - 1; k >= 0; --k) {
        const float4 c = lighting.layer[k];
        if (c.w < 0.0 || c.x <= h) {
            continue;
        }
        const float tau = cloud_depth(c);
        const float above = cloud_density(c.w, cloud_cells(rel + up * (c.x - h), uint(k)), 3,
                                          kCloudShadeSpread, 0.0);
        const float toward = cloud_density(
            c.w, cloud_cells(rel + l * sphere_span(h, mu_l, c.x).y, uint(k)), 3,
            kCloudShadeSpread, 0.0);
        sky = lerp(sky, (level * direct + sky) * cloud_diffuse(tau), above);
        direct *= exp(-tau * toward);
    }
    return float4(direct, sky);
}
