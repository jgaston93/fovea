#include "fovea/render/particle_pack.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace fovea {

namespace {

std::array<float, 3> floats3(const DVec3& v) noexcept {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

}  // namespace

std::size_t pack_particles(const ParticleFrame& frame, LocalTime at, const ParticleLook& look,
                           const DVec3& anchor, std::span<GpuParticle> out,
                           std::span<GpuOrigin> origins) noexcept {
    const std::size_t n_origins = std::min(frame.origins.size(), origins.size());
    for (std::size_t i = 0; i < n_origins; ++i) {
        const ParticleOrigin& o = frame.origins[i];
        GpuOrigin& g = origins[i];
        g.offset = floats3(o.ecef - anchor);
        g.east = floats3(o.enu_to_ecef[0]);
        g.north = floats3(o.enu_to_ecef[1]);
        g.up = floats3(o.enu_to_ecef[2]);
    }
    std::size_t n = 0;
    const std::size_t count = std::min(frame.count, frame.particles.size());
    for (std::size_t i = 0; i < count && n < out.size(); ++i) {
        const Particle& p = frame.particles[i];
        if (p.origin >= n_origins || p.radius <= 0.0F || p.bulk_density <= 0.0F) {
            continue;
        }
        const float opacity = particle_opacity_at(p, at, look.fade);
        if (opacity <= 0.0F) {
            continue;
        }
        const Vec3 pos = particle_position_at(p, at);
        GpuParticle& g = out[n++];
        g.position = {pos.x, pos.y, pos.z};
        g.radius = p.radius;
        g.extinction = p.bulk_density * look.extinction_m2_per_kg * opacity;
        g.origin = p.origin;
    }
    return n;
}

void particle_centres(std::span<const GpuParticle> particles, std::span<const GpuOrigin> origins,
                      std::span<Vec3> out) noexcept {
    const std::size_t n = std::min(particles.size(), out.size());
    for (std::size_t i = 0; i < n; ++i) {
        const GpuParticle& p = particles[i];
        const GpuOrigin& o = origins[p.origin];
        out[i] = Vec3(o.offset[0], o.offset[1], o.offset[2]) +
                 Vec3(o.east[0], o.east[1], o.east[2]) * p.position[0] +
                 Vec3(o.north[0], o.north[1], o.north[2]) * p.position[1] +
                 Vec3(o.up[0], o.up[1], o.up[2]) * p.position[2];
    }
}

GpuVolumeSpace volume_space(std::span<const Vec3> centres, std::span<const GpuParticle> particles,
                            std::uint32_t size, std::uint32_t light_size, float max_side) noexcept {
    GpuVolumeSpace v;
    v.size = std::max(size, 1U);
    v.light_size = std::max(light_size, 1U);
    Vec3 lo(0.0F);
    Vec3 hi(0.0F);
    bool any = false;
    for (std::size_t i = 0; i < centres.size() && i < particles.size(); ++i) {
        const Vec3 r(particles[i].radius);
        lo = any ? glm::min(lo, centres[i] - r) : centres[i] - r;
        hi = any ? glm::max(hi, centres[i] + r) : centres[i] + r;
        any = true;
    }
    const Vec3 mid = (lo + hi) * 0.5F;
    const Vec3 extent = hi - lo;
    float side = std::max(std::max(extent.x, extent.y), extent.z) * 1.02F;
    side = std::clamp(side, 1.0F, std::max(max_side, 1.0F));
    const Vec3 corner = mid - Vec3(side * 0.5F);
    v.corner = {corner.x, corner.y, corner.z};
    v.voxel = side / static_cast<float>(v.size);
    v.inv_voxel = 1.0F / v.voxel;
    return v;
float erosion_gain(std::span<const std::uint32_t, 256> histogram, float detail,
                   float erosion) noexcept {
    if (erosion <= 0.0F) {
        return 1.0F;
    }
    // The detail volume's mean, as the shader's kDetailMean is (its factor's
    // normaliser cancels in the ratio, so only its shape matters here).
    double count = 0.0;
    for (const std::uint32_t c : histogram) {
        count += c;
    }
    if (count <= 0.0) {
        return 1.0F;
    }
    // Over the volume: r^2 at the midpoints of 512 shells of equal volume,
    // each as likely; n by the histogram. Both sums as the shader weighs a
    // point: the base w, and eroded(w, r^2, n), times the detail's factor.
    constexpr int kShells = 512;
    double with = 0.0;
    double without = 0.0;
    for (int k = 0; k < kShells; ++k) {
        const double u = (k + 0.5) / kShells;  // the volume inside: r^3
        const double r2 = std::pow(u, 2.0 / 3.0);
        const double w = std::clamp(3.0 * (1.0 - r2), 0.0, 1.0);
        const double s = std::max(1.0 - r2, 0.0);
        for (std::size_t level = 0; level < histogram.size(); ++level) {
            if (histogram[level] == 0) {
                continue;
            }
            const double n = static_cast<double>(level) / 255.0;
            const double p = histogram[level] / count;
            const double f =
                1.0 - static_cast<double>(detail) * (1.0 - n);  // the factor, less its normaliser
            const double t = static_cast<double>(erosion) * (1.0 - n);
            const double e = w * std::clamp((s - t) / std::max(1.0 - t, 1e-3) * 4.0, 0.0, 1.0);
            with += p * e * f;
            without += p * w * f;
        }
    }
    return with > 0.0 ? static_cast<float>(without / with) : 1.0F;
}

GpuLightSpace light_space(std::span<const Vec3> centres, std::span<const GpuParticle> particles,
                          const Vec3& travel) noexcept {
    GpuLightSpace l;
    const std::size_t n = std::min(centres.size(), particles.size());
    Vec3 mean(0.0F);
    for (std::size_t i = 0; i < n; ++i) {
        mean += centres[i];
    }
    if (n > 0) {
        mean /= static_cast<float>(n);
    }
    float radius = 1.0F;
    for (std::size_t i = 0; i < n; ++i) {
        radius = std::max(radius, glm::length(centres[i] - mean) + particles[i].radius);
    }
    const Vec3 w = glm::normalize(travel);
    const Vec3 helper = std::abs(w.z) < 0.9F ? Vec3(0.0F, 0.0F, 1.0F) : Vec3(1.0F, 0.0F, 0.0F);
    const Vec3 u = glm::normalize(glm::cross(helper, w));
    const Vec3 v = glm::cross(w, u);
    l.centre = {mean.x, mean.y, mean.z};
    l.inv_radius = 1.0F / radius;
    l.u = {u.x, u.y, u.z};
    l.v = {v.x, v.y, v.z};
    l.travel = {w.x, w.y, w.z};
    return l;
}

Vec3 view_centre_ray(const Mat4& ecef_to_clip) noexcept {
    const Mat4& m = ecef_to_clip;
    // glm is column-major: row i is (m[0][i], m[1][i], m[2][i]). The ray
    // lies in both clip x's and clip y's zero planes: their normals' cross
    // product, turned to the side clip w is positive (in front).
    const Vec3 row_x(m[0][0], m[1][0], m[2][0]);
    const Vec3 row_y(m[0][1], m[1][1], m[2][1]);
    const Vec3 row_w(m[0][3], m[1][3], m[2][3]);
    Vec3 d = glm::cross(row_x, row_y);
    if (glm::dot(d, row_w) < 0.0F) {
        d = -d;
    }
    const float len = glm::length(d);
    return len > 0.0F ? d / len : glm::normalize(row_w);
}

HalfAngle half_angle(const Vec3& view_forward, const Vec3& travel) noexcept {
    // Green 2013's pseudocode tests the dot product against 1.0; its text
    // (and Kniss's) says positive, which is what's meant.
    HalfAngle h;
    const Vec3 v = glm::normalize(view_forward);
    const Vec3 l = glm::normalize(travel);
    h.front_to_back = glm::dot(v, l) > 0.0F;
    const Vec3 sum = h.front_to_back ? v + l : l - v;
    const float len = glm::length(sum);
    h.axis = len > 1e-6F ? sum / len : l;  // looking straight into the light: along it
    return h;
}

namespace {

// A float's bits, ordered as the float is (negatives flipped whole,
// positives with the sign bit set).
std::uint32_t sortable(float f) noexcept {
    const auto u = std::bit_cast<std::uint32_t>(f);
    return (u & 0x80000000U) != 0 ? ~u : u | 0x80000000U;
}

}  // namespace

void sort_along(std::span<const Vec3> centres, const Vec3& axis, std::span<std::uint32_t> keys,
                std::span<std::uint32_t> scratch, std::span<std::uint32_t> order) noexcept {
    const std::size_t n = centres.size();
    // keys and scratch each hold n keys then n indices: ping and pong.
    std::uint32_t* key_a = keys.data();
    std::uint32_t* idx_a = keys.data() + n;
    std::uint32_t* key_b = scratch.data();
    std::uint32_t* idx_b = scratch.data() + n;
    for (std::size_t i = 0; i < n; ++i) {
        key_a[i] = sortable(glm::dot(axis, centres[i]));
        idx_a[i] = static_cast<std::uint32_t>(i);
    }
    // Least significant byte first, four passes: stable, so the result is
    // in key order.
    for (unsigned shift = 0; shift < 32; shift += 8) {
        std::array<std::size_t, 257> start{};
        for (std::size_t i = 0; i < n; ++i) {
            ++start[((key_a[i] >> shift) & 0xFFU) + 1];
        }
        for (std::size_t b = 1; b < start.size(); ++b) {
            start[b] += start[b - 1];
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t at = start[(key_a[i] >> shift) & 0xFFU]++;
            key_b[at] = key_a[i];
            idx_b[at] = idx_a[i];
        }
        std::swap(key_a, key_b);
        std::swap(idx_a, idx_b);
    }
    // Four passes: the result is back in `keys`.
    std::copy(idx_a, idx_a + n, order.begin());
}

}  // namespace fovea
