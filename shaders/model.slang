// Models (milestone 4 step 3): each primitive of each placement drawn is one
// indirect draw command, its instance one record: the placement's frame
// (glTF's: +Y up, +Z its front) to ECEF axes, and its origin relative to
// the eye (made in double on the CPU), and the primitive's material.
// Vertices come from the shared vertex buffer.

struct ModelDraw {
    float4 row0;  // xyz: the frame to ECEF axes; w: the origin relative to the eye
    float4 row1;
    float4 row2;
    uint material;
    uint unused0;  // single uints: a uint3 would align to 16
    uint unused1;
    uint unused2;
};

struct Material {
    float4 base_colour;  // linear
    uint image;          // 0xFFFFFFFF: none
    uint sampler;        // wrap u x 3 + wrap v
    uint alpha_mode;     // 0 opaque, 1 mask, 2 blend (drawn opaque for now)
    float alpha_cutoff;
};

struct ModelPush {
    float4x4 ecef_to_clip;  // directions relative to the eye to clip space
    uint samples;           // the scene pass's per pixel
};
[[vk::push_constant]] ConstantBuffer<ModelPush> push;

// Each view's lighting (milestone 6), set 1.
#define FRAME_SET 1
#include "include/frame.slang"

[[vk::binding(0, 0)]] StructuredBuffer<ModelDraw> draws;
[[vk::binding(1, 0)]] StructuredBuffer<Material> materials;
[[vk::binding(2, 0)]] SamplerState samplers[9];
[[vk::binding(3, 0)]] Texture2D<float4> images[];

struct VertexIn {
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv;
};

struct ModelVertex {
    float4 position : SV_Position;
    float3 normal;
    float2 uv;
    float3 rel;  // the point relative to the eye, ECEF axes, metres
    nointerpolation uint material;
};

[shader("vertex")]
ModelVertex vs_model(VertexIn v, uint instance: SV_VulkanInstanceID) {
    const ModelDraw d = draws[instance];
    const float4 p = float4(v.position, 1.0);
    const float3 rel = float3(dot(d.row0, p), dot(d.row1, p), dot(d.row2, p));
    // The frame is a rotation: normals turn with it.
    const float3 n = float3(dot(d.row0.xyz, v.normal), dot(d.row1.xyz, v.normal),
                            dot(d.row2.xyz, v.normal));
    ModelVertex o;
    o.position = mul(push.ecef_to_clip, float4(rel, 1.0));
    o.rel = rel;
    o.normal = n;
    o.uv = v.uv;
    o.material = d.material;
    return o;
}

[shader("fragment")]
float4 fs_model(ModelVertex i, bool front: SV_IsFrontFace) : SV_Target {
    const Material m = materials[i.material];
    float4 colour = m.base_colour;
    if (m.image != 0xFFFFFFFFu) {
        // sRGB images: the sample is linear already.
        colour *= images[NonUniformResourceIndex(m.image)].Sample(
            samplers[NonUniformResourceIndex(m.sampler)], i.uv);
    }
    // Opaque (and blend, drawn opaque for now): all the pixel's samples.
    float coverage = 1.0;
    if (m.alpha_mode == 1) {
        if (push.samples > 1) {
            // Alpha to coverage (milestone 5 step 2, D-027): alpha sharpened
            // about the material's cutoff, so the cut-out falls where its
            // author put it, its edge about a pixel wide.
            // Where alpha doesn't vary, glTF's rule: at the cutoff or over
            // it, opaque. Wholly cut away: nothing to shade.
            const float w = fwidth(colour.a);
            coverage = w > 1e-4 ? saturate((colour.a - m.alpha_cutoff) / w + 0.5)
                                : (colour.a >= m.alpha_cutoff ? 1.0 : 0.0);
            if (coverage <= 0.0) {
                discard;
            }
        } else if (colour.a < m.alpha_cutoff) {
            discard;
        }
    }
    // Both sides are drawn: a back face's normal is turned round.
    const float3 n = normalize(i.normal) * (front ? 1.0 : -1.0);
    // Lambertian under the sun and sky, pre-exposed (D-031).
    const ViewSky s = view_sky[lighting.view];
    // Through the air to the eye (step 3).
    const float4 air = aerial_perspective(i.position.xy, length(i.rel) * 0.001);
    // In the shadows of whatever is between it and the light (step 5).
    const float2 shade = shadowed(shadow_lit(shadow_maps, shadow_sampler, i.rel, n, ddx(i.rel), ddy(i.rel)));
    // Under the clouds, through the air and then the fog, rain and snow
    // (step 6, D-034).
    const float3 lit = colour.rgb * irradiance_on(n, s, shade, under_clouds(i.rel, s)) * kInvPi;
    const float dist = length(i.rel);
    const float mu = dot(i.rel, lighting.up) / max(dist, 1e-3);
    const float3 seen = through_bands(lit * air.a + air.rgb * lighting.weather_light, mu, dist, s, 1.0);
    return float4(seen * s.exposure, coverage);
}

// The shadow maps (step 5, D-032): depth only, from the light, push's
// matrix taking points relative to the eye to the cascade's map.
struct ShadowVertex {
    float4 position : SV_Position;
    float2 uv;
    nointerpolation uint material;
};

[shader("vertex")]
ShadowVertex vs_model_shadow(VertexIn v, uint instance: SV_VulkanInstanceID) {
    const ModelDraw d = draws[instance];
    const float4 p = float4(v.position, 1.0);
    const float3 rel = float3(dot(d.row0, p), dot(d.row1, p), dot(d.row2, p));
    ShadowVertex o;
    o.position = mul(push.ecef_to_clip, float4(rel, 1.0));
    o.uv = v.uv;
    o.material = d.material;
    return o;
}

// Masked materials cast only what they don't cut away.
[shader("fragment")]
void fs_model_shadow(ShadowVertex i) {
    const Material m = materials[i.material];
    if (m.alpha_mode != 1) {
        return;
    }
    float alpha = m.base_colour.a;
    if (m.image != 0xFFFFFFFFu) {
        alpha *= images[NonUniformResourceIndex(m.image)]
                     .Sample(samplers[NonUniformResourceIndex(m.sampler)], i.uv)
                     .a;
    }
    if (alpha < m.alpha_cutoff) {
        discard;
    }
}
