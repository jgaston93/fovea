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
    float3 sun;             // towards the sun, ECEF axes
    uint unused;
};
[[vk::push_constant]] ConstantBuffer<ModelPush> push;

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
    if (m.alpha_mode == 1 && colour.a < m.alpha_cutoff) {
        discard;
    }
    // Both sides are drawn: a back face's normal is turned round.
    const float3 n = normalize(i.normal) * (front ? 1.0 : -1.0);
    // The terrain's sun, and light from the sky everywhere.
    const float sun = saturate(dot(n, push.sun));
    return float4(colour.rgb * (0.35 + 0.65 * sun), 1.0);
}
