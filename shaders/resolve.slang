// The resolve (D-028, milestone 5 step 1): a multisampled HDR pixel's
// samples, each weighted by 1 / (1 + its luminance), into one
// (libs/render/include/fovea/render/resolve.hpp is its reference), and its
// depth the nearest sample's (reverse-Z: the greatest). Drawn with
// vs_fullscreen (reference_scene.slang) into the single-sampled HDR target
// the tone mapping reads, and its depth.

struct ResolvePush {
    uint samples;
};
[[vk::push_constant]] ConstantBuffer<ResolvePush> push;

[[vk::binding(0, 0)]] Texture2DMS<float4> scene;
[[vk::binding(1, 0)]] Texture2DMS<float> depth;

struct Resolved {
    float4 colour : SV_Target;
    float depth : SV_Depth;
};

[shader("fragment")]
Resolved fs_resolve(float4 position: SV_Position) {
    const int2 p = int2(position.xy);
    float3 sum = float3(0.0);
    float weights = 0.0;
    float nearest = 0.0;
    for (uint i = 0; i < push.samples; ++i) {
        nearest = max(nearest, depth.Load(p, int(i)));
        const float3 c = scene.Load(p, int(i)).rgb;
        const float w = 1.0 / (1.0 + max(0.0, dot(c, float3(0.2126, 0.7152, 0.0722))));
        sum += c * w;
        weights += w;
    }
    Resolved o;
    o.colour = float4(sum / weights, 1.0);
    o.depth = nearest;
    return o;
}
