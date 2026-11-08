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
    float3 moon_local;  // towards the moon, the eye's level frame
    float moon_up;      // how much of the moon's disc is above the eye's horizon
    // The shadow maps (step 5, D-032): points relative to the eye (ECEF
    // axes, metres) to each cascade's map, x and y -1 to 1, z 0 nearest the
    // light; a texel's width there, metres; and whose light they are.
    float4x4 cascade[4];
    float4 cascade_texel;
    uint shadow_light;  // 0 none, 1 the sun, 2 the moon
    float unused2;      // single floats: a float3 would align to 16
    float unused3;
    float unused4;
};

// Written by the sky's compute passes for each view: mirrored as ViewSky in
// sky.hpp (std430). Light is pre-exposed where it's used: multiplied by
// `exposure`, the same in every view (D-031).
struct ViewSky {
    float3 sun_irradiance;  // at the eye, on a surface facing the sun
    float exposure;
    float3 sky_irradiance;  // at the eye, on a level surface (moonlit sky and airglow too)
    float unused;
    float3 moon_irradiance;  // at the eye, on a surface facing the moon (step 4)
    float unused1;
};

// The night sky's own faint glow (airglow and starlight, about 2e-4 lux on
// level ground; the sun's 10,000 units are 100,000 lux): what a moonless
// sky shows, so night isn't black (milestone 6 step 4). Radiance, per
// steradian, slightly blue-green as airglow is.
static const float3 kAirglow = float3(0.55, 0.65, 0.6) * (2e-5 / kPi);

#ifdef FRAME_SET
[[vk::binding(0, FRAME_SET)]] ConstantBuffer<ViewLighting> lighting;
[[vk::binding(1, FRAME_SET)]] StructuredBuffer<ViewSky> view_sky;
[[vk::binding(2, FRAME_SET)]] Sampler2DArray<float4> sky_view_table;
// The views' aerial-perspective volumes, stacked in depth: view v's slices
// are v * 64 to v * 64 + 63 (milestone 6 step 3).
[[vk::binding(4, FRAME_SET)]] Sampler3D<float4> aerial_volume;
// The atmosphere's transmittance table (for the stars and the moon's disc).
[[vk::binding(5, FRAME_SET)]] Sampler2D<float4> transmittance_table;
// The views' shadow maps, kShadowCascades layers a view (step 5).
[[vk::binding(6, FRAME_SET)]] Texture2DArray<float> shadow_maps;
// Nearest-texel, the comparison made in shadow_lit.
[[vk::binding(7, FRAME_SET)]] SamplerState shadow_sampler;

// How much of the shadow light reaches a point relative to the eye (ECEF,
// metres) with normal n: the first cascade whose map holds it, its depth
// compared with the nearest caster's over 3 x 3 texels. The point is moved
// a texel along its normal first, and toward the light, so a surface
// doesn't shadow itself. Taking the maps as arguments lets the terrain pass its own copy
// (see terrain.slang). The depths are read with Sample and a nearest
// sampler, and compared here: in the terrain's fragment shader, lavapipe
// (Mesa 25.2.8, LLVM 20) crashes compiling Load, SampleLevel or SampleCmp
// of an image, though not Sample.
float shadow_lit(Texture2DArray<float> maps, SamplerState nearest, float3 rel, float3 n) {
    if (lighting.shadow_light == 0) {
        return 1.0;
    }
    // Toward the light too, the more the light grazes the surface (a texel
    // there spans more depth): along the light's own ray, which doesn't
    // move a shadow's edge.
    const float3 l = lighting.shadow_light == 1 ? lighting.sun : lighting.moon;
    const float ndl = max(dot(n, l), 0.05);
    // At most 4 (the light 14 degrees off the surface): beyond, the point
    // would pass casters near it and their shadows start detached.
    const float grazing = min(sqrt(saturate(1.0 - ndl * ndl)) / ndl, 4.0);
    for (uint c = 0; c < 4; ++c) {
        const float texel_m = lighting.cascade_texel[c];
        const float3 p = rel + n * texel_m + l * texel_m * (1.0 + 1.5 * grazing);
        const float4 m = mul(lighting.cascade[c], float4(p, 1.0));
        if (all(abs(m.xy) < 0.98) && m.z >= 0.0 && m.z <= 1.0) {
            const float2 uv = m.xy * 0.5 + 0.5;
            const float layer = float(lighting.view * 4 + c);
            const float texel = 1.0 / 2048.0;
            float lit = 0.0;
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    const float caster =
                        maps.Sample(nearest, float3(uv + float2(x, y) * texel, layer));
                    lit += m.z <= caster ? 1.0 : 0.0;
                }
            }
            return lit / 9.0;
        }
    }
    return 1.0;
}

// What of the sun's and the moon's light a shadow leaves: (sun, moon).
float2 shadowed(float lit) {
    return float2(lighting.shadow_light == 1 ? lit : 1.0, lighting.shadow_light == 2 ? lit : 1.0);
}
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
// as on level ground, which a model's walls also take for now. The moon's
// too (step 4).
float3 irradiance_on(float3 n, ViewSky s, float2 lit) {
    return s.sun_irradiance * saturate(dot(n, lighting.sun)) * lit.x +
           s.moon_irradiance * saturate(dot(n, lighting.moon)) * lit.y + s.sky_irradiance;
}
#endif
