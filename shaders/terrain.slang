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
    float3 sun;             // towards the sun, ECEF axes
    uint samples;           // per tile side
};
[[vk::push_constant]] ConstantBuffer<TerrainPush> push;

[[vk::binding(0, 0)]] StructuredBuffer<Control> controls;
[[vk::binding(1, 0)]] StructuredBuffer<TileDraw> draws;
[[vk::binding(2, 0)]] SamplerState imagery_sampler;
[[vk::binding(3, 0)]] Texture2D<float> heights[];
[[vk::binding(4, 0)]] Texture2D<float4> imagery[];

static const uint kControls = 9;

struct Surface {
    float3 position;  // relative to the tile's origin
    float3 up;
};

// The point at grid sample (column, row), on the ellipsoid and raised by h.
Surface surface(uint tile, int column, int row, float h) {
    const float cells = float(push.samples - 1);
    const float2 g = float2(float(column), float(row)) / cells * float(kControls - 1);
    const uint2 c = min(uint2(g), uint2(kControls - 2));
    const float2 f = g - float2(c);
    const uint base = tile * kControls * kControls + c.y * kControls + c.x;
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
    nointerpolation uint tile;
};

[shader("vertex")]
TerrainVertex vs_terrain(uint vertex: SV_VulkanVertexID, uint instance: SV_VulkanInstanceID) {
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
    const float3 normal = normalize(cross(e - w, no - so));
    if (skirt) {
        s.position -= s.up * d.skirt_m;
    }

    TerrainVertex o;
    o.position = mul(push.ecef_to_clip, float4(d.offset + s.position, 1.0));
    // Texel centres lie inside the tile: its edges are the texture's edges,
    // row 0 at the south as in the grid.
    o.uv = float2(float(cc), float(rc)) / float(n - 1);
    o.normal = normal;
    o.tile = d.tile;
    return o;
}

[shader("fragment")]
float4 fs_terrain(TerrainVertex i) : SV_Target {
    // sRGB textures: the sample is linear already.
    const float3 albedo =
        imagery[NonUniformResourceIndex(i.tile)].Sample(imagery_sampler, i.uv).rgb;
    // The photographs carry their own shading; the sun only adds a little
    // relief, so lighting stays close to what was photographed.
    const float sun = saturate(dot(normalize(i.normal), push.sun));
    return float4(albedo * (0.75 + 0.25 * sun), 1.0);
}
