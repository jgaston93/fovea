// The milestone 1 reference scene (step 6): a flat ground grid at the eye
// height, a horizon line and a sky gradient, for checking each channel's
// off-axis view and that lines continue across screens. No geometry: every
// pixel turns its NDC back into a view ray through the inverse of the
// channel's projection and shades what that ray sees.
//
// Then a tone-map pass draws the linear HDR scene into the output (step 6
// keeps scene-referred light values until here: foundational decision 3).

struct Fullscreen {
    float4 position : SV_Position;
    float2 ndc;  // Vulkan NDC: x right, y down, in [-1, 1]
};

// One triangle covering the screen: (-1,-1), (3,-1), (-1,3).
[shader("vertex")]
Fullscreen vs_fullscreen(uint id: SV_VertexID) {
    const float2 p = float2(float((id << 1) & 2), float(id & 2));
    Fullscreen o;
    o.ndc = p * 2.0 - 1.0;
    o.position = float4(o.ndc, 0.0, 1.0);
    return o;
}

struct ScenePush {
    // Clip space to the local level frame (x north, y east, z down): the
    // eye's rotation times the inverse projection. One matrix keeps the
    // block within the 128 bytes of push constants every GPU has.
    float4x4 clip_to_local;
    float eye_height_m;
    float grid_spacing_m;
    float2 unused;
};
[[vk::push_constant]] ConstantBuffer<ScenePush> scene;

static const float3 kSkyHorizon = float3(0.55, 0.65, 0.80);
static const float3 kSkyZenith = float3(0.10, 0.22, 0.55);
static const float3 kGround = float3(0.16, 0.18, 0.12);
static const float3 kGridLine = float3(0.85, 0.85, 0.80);
static const float3 kHorizonLine = float3(1.00, 0.85, 0.50);

// Coverage of a 1-pixel-wide line at every integer of g, antialiased with
// the screen-space derivative, and faded out where lines get denser than a
// few pixels apart (towards the horizon) instead of aliasing.
float grid_lines(float2 g) {
    const float2 w = max(fwidth(g), float2(1e-6));
    const float2 distance_px = abs(frac(g + 0.5) - 0.5) / w;
    const float line = 1.0 - saturate(min(distance_px.x, distance_px.y));
    const float fade = saturate(1.0 - (max(w.x, w.y) - 0.15) * 4.0);
    return line * fade;
}

[shader("fragment")]
float4 fs_scene(Fullscreen i) : SV_Target {
    // Any depth works for the direction; 1 is the near plane (reverse-Z).
    // The rotation leaves w alone, so the divide still gives the point.
    const float4 p = mul(scene.clip_to_local, float4(i.ndc, 1.0, 1.0));
    const float3 ray = normalize(p.xyz / p.w);

    float3 colour;
    if (ray.z > 0.0) {
        // Below the horizon: the ground plane, eye_height_m below the eye.
        const float t = scene.eye_height_m / ray.z;
        const float2 ground_m = ray.xy * t;  // north, east
        colour = lerp(kGround, kGridLine, grid_lines(ground_m / scene.grid_spacing_m));
    } else {
        const float up = saturate(-ray.z);
        colour = lerp(kSkyHorizon, kSkyZenith, sqrt(up));
    }
    // The horizon line: where the ray is level, about one pixel wide.
    const float level = abs(ray.z) / max(fwidth(ray.z), 1e-6);
    colour = lerp(colour, kHorizonLine, 1.0 - saturate(level));
    return float4(colour, 1.0);
}

struct TonemapPush {
    float exposure;     // linear multiplier (2^EV)
    uint encode_srgb;   // 1: the target stores what it gets (UNORM), so encode here
    float2 unused;
};
[[vk::push_constant]] ConstantBuffer<TonemapPush> tonemap;
[[vk::binding(0, 0)]] Sampler2D<float4> hdr_scene;

// A close fit to the ACES filmic curve (K. Narkowicz, 2015).
float3 aces(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float3 encode_srgb(float3 c) {
    return select(c <= 0.0031308, 12.92 * c, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055);
}

[shader("fragment")]
float4 fs_tonemap(Fullscreen i) : SV_Target {
    const float2 uv = i.ndc * 0.5 + 0.5;
    const float3 mapped = aces(hdr_scene.Sample(uv).rgb * tonemap.exposure);
    return float4(tonemap.encode_srgb != 0 ? encode_srgb(mapped) : mapped, 1.0);
}
