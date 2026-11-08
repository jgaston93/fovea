#pragma once

#include "atmosphere.slang"

// Each view's lighting (milestone 6, D-029 to D-031): what the scene's
// shaders light with. With FRAME_SET defined (the set number the pipeline
// binds it as), the frame set's bindings too.

static const float kInvPi = 1.0 / kPi;

// Written by the CPU for each view: mirrored as ViewLighting in
// gpu_context.hpp (std140).
struct ViewLighting {
    float3 sun;        // towards the sun, ECEF axes
    float moon_lit;    // the fraction of the moon's disc lit
    float3 moon;       // towards the moon from the eye, ECEF axes
    float sun_up;      // how much of the sun's disc is above the eye's horizon
    float3 sun_local;  // towards the sun, the eye's level frame (x north, y east, z down)
    uint view;         // this view's entries in the arrays below
    float eye_radius;  // km: the atmosphere's bottom radius plus the eye's height
    float inv_width;   // 1 / the view's render width, pixels (single floats: a float3
    float inv_height;  // would align to 16 and grow the block)
    uint views;        // how many views share the sky's arrays
};

// Written by the sky's compute passes for each view: mirrored as ViewSky in
// sky.hpp (std430). Light is pre-exposed where it's used: multiplied by
// `exposure`, the same in every view (D-031).
struct ViewSky {
    float3 sun_irradiance;  // at the eye, on a surface facing the sun
    float exposure;
    float3 sky_irradiance;  // at the eye, on a level surface
    float unused;
};

#ifdef FRAME_SET
[[vk::binding(0, FRAME_SET)]] ConstantBuffer<ViewLighting> lighting;
[[vk::binding(1, FRAME_SET)]] StructuredBuffer<ViewSky> view_sky;
[[vk::binding(2, FRAME_SET)]] Sampler2DArray<float4> sky_view_table;
// The views' aerial-perspective volumes, stacked in depth: view v's slices
// are v * 64 to v * 64 + 63 (milestone 6 step 3).
[[vk::binding(4, FRAME_SET)]] Sampler3D<float4> aerial_volume;
// Binding 3, the atmosphere (atmosphere.slang's Atmosphere), is declared by
// the shaders that take it.

// What the air between the eye and a point adds and takes away (D-030).
// The transmittance is grey, the mean of red, green and blue, as Hillaire's
// volume keeps it: distant lit surfaces lose a little of the reddening that
// blue's faster extinction would give them, while the light scattered in
// keeps its colour.
// `pixel` the fragment's position (SV_Position.xy), `distance_km` its
// distance from the eye. Returns the light scattered in (rgb, before
// exposure) and the transmittance (a). The volume's first slice is faded in
// from the eye, where nothing is in the way yet.
float4 aerial_perspective(float2 pixel, float distance_km) {
    return aerial_perspective_uv(pixel * float2(lighting.inv_width, lighting.inv_height),
                                 distance_km);
}

// The same, by the point's place on the screen (0 to 1, as SV_Position / size).
float4 aerial_perspective_uv(float2 uv, float distance_km) {
    // The slice coordinate of the point; the volume's texel centres sit at
    // the middle of each slice, so slice s's far edge is at s + 1.
    const float s = aerial_slice(distance_km);
    const float depth = kAerialSize.z;
    const float z = clamp(s - 0.5, 0.5, depth - 0.5);
    float4 a = aerial_volume.SampleLevel(
        float3(uv, (depth * float(lighting.view) + z) / (depth * float(lighting.views))), 0.0);
    const float near = saturate(s);
    a.rgb *= near;
    a.a = lerp(1.0, a.a, near);
    return a;
}

// The light falling on a surface of normal n (ECEF axes): the sun's on it
// (its irradiance counts only the disc above the horizon), and the sky's,
// as on level ground, which a model's walls also take for now.
float3 irradiance_on(float3 n, ViewSky s) {
    return s.sun_irradiance * saturate(dot(n, lighting.sun)) + s.sky_irradiance;
}
#endif
