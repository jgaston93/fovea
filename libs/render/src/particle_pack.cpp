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

float erf_approx(float x) noexcept {
    // Abramowitz and Stegun, Handbook of Mathematical Functions, 7.1.26 (from
    // memory; its error, under 1.5e-7 there, tested here against std::erf).
    const float a = std::abs(x);
    const float t = 1.0F / (1.0F + 0.3275911F * a);
    const float poly =
        t * (0.254829592F +
             t * (-0.284496736F + t * (1.421413741F + t * (-1.453152027F + t * 1.061405429F))));
    const float y = 1.0F - poly * std::exp(-a * a);
    return x < 0.0F ? -y : y;
}

bool whitening(const std::array<float, 6>& cov, std::array<float, 6>& w,
               float& sigma_bound) noexcept {
    const auto ee = static_cast<double>(cov[0]);
    const auto en = static_cast<double>(cov[1]);
    const auto eu = static_cast<double>(cov[2]);
    const auto nn = static_cast<double>(cov[3]);
    const auto nu = static_cast<double>(cov[4]);
    const auto uu = static_cast<double>(cov[5]);
    if (!(ee > 0.0)) {
        return false;
    }
    // Sigma = L L^T, L = [[a, 0, 0], [b, c, 0], [d, e, f]].
    const double a = std::sqrt(ee);
    const double b = en / a;
    const double d = eu / a;
    const double c2 = nn - b * b;
    if (!(c2 > 0.0)) {
        return false;
    }
    const double c = std::sqrt(c2);
    const double e = (nu - d * b) / c;
    const double f2 = uu - d * d - e * e;
    if (!(f2 > 0.0) || !(a * c * std::sqrt(f2) > 1e-6)) {
        return false;
    }
    const double f = std::sqrt(f2);
    // W = L^-1, lower triangular too.
    w = {static_cast<float>(1.0 / a),      static_cast<float>(-b / (a * c)),
         static_cast<float>(1.0 / c),      static_cast<float>((b * e - c * d) / (a * c * f)),
         static_cast<float>(-e / (c * f)), static_cast<float>(1.0 / f)};
    // Gershgorin: every eigenvalue under the largest row's diagonal plus its
    // off-diagonals' magnitudes.
    const double gersh =
        std::max({ee + std::abs(en) + std::abs(eu), nn + std::abs(en) + std::abs(nu),
                  uu + std::abs(eu) + std::abs(nu)});
    sigma_bound = static_cast<float>(std::sqrt(gersh));
    return true;
}

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
    constexpr float kTwoPi32 = 15.7496099F;  // (2 pi)^1.5
    const float floor2 = kSigmaFloorM * kSigmaFloorM;
    std::size_t n = 0;
    const std::size_t count = std::min(frame.count, frame.particles.size());
    for (std::size_t i = 0; i < count && n < out.size(); ++i) {
        const Particle& p = frame.particles[i];
        // Its extinction: a cluster's, or a puff's mass times the coefficient.
        const float extinction =
            p.extinction_m2 > 0.0F ? p.extinction_m2 : p.mass_kg * look.extinction_m2_per_kg;
        if (p.origin >= n_origins || !(extinction > 0.0F)) {
            continue;
        }
        const float opacity = particle_opacity_at(p, at, look.fade);
        if (opacity <= 0.0F) {
            continue;
        }
        std::array<float, 6> cov = particle_covariance_at(p, at);
        cov[0] += floor2;
        cov[3] += floor2;
        cov[5] += floor2;
        std::array<float, 6> w{};
        float sigma = 0.0F;
        if (!whitening(cov, w, sigma) || 3.0F * sigma > kMaxBoundM) {
            continue;
        }
        const Vec3 pos = particle_position_at(p, at);
        GpuParticle& g = out[n++];
        g.position = {pos.x, pos.y, pos.z};
        g.radius = 3.0F * sigma;
        g.extinction = extinction * opacity;
        g.peak = g.extinction * w[0] * w[2] * w[5] / kTwoPi32;
        g.origin = p.origin;
        g.whiten = w;
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
                            std::uint32_t size, std::uint32_t light_size, float max_side,
                            const DVec3& anchor) noexcept {
    GpuVolumeSpace v;
    v.size = std::max(size, 1U);
    v.light_size = std::max(light_size, 1U);
    Vec3 lo(0.0F);
    Vec3 hi(0.0F);
    bool any = false;
    double extinction_sum = 0.0;
    std::size_t counted = 0;
    for (std::size_t i = 0; i < centres.size() && i < particles.size(); ++i) {
        const Vec3 r(particles[i].radius);
        extinction_sum += static_cast<double>(particles[i].peak);
        ++counted;
        lo = any ? glm::min(lo, centres[i] - r) : centres[i] - r;
        hi = any ? glm::max(hi, centres[i] + r) : centres[i] + r;
        any = true;
    }
    // Still in the world from frame to frame, so a particle lands in the
    // same voxels while it stays put (a box fitted afresh each frame moved
    // its grid by fractions of a voxel, and the cloud flickered): the voxel
    // a power of two of metres, the smallest that holds the bounds and a
    // voxel's margin each side, and the corner on a multiple of it in ECEF
    // (in doubles: the anchor is millions of metres out).
    const Vec3 mid = (lo + hi) * 0.5F;
    const Vec3 extent = hi - lo;
    const auto need = static_cast<double>(std::max(std::max(extent.x, extent.y), extent.z));
    const auto count = static_cast<double>(v.size);
    double voxel = std::exp2(std::ceil(std::log2(std::max(need, 1.0) / (count - 2.0))));
    voxel = std::min(
        voxel,
        std::exp2(std::floor(std::log2(std::max(static_cast<double>(max_side), 1.0) / count))));
    const double side = voxel * count;
    const DVec3 world_low = anchor + DVec3(mid) - DVec3(side * 0.5);
    const DVec3 snapped = glm::floor(world_low / voxel) * voxel;
    const DVec3 corner = snapped - anchor;
    v.corner = {static_cast<float>(corner.x), static_cast<float>(corner.y),
                static_cast<float>(corner.z)};
    v.voxel = static_cast<float>(voxel);
    v.inv_voxel = static_cast<float>(1.0 / voxel);
    // The erosion's scale (D-052): the particles' mean peak extinction
    // coefficient, a lone Gaussian's density at its centre.
    const double mean = counted > 0 ? extinction_sum / static_cast<double>(counted) : 0.0;
    v.inv_reference = mean > 0.0 ? static_cast<float>(1.0 / mean) : 1.0F;
    return v;
}

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
        // A Gaussian out to 3 sigma (D-053), shells of equal volume: rho^2
        // in sigmas; the density's profile w, 1 at the centre, the erosion's
        // s the same (eroded where it's thin).
        const double u = (k + 0.5) / kShells;
        const double r2 = 9.0 * std::pow(u, 2.0 / 3.0);
        const double w = std::exp(-0.5 * r2);
        const double s = w;
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

float volume_erosion_gain(std::span<const std::uint32_t, 256> histogram, float detail,
                          float erosion) noexcept {
    if (erosion <= 0.0F) {
        return 1.0F;
    }
    // The histogram in 32 bins (each its mean level), so the double sum
    // over the fine and the coarse field stays small.
    constexpr std::size_t kBins = 32;
    std::array<double, kBins> p{};
    std::array<double, kBins> level{};
    double count = 0.0;
    for (std::size_t k = 0; k < histogram.size(); ++k) {
        const std::size_t b = k * kBins / histogram.size();
        p[b] += histogram[k];
        level[b] += static_cast<double>(histogram[k]) * static_cast<double>(k) / 255.0;
        count += histogram[k];
    }
    if (count <= 0.0) {
        return 1.0F;
    }
    double mean = 0.0;
    for (std::size_t b = 0; b < kBins; ++b) {
        level[b] = p[b] > 0.0 ? level[b] / p[b] : 0.0;
        p[b] /= count;
        mean += p[b] * level[b];
    }
    const auto d = static_cast<double>(detail);
    const auto e = static_cast<double>(erosion);
    const double normaliser = std::max(1.0 - d * (1.0 - mean), 1e-6);
    // A lone particle, as particles.slang's fs_volume has it: its density
    // over the particles' mean peak, w times the fine field's factor; s = 1 -
    // exp(-2 that); t from the coarse field, independent of the fine.
    constexpr int kShells = 128;
    double with = 0.0;
    double without = 0.0;
    for (int k = 0; k < kShells; ++k) {
        const double u = (k + 0.5) / kShells;
        // A lone Gaussian (D-053) out to 3 sigma, its density over the
        // particles' mean peak: 1 at its centre.
        const double r2 = 9.0 * std::pow(u, 2.0 / 3.0);
        const double w = std::exp(-0.5 * r2);
        for (std::size_t a = 0; a < kBins; ++a) {
            if (p[a] == 0.0) {
                continue;
            }
            const double sigma = w * (1.0 - d * (1.0 - level[a])) / normaliser;
            const double s = 1.0 - std::exp(-2.0 * sigma);
            without += p[a] * sigma;
            for (std::size_t b = 0; b < kBins; ++b) {
                if (p[b] == 0.0) {
                    continue;
                }
                const double t = e * (1.0 - level[b]);
                with += p[a] * p[b] * sigma *
                        std::clamp((s - t) / std::max(1.0 - t, 1e-3) * 4.0, 0.0, 1.0);
            }
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
