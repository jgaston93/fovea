#pragma once

// The weather layers (milestone 6 step 6, D-034), from each view's
// lighting block: bands of fog, rain and snow the air between the eye and a
// point passes through, and cloud layers, their coverage thresholding
// noise, that shade what is below them. Included by frame.slang
// with FRAME_SET; renderer.cpp fills the layers, lowest first.

// Atmosphere::bottom_radius_km, in metres: heights are above it.
static const float kGroundRadiusM = 6371000.0;
// The cloud noise's coarsest cell, metres, and its period in cells
// (renderer.cpp's kCloudCellM and kCloudPeriod): the eye's place in it
// comes as noise_origin.
static const float kCloudCellM = 3000.0;
static const int kCloudPeriod = 256;
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

// The clouds' shape (cloud_noise.slang's volume of Perlin-Worley noise):
// it tiles every kShapeCells noise cells (12 km), and its quantiles (the
// table's floats from 256 on) give the value below which a share of it
// lies. Shared by the clouds drawn (clouds.slang) and their shade on the
// ground (under_clouds), so the shadows fall under the clouds.
static const float kShapeCells = 4.0;

float shape_quantile(StructuredBuffer<uint> table, float x) {
    const float i = saturate(x) * 255.0;
    const uint lo = uint(floor(i));
    const uint hi = min(lo + 1, 255u);
    return lerp(asfloat(table[256 + lo]), asfloat(table[256 + hi]), i - float(lo));
}

// A layer's density, 0 to 1, from the shape's value `base` there: the shape
// above the quantile a share `coverage * widen` (at most all) of it lies
// above, whole a third of the way up the values beyond (the shape's values
// bunch: over all of them, most cloud stayed thin). An overcast is whole,
// thicker and thinner. `widen` makes up what the edges' erosion takes back.
float cover_density(StructuredBuffer<uint> table, float coverage, float base, float widen) {
    if (coverage <= 0.0) {
        return 0.0;
    }
    if (coverage >= 1.0) {
        return 0.6 + 0.4 * base;
    }
    const float above = min(coverage * widen, 1.0);
    const float threshold = shape_quantile(table, 1.0 - above);
    const float core = shape_quantile(table, 1.0 - above * 0.67);
    return saturate((base - threshold) / max(core - threshold, 1e-3));
}

// The 2D layer's widening (clouds.slang's fs_clouds; measured).
static const float kLayerWiden = 1.25;

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
// From the clouds' shape and table (passed, so the terrain can pass its own
// copies: lavapipe crashes compiling its fragment shader with the frame
// set's images), sampled with implicit level of detail (lavapipe takes
// only that there), for every layer alike: derivatives want the same path
// through every pixel.
float4 under_clouds(float3 rel, ViewSky s, Texture3D<float4> shape, SamplerState linear,
                    StructuredBuffer<uint> table) {
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
        // Clouds above the point only (a band, or a cloud below it, counts
        // for nothing).
        const float over = c.w >= 0.0 && c.x > h ? 1.0 : 0.0;
        const float tau = cloud_depth(c);
        const float3 straight_up = cloud_cells(rel + up * max(c.x - h, 0.0), uint(k));
        const float3 sunward = cloud_cells(rel + l * max(sphere_span(h, mu_l, c.x).y, 0.0), uint(k));
        const float above =
            cover_density(table, c.w, shape.Sample(linear, straight_up / kShapeCells).r, kLayerWiden) *
            over;
        const float toward =
            cover_density(table, c.w, shape.Sample(linear, sunward / kShapeCells).r, kLayerWiden) *
            over;
        sky = lerp(sky, (level * direct + sky) * cloud_diffuse(tau), above);
        direct *= exp(-tau * toward);
    }
    return float4(direct, sky);
}
