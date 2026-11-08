// Each view's lighting (milestone 6, D-029 to D-031): what the scene's
// shaders light with. With FRAME_SET defined (the set number the pipeline
// binds it as), the frame set's bindings too.

static const float kInvPi = 0.318309886183791;

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
    float unused0;     // single floats: a float3 would align to 16 and grow the block
    float unused1;
    float unused2;
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
// Binding 3, the atmosphere (atmosphere.slang's Atmosphere), is declared by
// the shaders that take it.

// The light falling on a surface of normal n (ECEF axes): the sun's on it
// (its irradiance counts only the disc above the horizon), and the sky's,
// as on level ground, which a model's walls also take for now.
float3 irradiance_on(float3 n, ViewSky s) {
    return s.sun_irradiance * saturate(dot(n, lighting.sun)) + s.sky_irradiance;
}
#endif
