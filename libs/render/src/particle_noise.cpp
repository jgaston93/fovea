#include "fovea/render/particle_noise.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fovea {

namespace {

// A fixed shuffle of 0..255 (a linear congruential generator, Fisher-Yates),
// doubled as Perlin's p[] is so p[i + 1] needs no wrap.
const std::array<int, 512>& permutation() {
    static const std::array<int, 512> p = [] {
        std::array<int, 256> base{};
        for (int i = 0; i < 256; ++i) {
            base.at(static_cast<std::size_t>(i)) = i;
        }
        std::uint32_t state = 0x2545F491U;
        for (int i = 255; i > 0; --i) {
            state = state * 1664525U + 1013904223U;
            const auto j =
                static_cast<std::size_t>((state >> 8) % static_cast<std::uint32_t>(i + 1));
            std::swap(base.at(static_cast<std::size_t>(i)), base.at(j));
        }
        std::array<int, 512> doubled{};
        for (std::size_t i = 0; i < 256; ++i) {
            doubled.at(i) = doubled.at(i + 256) = base.at(i);
        }
        return doubled;
    }();
    return p;
}

// Perlin's: 6t^5 - 15t^4 + 10t^3.
float fade(float t) {
    return t * t * t * (t * (t * 6.0F - 15.0F) + 10.0F);
}

float lerp(float t, float a, float b) {
    return a + t * (b - a);
}

// Perlin's: the hash's low 4 bits pick one of 12 gradient directions.
float grad(int hash, float x, float y, float z) {
    const int h = hash & 15;
    const float u = h < 8 ? x : y;
    float v = z;  // his: h<4 ? y : h==12||h==14 ? x : z
    if (h < 4) {
        v = y;
    } else if (h == 12 || h == 14) {
        v = x;
    }
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

int wrap(int i, int period) {
    const int m = i % period;
    return m < 0 ? m + period : m;
}

// A cell's point, 0 to 1 in each axis, from its coordinates.
Vec3 cell_point(int x, int y, int z) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093U ^
                      static_cast<std::uint32_t>(y) * 19349663U ^
                      static_cast<std::uint32_t>(z) * 83492791U;
    Vec3 out;
    for (int k = 0; k < 3; ++k) {
        h ^= h >> 13U;
        h *= 0x5BD1E995U;
        h ^= h >> 15U;
        out[k] = static_cast<float>(h & 0xFFFFU) / 65535.0F;
    }
    return out;
}

}  // namespace

float periodic_perlin(const Vec3& p, int period, int variant) noexcept {
    const std::array<int, 512>& perm = permutation();
    const auto at = [&](int i) { return perm.at(static_cast<std::size_t>(i)); };
    const int xi = static_cast<int>(std::floor(p.x));
    const int yi = static_cast<int>(std::floor(p.y));
    const int zi = static_cast<int>(std::floor(p.z));
    const float x = p.x - static_cast<float>(xi);
    const float y = p.y - static_cast<float>(yi);
    const float z = p.z - static_cast<float>(zi);
    // Perlin's corner hashes, with each lattice coordinate and its +1 wrapped
    // at the period (his: & 255, and p[] doubled for the +1); the variant
    // offsets the first lookup (still within the doubled table: a < 256).
    const int X0 = wrap(xi, period);
    const int X1 = wrap(xi + 1, period);
    const int Y0 = wrap(yi, period);
    const int Y1 = wrap(yi + 1, period);
    const int Z0 = wrap(zi, period);
    const int Z1 = wrap(zi + 1, period);
    const int v0 = variant & 255;
    const auto hash = [&](int a, int b, int c) { return at(at(at(a + v0) + b) + c); };
    const float u = fade(x);
    const float v = fade(y);
    const float w = fade(z);
    return lerp(
        w,
        lerp(v, lerp(u, grad(hash(X0, Y0, Z0), x, y, z), grad(hash(X1, Y0, Z0), x - 1.0F, y, z)),
             lerp(u, grad(hash(X0, Y1, Z0), x, y - 1.0F, z),
                  grad(hash(X1, Y1, Z0), x - 1.0F, y - 1.0F, z))),
        lerp(v,
             lerp(u, grad(hash(X0, Y0, Z1), x, y, z - 1.0F),
                  grad(hash(X1, Y0, Z1), x - 1.0F, y, z - 1.0F)),
             lerp(u, grad(hash(X0, Y1, Z1), x, y - 1.0F, z - 1.0F),
                  grad(hash(X1, Y1, Z1), x - 1.0F, y - 1.0F, z - 1.0F))));
}

float periodic_cellular(const Vec3& p, int period) noexcept {
    const int xi = static_cast<int>(std::floor(p.x));
    const int yi = static_cast<int>(std::floor(p.y));
    const int zi = static_cast<int>(std::floor(p.z));
    float nearest = 3.0F;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = xi + dx;
                const int cy = yi + dy;
                const int cz = zi + dz;
                const Vec3 point =
                    Vec3(static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(cz)) +
                    cell_point(wrap(cx, period), wrap(cy, period), wrap(cz, period));
                nearest = std::min(nearest, glm::length(point - p));
            }
        }
    }
    return std::clamp(1.0F - nearest, 0.0F, 1.0F);  // F1 can pass 1 (about 0.07 % of points): 0
}

std::vector<float> detail_volume(int size, int cells, int octaves) {
    std::vector<float> out(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) *
                           static_cast<std::size_t>(size));
    float total = 0.0F;
    for (int o = 0; o < octaves; ++o) {
        total += std::ldexp(1.0F, -o);
    }
    std::size_t i = 0;
    for (int z = 0; z < size; ++z) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float sum = 0.0F;
                for (int o = 0; o < octaves; ++o) {
                    const int c = cells << o;
                    const float s = static_cast<float>(c) / static_cast<float>(size);
                    sum += std::ldexp(1.0F, -o) *
                           periodic_cellular(
                               Vec3(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F,
                                    static_cast<float>(z) + 0.5F) *
                                   s,
                               c);
                }
                out[i++] = sum / total;
            }
        }
    }
    // Stretched to 0 to 1: the octaves' average spans only about 0.13 to
    // 0.89 of it, and erosion wants the whole range.
    const auto [lo, hi] = std::minmax_element(out.begin(), out.end());
    const float low = *lo;
    const float span = *hi - *lo;
    if (span > 0.0F) {
        for (float& x : out) {
            x = (x - low) / span;
        }
    }
    return out;
}

std::vector<Vec3> curl_volume(int size, int cells, int octaves) {
    // Bridson et al. 2.2: three apparently uncorrelated noises for the
    // potential's three components. They suggest "the same noise function
    // evaluated at large offsets"; this noise wraps, so an offset of whole
    // periods gives the same function back, and the curl of (f, f, f) only
    // swirls across (1, 1, 1). Three variants of it instead.
    const auto potential = [&](const Vec3& q) {
        Vec3 psi(0.0F);
        for (int o = 0; o < octaves; ++o) {
            const int c = cells << o;
            const Vec3 at = q * static_cast<float>(c);
            const float a = std::ldexp(1.0F, -o);
            psi.x += a * periodic_perlin(at, c, 0);
            psi.y += a * periodic_perlin(at, c, 101);
            psi.z += a * periodic_perlin(at, c, 211);
        }
        return psi;
    };
    // Their equation 1, the partial derivatives by central differences with
    // a small step (they use 1e-4 of the domain).
    const float h = 1e-3F / static_cast<float>(cells << (octaves - 1));
    std::vector<Vec3> out(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) *
                          static_cast<std::size_t>(size));
    float largest = 0.0F;
    std::size_t i = 0;
    for (int z = 0; z < size; ++z) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const Vec3 q = Vec3(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F,
                                    static_cast<float>(z) + 0.5F) /
                               static_cast<float>(size);
                const auto d = [&](const Vec3& e) {
                    return (potential(q + e) - potential(q - e)) / (2.0F * h);
                };
                const Vec3 dx = d(Vec3(h, 0.0F, 0.0F));
                const Vec3 dy = d(Vec3(0.0F, h, 0.0F));
                const Vec3 dz = d(Vec3(0.0F, 0.0F, h));
                const Vec3 v(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x);
                largest = std::max(largest, glm::length(v));
                out[i++] = v;
            }
        }
    }
    if (largest > 0.0F) {
        for (Vec3& v : out) {
            v /= largest;
        }
    }
    return out;
}

}  // namespace fovea
