#pragma once

// The weather layers (milestone 6 step 6, D-034), from each view's
// lighting block: bands of fog, rain and snow that the air between the eye
// and a point passes through, integrated exactly along the ray. Included
// by frame.slang with FRAME_SET; renderer.cpp fills the bands, lowest
// first. Cloud layers aren't drawn yet (#73).

// The distances along a ray, from a point at height h (metres) with zenith
// cosine mu, within the sphere of height t: x the near, y the far; x > y if
// the ray misses it. The roots of s^2 + 2 r mu s + (r^2 - (R + t)^2), taken
// so neither cancels (r is the Earth's radius plus h: its square swamps
// the heights' difference in floats).
float2 sphere_span(float h, float mu, float t) {
    const float r = lighting.ground_radius_m + h;
    const float b = r * mu;
    const float c = (h - t) * (2.0 * lighting.ground_radius_m + h + t);
    const float disc = b * b - c;
    if (disc < 0.0) {
        return float2(1.0, -1.0);
    }
    const float q = -(b + (b >= 0.0 ? 1.0 : -1.0) * sqrt(disc));
    const float other = q != 0.0 ? c / q : 0.0;
    return float2(min(q, other), max(q, other));
}

// A ray's first `dist` metres, stopped at the ground (height 0) if it meets
// it first: the sky's rays, sent to 1e9 m, don't pass through the Earth to
// the bands beyond. An eye below height 0 (the sea's geoid can be) isn't
// stopped.
float to_ground(float mu, float dist) {
    const float2 g = sphere_span(lighting.eye_height_m, mu, 0.0);
    return g.x <= g.y && g.x > 0.0 ? min(dist, g.x) : dist;
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
    dist = to_ground(mu, dist);
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

// The light under the overcast rain or snow falls from (clouds aren't drawn
// yet, #73, but rain and snow imply them): no sun or moon, a fraction of the
// light on level ground. Rain's (kind 1) a dark, blue-grey fifth, a thick
// nimbostratus; snow's (kind 2) nearer half and whiter, from thinner cloud.
float3 overcast_light(ViewSky s, float kind) {
    const float3 tint = kind > 1.5 ? float3(0.45, 0.46, 0.48) : float3(0.19, 0.21, 0.24);
    return luminance(level_light(s)) * tint;
}

// The kind of the lowest band of rain or snow (layer w 1 or 2) a height is
// below the top of, so under its overcast, or 0.
float precipitation_above(float h) {
    for (uint k = 0; k < lighting.layer_count; ++k) {
        const float4 l = lighting.layer[k];
        if (l.w > 0.5 && h < l.y) {
            return l.w;
        }
    }
    return 0.0;
}

// The height of a point relative to the eye, on the sphere as the bands
// are: |eye + rel| less the radius, its difference from the eye's taken
// so the radius's square doesn't swamp it in floats.
float height_at(float3 rel) {
    const float r = lighting.ground_radius_m + lighting.eye_height_m;
    const float q = 2.0 * r * dot(rel, lighting.up) + dot(rel, rel);
    return lighting.eye_height_m + q / (sqrt(max(r * r + q, 0.0)) + r);
}

// The same for a point relative to the eye.
float precipitation_over(float3 rel) {
    return precipitation_above(height_at(rel));
}

// The atmosphere's in-scatter (step 3, lit by the sun) under an overcast of
// kind `kind`: grey, scaled as the overcast's light is to the open sky's.
float3 air_under(float3 air, ViewSky s, float kind) {
    if (kind < 0.5) {
        return air;
    }
    return luminance(air) * overcast_light(s, kind) / max(luminance(level_light(s)), 1e-6);
}

// A colour (radiance, before or after exposure as `scale` is 1 or the
// exposure) seen through the bands to a point dist metres along a ray: the
// light they scatter toward the eye, by each band's share of the depth,
// fog's as a cloud lit from above (most of the light on level ground, or
// under rain or snow their overcast's), rain's and snow's their overcast's.
float3 through_bands(float3 colour, float mu, float dist, ViewSky s, float scale) {
    if (lighting.layer_count == 0) {
        return colour;
    }
    dist = to_ground(mu, dist);
    float depth = 0.0;
    float3 light = float3(0.0);
    for (uint k = 0; k < lighting.layer_count; ++k) {
        const float4 l = lighting.layer[k];
        const float d = l.z * band_length(mu, dist, l.x, l.y);
        depth += d;
        const float kind = l.w > 0.5 ? l.w : precipitation_above(0.5 * (l.x + l.y));
        light += (kind > 0.5 ? overcast_light(s, kind) : level_light(s)) * d;
    }
    if (depth <= 0.0) {
        return colour;
    }
    return lerp(light / depth * 0.8 * kInvPi * scale, colour, exp(-depth));
}
