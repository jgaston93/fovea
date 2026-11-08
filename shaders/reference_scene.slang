// The milestone 1 reference scene (step 6): a flat ground grid at the eye
// height, a horizon line and the sky, for checking each channel's off-axis
// view and that lines continue across screens. No geometry: every pixel
// turns its NDC back into a view ray through the inverse of the channel's
// projection and shades what that ray sees. The sky is the atmosphere's
// (milestone 6 step 2): each view's sky-view table, and the sun's disc.
//
// Then a tone-map pass draws the linear HDR scene into the output (step 6
// keeps scene-referred light values until here: foundational decision 3).

#include "include/atmosphere.slang"
// The frame set is set 1 of the scene pass: set 0 is the tone map's.
#define FRAME_SET 1
#include "include/frame.slang"
[[vk::binding(3, 1)]] ConstantBuffer<Atmosphere> atmosphere;

struct Fullscreen {
    float4 position : SV_Position;
    float2 ndc;  // Vulkan NDC: x right, y down, in [-1, 1]
};

// One triangle covering the screen: (-1,-1), (3,-1), (-1,3). Vulkan's own
// vertex index, which (unlike SV_VertexID) needs no DrawParameters feature.
[shader("vertex")]
Fullscreen vs_fullscreen(uint id: SV_VulkanVertexID) {
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
    // The eye's offset north and east of the nearest grid crossing (within
    // half a cell): the grid repeats, so this is all the shader needs, and it
    // stays small however far the eye is. Not a position: anything that
    // doesn't repeat with the grid needs its own camera-relative input.
    float2 eye_offset_m;
};
[[vk::push_constant]] ConstantBuffer<ScenePush> scene;

static const float3 kGround = float3(0.16, 0.18, 0.12);  // albedos, lit
static const float3 kGridLine = float3(0.85, 0.85, 0.80);
static const float3 kHorizonLine = float3(1.00, 0.85, 0.50);

// Coverage of a line at every integer of g: full on the line, falling to
// zero one pixel either side (so one pixel wide at half coverage),
// antialiased with the screen-space width w of one unit of g, and faded out
// where lines get denser than a few pixels apart (towards the horizon)
// instead of aliasing.
float grid_lines(float2 g, float2 w_raw) {
    const float2 w = max(w_raw, float2(1e-6));
    const float2 distance_px = abs(frac(g + 0.5) - 0.5) / w;
    const float line = 1.0 - saturate(min(distance_px.x, distance_px.y));
    const float fade = saturate(1.0 - (max(w.x, w.y) - 0.15) * 4.0);
    return line * fade;
}

// The sky seen along a ray of the level frame (x north, y east, z down),
// from the view's sky-view table: by its zenith angle, and its azimuth from
// the sun's.
float3 sky_radiance(float3 ray) {
    const float zenith = acos(clamp(-ray.z, -1.0, 1.0));
    const float2 h = ray.xy;
    const float2 sun_h = lighting.sun_local.xy;
    float azimuth = 0.0;
    if (dot(h, h) > 1e-10 && dot(sun_h, sun_h) > 1e-10) {
        azimuth = acos(clamp(dot(normalize(h), normalize(sun_h)), -1.0, 1.0));
    }
    const float2 uv = sky_view_uv(atmosphere, lighting.eye_radius, zenith, azimuth);
    return sky_view_table.SampleLevel(float3(uv, float(lighting.view)), 0.0).rgb;
}

[shader("fragment")]
float4 fs_scene(Fullscreen i) : SV_Target {
    // Any depth works for the direction; 1 is the near plane (reverse-Z).
    // The rotation leaves w alone, so the divide still gives the point.
    const float4 p = mul(scene.clip_to_local, float4(i.ndc, 1.0, 1.0));
    const float3 ray = normalize(p.xyz / p.w);

    // Derivatives (fwidth) are taken outside any branch: inside one that
    // neighbouring pixels take differently, as along the horizon, they are
    // undefined. The ground point is computed for every pixel and used only
    // below the horizon.
    const float t = scene.eye_height_m / max(ray.z, 1e-6);
    const float2 ground_m = scene.eye_offset_m + ray.xy * t;  // north, east
    const float2 g = ground_m / scene.grid_spacing_m;
    const float2 g_width = fwidth(g);
    const float ray_z_width = fwidth(ray.z);
    // The sun's disc: its edge a pixel wide.
    const float sun_angle = acos(clamp(dot(ray, lighting.sun_local), -1.0, 1.0));
    const float sun_width = max(fwidth(sun_angle), 1e-6);

    const ViewSky s = view_sky[lighting.view];
    float3 colour;
    if (ray.z > 0.0) {
        // Below the horizon: the ground plane, eye_height_m below the eye,
        // lit as level ground.
        const float3 e = s.sun_irradiance * saturate(-lighting.sun_local.z) + s.sky_irradiance;
        colour = lerp(kGround, kGridLine, grid_lines(g, g_width)) * e * kInvPi * s.exposure;
    } else {
        colour = sky_radiance(ray) * s.exposure;
        const float r = atmosphere.sun_angular_radius;
        const float disc = saturate((r - sun_angle) / sun_width + 0.5);
        // Its radiance: the irradiance it gives over the solid angle it
        // covers. Kept within half floats (a low sun, bright against a
        // dim sky, would pass their largest).
        colour += disc * min(s.sun_irradiance * s.exposure / (kPi * r * r), float3(60000.0));
    }
    // The horizon line: where the ray is level, as wide as a grid line.
    const float level = abs(ray.z) / max(ray_z_width, 1e-6);
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
