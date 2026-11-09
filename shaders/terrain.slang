// Terrain (milestone 3 step 5): every tile drawn is one instance of a
// shared grid of (samples + 2)^2 vertices, its outer ring the skirt. The
// vertex shader builds each vertex from the tile's height grid: the
// ellipsoid point under it (bilinear in a 9 x 9 grid of points the CPU
// computed in double, relative to the tile's origin; under a centimetre
// from the true ellipsoid at these tile sizes) raised along the ellipsoid's
// normal by the height. Positions stay relative to the eye: the CPU gives
// each tile its origin's offset from the eye.

// A tile's 9 x 9 control points, row by row from the south-west: the
// ellipsoid point (xyz, relative to the tile's origin, ECEF axes) and the
// ellipsoid normal there.
struct Control {
    float4 position;
    float4 up;
};

// One tile drawn: its origin relative to the eye (ECEF axes), which tile,
// and how far its skirt hangs below its edge.
struct TileDraw {
    float3 offset;
    uint tile;
    float skirt_m;
    uint unused0;  // single uints: a uint3 would align to 16 and make this 48 bytes
    uint unused1;
    uint unused2;
};

struct TerrainPush {
    float4x4 ecef_to_clip;  // directions relative to the eye to clip space
    uint samples;           // per tile side
};
[[vk::push_constant]] ConstantBuffer<TerrainPush> push;

// Each view's lighting (milestone 6), set 1.
#define FRAME_SET 1
#include "include/frame.slang"

[[vk::binding(0, 0)]] StructuredBuffer<Control> controls;
[[vk::binding(1, 0)]] StructuredBuffer<TileDraw> draws;
[[vk::binding(2, 0)]] SamplerState imagery_sampler;
[[vk::binding(3, 0)]] Texture2D<float> heights[];
[[vk::binding(4, 0)]] Texture2D<float4> imagery[];
// The shadow maps and their sampler (step 5), in this set rather than
// the frame set's (6 and 7): lavapipe crashes compiling this fragment
// shader with any image of the frame set.
[[vk::binding(5, 0)]] Texture2DArray<float> terrain_shadow_maps;
[[vk::binding(6, 0)]] SamplerState terrain_shadow_sampler;

static const uint kControls = 9;

struct Surface {
    float3 position;  // relative to the tile's origin
    float3 up;
};

// The point at grid sample (column, row), on the ellipsoid and raised by h.
Surface surface(uint tile, int column, int row, float h) {
    const float cells = float(push.samples - 1);
    // Columns and rows may be one outside the grid (a neighbour, for the
    // normal): the edge cell then carries on linearly. Floored as signed
    // first; a negative float to uint is undefined in SPIR-V.
    const float2 g = float2(float(column), float(row)) / cells * float(kControls - 1);
    const int2 c = clamp(int2(floor(g)), int2(0), int2(int(kControls) - 2));
    const float2 f = g - float2(c);
    const uint base = tile * kControls * kControls + uint(c.y) * kControls + uint(c.x);
    const Control c00 = controls[base];
    const Control c10 = controls[base + 1];
    const Control c01 = controls[base + kControls];
    const Control c11 = controls[base + kControls + 1];
    const float3 p = lerp(lerp(c00.position.xyz, c10.position.xyz, f.x),
                          lerp(c01.position.xyz, c11.position.xyz, f.x), f.y);
    const float3 up = normalize(lerp(lerp(c00.up.xyz, c10.up.xyz, f.x),
                                     lerp(c01.up.xyz, c11.up.xyz, f.x), f.y));
    Surface s;
    s.position = p + up * h;
    s.up = up;
    return s;
}

float height_at(uint tile, int column, int row) {
    const int last = int(push.samples) - 1;
    return heights[NonUniformResourceIndex(tile)].Load(
        int3(clamp(column, 0, last), clamp(row, 0, last), 0));
}

struct TerrainVertex {
    float4 position : SV_Position;
    float2 uv;
    float3 normal;
    float4 air;  // the air between it and the eye (aerial_perspective)
    float3 rel;  // relative to the eye, ECEF axes, metres (for the shadows)
    nointerpolation uint tile;
};

// A vertex of a tile: where it is relative to the eye (ECEF axes, metres),
// its normal and its place in the tile's imagery.
struct TerrainPoint {
    float3 rel;
    float3 normal;
    float2 uv;
    uint tile;
};

TerrainPoint terrain_point(uint vertex, uint instance) {
    const TileDraw d = draws[instance];
    const int n = int(push.samples);
    const int side = n + 2;
    const int column = int(vertex % uint(side)) - 1;
    const int row = int(vertex / uint(side)) - 1;
    const int cc = clamp(column, 0, n - 1);
    const int rc = clamp(row, 0, n - 1);
    const bool skirt = cc != column || rc != row;

    Surface s = surface(d.tile, cc, rc, height_at(d.tile, cc, rc));
    // The normal from the neighbouring samples (east minus west, north
    // minus south): east cross north is up.
    const float3 e = surface(d.tile, cc + 1, rc, height_at(d.tile, cc + 1, rc)).position;
    const float3 w = surface(d.tile, cc - 1, rc, height_at(d.tile, cc - 1, rc)).position;
    const float3 no = surface(d.tile, cc, rc + 1, height_at(d.tile, cc, rc + 1)).position;
    const float3 so = surface(d.tile, cc, rc - 1, height_at(d.tile, cc, rc - 1)).position;
    if (skirt) {
        s.position -= s.up * d.skirt_m;
    }
    TerrainPoint p;
    p.rel = d.offset + s.position;
    p.normal = normalize(cross(e - w, no - so));
    // Texel centres lie inside the tile: its edges are the texture's edges,
    // row 0 at the south as in the grid.
    p.uv = float2(float(cc), float(rc)) / float(n - 1);
    p.tile = d.tile;
    return p;
}

[shader("vertex")]
TerrainVertex vs_terrain(uint vertex: SV_VulkanVertexID, uint instance: SV_VulkanInstanceID) {
    const TerrainPoint p = terrain_point(vertex, instance);
    TerrainVertex o;
    o.position = mul(push.ecef_to_clip, float4(p.rel, 1.0));
    // Through the air to the eye (step 3), at each vertex: terrain's
    // triangles are a few pixels across, where the haze barely changes.
    // (In the fragment shader, any image of the frame set makes lavapipe,
    // Mesa 25.2.8 with LLVM 20, crash compiling this shader; models and the
    // sky look it up per pixel.)
    // A vertex behind the eye has no place on the screen: it is near (a
    // triangle passing under the eye), where the haze is slight, and takes
    // the middle column's. Off the screen, the edge columns serve.
    const float2 uv =
        o.position.w > 1e-6 ? o.position.xy / o.position.w * 0.5 + 0.5 : float2(0.5, 0.5);
    o.air = aerial_perspective_uv(uv, length(p.rel) * 0.001);
    o.uv = p.uv;
    o.normal = p.normal;
    o.rel = p.rel;
    o.tile = p.tile;
    return o;
}

// The shadow maps (step 5): depth only, push's matrix taking points
// relative to the eye to the cascade's map.
[shader("vertex")]
float4 vs_terrain_shadow(uint vertex: SV_VulkanVertexID, uint instance: SV_VulkanInstanceID)
    : SV_Position {
    return mul(push.ecef_to_clip, float4(terrain_point(vertex, instance).rel, 1.0));
}

[shader("fragment")]
float4 fs_terrain(TerrainVertex i) : SV_Target {
    // sRGB textures: the sample is linear already.
    const float3 albedo =
        imagery[NonUniformResourceIndex(i.tile)].Sample(imagery_sampler, i.uv).rgb;
    // The photographs carry their own shading: the sun lights them mostly
    // as level ground, a quarter by the slope's own angle, so they stay
    // close to what was photographed while the light's strength and colour
    // follow the sun and sky. Pre-exposed (D-031).
    const ViewSky s = view_sky[lighting.view];
    const float level = saturate(-lighting.sun_local.z);
    const float slope = saturate(dot(normalize(i.normal), lighting.sun));
    const float moon_level = saturate(-lighting.moon_local.z);
    const float moon_slope = saturate(dot(normalize(i.normal), lighting.moon));
    // In the shadows of models, and of terrain when the light is low (step
    // 5, D-032: the photographs hold the rest).
    const float2 shade = shadowed(
        shadow_lit(terrain_shadow_maps, terrain_shadow_sampler, i.rel, normalize(i.normal),
                   ddx(i.rel), ddy(i.rel)));
    // Under rain or snow, their overcast's light alone (step 6).
    const float kind = precipitation_over(i.rel);
    const float3 e = kind > 0.5
                         ? overcast_light(s, kind)
                         : s.sun_irradiance * lerp(level, slope, 0.25) * shade.x +
                               s.moon_irradiance * lerp(moon_level, moon_slope, 0.25) * shade.y +
                               s.sky_irradiance;
    // Through the air, then the fog, rain and snow (step 6, D-034).
    const float3 lit = albedo * e * kInvPi * i.air.a + i.air.rgb;
    const float dist = length(i.rel);
    const float mu = dot(i.rel, lighting.up) / max(dist, 1e-3);
    return float4(through_bands(lit, mu, dist, s, 1.0) * s.exposure, 1.0);
}
