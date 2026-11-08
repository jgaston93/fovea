// The clouds' noise (milestone 6 step 6, D-034), made once on the GPU:
// a shape volume (128 cubed) of Perlin-Worley noise, as Schneider's
// ("The Real-Time Volumetric Cloudscapes of Horizon Zero Dawn", 2015): its
// gradient noise's billows carved by inverted Worley noise, cellular
// shapes; and a detail volume (32 cubed) of Worley noise that erodes their
// edges into wisps. Both tile. Then a histogram of the shape's values and
// its quantiles, so a layer's coverage c thresholds it where c of it lies
// above (weather.slang's cloud shapes).

[[vk::binding(0, 0)]] RWTexture3D<float4> shape_out;
[[vk::binding(1, 0)]] RWTexture3D<float4> detail_out;
// [0, 256) the histogram's counts; [256, 512) the quantiles (as float bits).
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> table;

static const uint kShapeSize = 128;
static const uint kDetailSize = 32;

uint pcg(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

int3 wrap(int3 c, int period) {
    return ((c % period) + period) % period;
}

// A lattice point's three values, 0 to 1, wrapped to the period.
float3 hash3(int3 c, int period, uint seed) {
    const uint3 u = uint3(wrap(c, period));
    const uint h = pcg(u.x + pcg(u.y + pcg(u.z + seed)));
    return float3(float(h & 1023u), float((h >> 10) & 1023u), float((h >> 20) & 1023u)) / 1023.0;
}

// Worley: 1 less the distance to the nearest of one random point a cell
// (p in cells), so cells are bright at their points.
float worley(float3 p, int period, uint seed) {
    const int3 c = int3(floor(p));
    float nearest = 1e9;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const int3 n = c + int3(x, y, z);
                const float3 point = float3(n) + hash3(n, period, seed);
                const float3 d = p - point;
                nearest = min(nearest, dot(d, d));
            }
        }
    }
    return saturate(1.0 - sqrt(nearest));
}

// Gradient (Perlin) noise, -1 to 1 roughly, quintic between lattice points.
float perlin(float3 p, int period) {
    const int3 c = int3(floor(p));
    const float3 f = p - floor(p);
    const float3 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float corners[8];
    for (int k = 0; k < 8; ++k) {
        const int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        const float3 g = normalize(hash3(c + o, period, 7u) * 2.0 - 1.0 + 1e-4);
        corners[k] = dot(g, f - float3(o));
    }
    const float x00 = lerp(corners[0], corners[1], w.x);
    const float x10 = lerp(corners[2], corners[3], w.x);
    const float x01 = lerp(corners[4], corners[5], w.x);
    const float x11 = lerp(corners[6], corners[7], w.x);
    return lerp(lerp(x00, x10, w.y), lerp(x01, x11, w.y), w.z);
}

float remap(float v, float l0, float h0, float l1, float h1) {
    return l1 + (v - l0) * (h1 - l1) / max(h0 - l0, 1e-6);
}

// Three octaves of Worley, from `cells` a tile.
float worley_fbm(float3 p, int cells, uint seed) {
    return worley(p * cells, cells, seed) * 0.625 + worley(p * cells * 2, cells * 2, seed + 1) * 0.25 +
           worley(p * cells * 4, cells * 4, seed + 2) * 0.125;
}

[shader("compute")]
[numthreads(4, 4, 4)]
void cs_shape(uint3 id: SV_DispatchThreadID) {
    const float3 p = (float3(id) + 0.5) / float(kShapeSize);  // 0 to 1, a tile
    // Perlin fBm, 0 to 1, the billows' low frequencies.
    float perlin_sum = 0.0;
    float a = 1.0;
    int cells = 4;
    for (int o = 0; o < 4; ++o) {
        perlin_sum += a * perlin(p * cells, cells);
        a *= 0.5;
        cells *= 2;
    }
    const float pf = saturate(perlin_sum / 1.875 * 0.5 + 0.5);
    // Perlin-Worley: the Perlin noise lifted where Worley's cells are, so
    // its shapes round into cells.
    const float pw = remap(pf, 0.0, 1.0, worley_fbm(p, 4, 11u), 1.0);
    // Schneider's base cloud: carved by finer Worley.
    const float fine = worley_fbm(p, 8, 23u);
    const float base = saturate(remap(pw, fine - 1.0, 1.0, 0.0, 1.0));
    shape_out[id] = float4(base, pw, fine, 1.0);
    uint ignored;
    InterlockedAdd(table[min(uint(base * 256.0), 255u)], 1u, ignored);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void cs_detail(uint3 id: SV_DispatchThreadID) {
    const float3 p = (float3(id) + 0.5) / float(kDetailSize);
    detail_out[id] = float4(worley_fbm(p, 2, 37u), worley_fbm(p, 4, 41u), 0.0, 1.0);
}

// One thread: the quantiles from the histogram, quantile i the value i / 255
// of the shape's texels lie below.
[shader("compute")]
[numthreads(1, 1, 1)]
void cs_quantiles(uint3 id: SV_DispatchThreadID) {
    const float total = float(kShapeSize * kShapeSize * kShapeSize);
    uint bin = 0;
    float below = 0.0;  // texels in the bins before `bin`
    for (uint i = 0; i < 256; ++i) {
        const float want = float(i) / 255.0 * total;
        while (bin < 255 && below + float(table[bin]) < want) {
            below += float(table[bin]);
            ++bin;
        }
        const float in_bin = max(float(table[bin]), 1.0);
        const float v = (float(bin) + saturate((want - below) / in_bin)) / 256.0;
        table[256 + i] = asuint(v);
    }
}
