#pragma once

// The weather layers (milestone 6 step 6, D-034), from each view's
// lighting block: bands of fog, rain and snow that the air between the eye
// and a point passes through, integrated exactly along the ray. Included
// by frame.slang with FRAME_SET; renderer.cpp fills the bands, lowest
// first. Cloud layers aren't drawn yet (#73).

// Atmosphere::bottom_radius_km, in metres: heights are above it.
static const float kGroundRadiusM = 6371000.0;

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
        depth += l.z * band_length(mu, dist, l.x, l.y);
    }
    return exp(-depth);
}

// The light on level ground at the eye.
float3 level_light(ViewSky s) {
    return s.sun_irradiance * saturate(-lighting.sun_local.z) +
           s.moon_irradiance * saturate(-lighting.moon_local.z) + s.sky_irradiance;
}

// The light under the overcast rain and snow fall from (clouds aren't
// drawn yet, #73, but rain and snow imply them): no sun or moon, a grey
// third of the light on level ground, about what a thick stratus lets
// through.
float3 overcast_light(ViewSky s) {
    return float3(luminance(level_light(s)) * 0.3);
}

// Whether a point (relative to the eye) is below the top of a band of rain
// or snow (layer w 1), so under its overcast. Its height taken as level
// with the eye's ground: near enough within the bands' reach.
bool under_precipitation(float3 rel) {
    const float h = lighting.eye_height_m + dot(rel, lighting.up);
    for (uint k = 0; k < lighting.layer_count; ++k) {
        const float4 l = lighting.layer[k];
        if (l.w > 0.5 && h < l.y) {
            return true;
        }
    }
    return false;
}

// A colour (radiance, before or after exposure as `scale` is 1 or the
// exposure) seen through the bands to a point dist metres along a ray: the
// light they scatter toward the eye, by their share of the depth, fog's as
// a cloud lit from above (most of the light on level ground), rain's and
// snow's the overcast's.
float3 through_bands(float3 colour, float mu, float dist, ViewSky s, float scale) {
    if (lighting.layer_count == 0) {
        return colour;
    }
    float fog = 0.0;
    float precipitation = 0.0;
    for (uint k = 0; k < lighting.layer_count; ++k) {
        const float4 l = lighting.layer[k];
        const float depth = l.z * band_length(mu, dist, l.x, l.y);
        if (l.w > 0.5) {
            precipitation += depth;
        } else {
            fog += depth;
        }
    }
    const float depth = fog + precipitation;
    if (depth <= 0.0) {
        return colour;
    }
    const float3 scattered =
        (level_light(s) * fog + overcast_light(s) * precipitation) / depth * 0.8 * kInvPi;
    return lerp(scattered * scale, colour, exp(-depth));
}
