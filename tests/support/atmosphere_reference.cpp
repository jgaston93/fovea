#include "support/atmosphere_reference.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fovea::test {

namespace {

constexpr double kPi = std::numbers::pi;

Rgb operator+(const Rgb& a, const Rgb& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Rgb operator*(const Rgb& a, const Rgb& b) {
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}
Rgb operator*(const Rgb& a, double s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
Rgb exp_neg(const Rgb& a) {
    return {std::exp(-a[0]), std::exp(-a[1]), std::exp(-a[2])};
}

double distance_to_sphere(double r, double mu, double radius) {
    const double d = r * r * (mu * mu - 1.0) + radius * radius;
    return d < 0.0 ? -1.0 : -r * mu + std::sqrt(d);
}

// As the shaders: below the horizon by the angle, not by the sign of a
// distance that rounds to -0 at the ground itself.
double distance_to_ground(double r, double mu, double bottom) {
    if (mu >= -std::sqrt(std::max(1.0 - (bottom / r) * (bottom / r), 0.0))) {
        return -1.0;
    }
    const double d = (r * mu) * (r * mu) - (r - bottom) * (r + bottom);
    return std::max(-r * mu - std::sqrt(std::max(d, 0.0)), 0.0);
}

double unit_to_texel(double x, double size) {
    return 0.5 / size + x * (1.0 - 1.0 / size);
}
double texel_to_unit(double u, double size) {
    return (u - 0.5 / size) / (1.0 - 1.0 / size);
}

// Bruneton's transmittance mapping, for a table of the given size.
std::array<double, 2> t_uv(const Atmosphere& a, double r, double mu, double w, double h_size) {
    const double h =
        std::sqrt(a.top_radius_km * a.top_radius_km - a.bottom_radius_km * a.bottom_radius_km);
    const double rho = std::sqrt(std::max(r * r - a.bottom_radius_km * a.bottom_radius_km, 0.0));
    const double d = std::max(distance_to_sphere(r, mu, a.top_radius_km), 0.0);
    const double d_min = a.top_radius_km - r;
    const double d_max = rho + h;
    return {unit_to_texel((d - d_min) / (d_max - d_min), w), unit_to_texel(rho / h, h_size)};
}

std::array<double, 2> t_r_mu(const Atmosphere& a, double u, double v, double w, double h_size) {
    const double x_mu = texel_to_unit(u, w);
    const double x_r = texel_to_unit(v, h_size);
    const double h =
        std::sqrt(a.top_radius_km * a.top_radius_km - a.bottom_radius_km * a.bottom_radius_km);
    const double rho = h * x_r;
    const double r = std::sqrt(rho * rho + a.bottom_radius_km * a.bottom_radius_km);
    const double d_min = a.top_radius_km - r;
    const double d_max = rho + h;
    const double d = d_min + x_mu * (d_max - d_min);
    const double mu =
        d == 0.0 ? 1.0 : std::clamp((h * h - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
    return {r, mu};
}

// Bilinear, clamped to the edge texels, as the GPU's sampler.
Rgb bilinear(const std::vector<Rgb>& table, int w, int h, double u, double v) {
    const double x = std::clamp(u * w - 0.5, 0.0, w - 1.0);
    const double y = std::clamp(v * h - 0.5, 0.0, h - 1.0);
    const int x0 = std::min(static_cast<int>(x), w - 2);
    const int y0 = std::min(static_cast<int>(y), h - 2);
    const double fx = x - x0;
    const double fy = y - y0;
    const auto at = [&](int i, int j) { return table[static_cast<std::size_t>(j * w + i)]; };
    return at(x0, y0) * ((1 - fx) * (1 - fy)) + at(x0 + 1, y0) * (fx * (1 - fy)) +
           at(x0, y0 + 1) * ((1 - fx) * fy) + at(x0 + 1, y0 + 1) * (fx * fy);
}

double rayleigh_phase(double c) {
    return 3.0 / (16.0 * kPi) * (1.0 + c * c);
}

double mie_phase(double g, double c) {
    const double g2 = g * g;
    return 3.0 / (8.0 * kPi) * (1.0 - g2) * (1.0 + c * c) /
           ((2.0 + g2) * std::pow(1.0 + g2 - 2.0 * g * c, 1.5));
}

}  // namespace

AtmosphereReference::Medium AtmosphereReference::medium_at(double height_km) const {
    const double h = std::max(height_km, 0.0);
    const double rd = std::exp(-h / a_.rayleigh_scale_height_km);
    const double md = std::exp(-h / a_.mie_scale_height_km);
    const double od =
        std::max(0.0, 1.0 - std::abs(h - a_.ozone_centre_km) / a_.ozone_half_width_km);
    Medium m;
    m.rayleigh = Rgb{a_.rayleigh_scattering} * rd;
    m.mie = a_.mie_scattering * md;
    m.scattering = m.rayleigh + Rgb{m.mie, m.mie, m.mie};
    m.extinction = m.rayleigh +
                   Rgb{a_.mie_extinction * md, a_.mie_extinction * md, a_.mie_extinction * md} +
                   Rgb{a_.ozone_absorption} * od;
    return m;
}

Rgb AtmosphereReference::transmittance(double r, double mu) const {
    const double path = std::max(distance_to_sphere(r, mu, a_.top_radius_km), 0.0);
    const int steps = 2000;
    const double dt = path / steps;
    Rgb depth{};
    for (int i = 0; i < steps; ++i) {
        const double t = (i + 0.5) * dt;
        const double ri = std::sqrt(r * r + t * t + 2.0 * r * mu * t);
        depth = depth + medium_at(ri - a_.bottom_radius_km).extinction * dt;
    }
    return exp_neg(depth);
}

Rgb AtmosphereReference::sun_transmittance(double r, double mu_s) const {
    if (distance_to_ground(r, mu_s, a_.bottom_radius_km) >= 0.0) {
        return {};
    }
    const auto uv = t_uv(a_, r, mu_s, t_w_, t_h_);
    return bilinear(t_table_, t_w_, t_h_, uv[0], uv[1]);
}

Rgb AtmosphereReference::multi_scattering(double r, double mu_s) const {
    const double x = std::clamp(mu_s * 0.5 + 0.5, 0.0, 1.0);
    const double y =
        std::clamp((r - a_.bottom_radius_km) / (a_.top_radius_km - a_.bottom_radius_km), 0.0, 1.0);
    return bilinear(ms_table_, ms_n_, ms_n_, unit_to_texel(x, ms_n_), unit_to_texel(y, ms_n_));
}

AtmosphereReference::AtmosphereReference(const Atmosphere& a) : a_(a) {
    t_table_.resize(static_cast<std::size_t>(t_w_ * t_h_));
    for (int j = 0; j < t_h_; ++j) {
        for (int i = 0; i < t_w_; ++i) {
            const auto rm = t_r_mu(a_, (i + 0.5) / t_w_, (j + 0.5) / t_h_, t_w_, t_h_);
            // Fewer steps than transmittance(): a table of 65,536.
            const double path = std::max(distance_to_sphere(rm[0], rm[1], a_.top_radius_km), 0.0);
            const int steps = 400;
            const double dt = path / steps;
            Rgb depth{};
            for (int k = 0; k < steps; ++k) {
                const double t = (k + 0.5) * dt;
                const double ri = std::sqrt(rm[0] * rm[0] + t * t + 2.0 * rm[0] * rm[1] * t);
                depth = depth + medium_at(ri - a_.bottom_radius_km).extinction * dt;
            }
            t_table_[static_cast<std::size_t>(j * t_w_ + i)] = exp_neg(depth);
        }
    }

    // Multiple scattering (Hillaire): 576 directions, 60 steps each.
    ms_table_.resize(static_cast<std::size_t>(ms_n_ * ms_n_));
    const int dirs = 24;
    const double uniform_phase = 1.0 / (4.0 * kPi);
    for (int j = 0; j < ms_n_; ++j) {
        for (int i = 0; i < ms_n_; ++i) {
            const double mu_s = texel_to_unit((i + 0.5) / ms_n_, ms_n_) * 2.0 - 1.0;
            const double r = a_.bottom_radius_km + texel_to_unit((j + 0.5) / ms_n_, ms_n_) *
                                                       (a_.top_radius_km - a_.bottom_radius_km);
            const std::array<double, 3> sun = {std::sqrt(std::max(1.0 - mu_s * mu_s, 0.0)), mu_s,
                                               0.0};
            Rgb light{};
            Rgb fraction{};
            for (int k = 0; k < dirs * dirs; ++k) {
                const double cos_t = 1.0 - 2.0 * ((k / dirs) + 0.5) / dirs;
                const double phi = 2.0 * kPi * ((k % dirs) + 0.5) / dirs;
                const double sin_t = std::sqrt(std::max(1.0 - cos_t * cos_t, 0.0));
                const std::array<double, 3> dir = {sin_t * std::cos(phi), cos_t,
                                                   sin_t * std::sin(phi)};
                const double ground = distance_to_ground(r, dir[1], a_.bottom_radius_km);
                const double path =
                    ground >= 0.0 ? ground
                                  : std::max(distance_to_sphere(r, dir[1], a_.top_radius_km), 0.0);
                const int steps = 60;
                const double dt = path / steps;
                Rgb through{1.0, 1.0, 1.0};
                Rgb l{};
                Rgb f{};
                for (int s = 0; s < steps; ++s) {
                    const double t = (s + 0.5) * dt;
                    const std::array<double, 3> p = {dir[0] * t, r + dir[1] * t, dir[2] * t};
                    const double ri = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                    const Medium m = medium_at(ri - a_.bottom_radius_km);
                    const Rgb step_t = exp_neg(m.extinction * dt);
                    const double mu_si = (p[0] * sun[0] + p[1] * sun[1]) / ri;
                    const Rgb src = m.scattering * sun_transmittance(ri, mu_si) * uniform_phase;
                    for (std::size_t c = 0; c < 3; ++c) {
                        const double e = std::max(m.extinction[c], 1e-12);
                        l[c] += through[c] * src[c] * (1.0 - step_t[c]) / e;
                        f[c] += through[c] * m.scattering[c] * (1.0 - step_t[c]) / e;
                    }
                    through = through * step_t;
                }
                if (ground >= 0.0) {
                    const std::array<double, 3> p = {dir[0] * ground, r + dir[1] * ground,
                                                     dir[2] * ground};
                    const double mu_g = (p[0] * sun[0] + p[1] * sun[1]) / a_.bottom_radius_km;
                    l = l + through * sun_transmittance(a_.bottom_radius_km, mu_g) *
                                Rgb{a_.ground_albedo} * (std::max(mu_g, 0.0) / kPi);
                }
                light = light + l;
                fraction = fraction + f;
            }
            const double n = dirs * dirs;
            Rgb out{};
            for (std::size_t c = 0; c < 3; ++c) {
                out[c] = (light[c] / n) / (1.0 - fraction[c] / n);
            }
            ms_table_[static_cast<std::size_t>(j * ms_n_ + i)] = out;
        }
    }
}

Rgb AtmosphereReference::sky_radiance(double r, double mu_s, double zenith, double azimuth) const {
    const std::array<double, 3> dir = {std::sin(zenith) * std::cos(azimuth), std::cos(zenith),
                                       std::sin(zenith) * std::sin(azimuth)};
    const std::array<double, 3> sun = {std::sqrt(std::max(1.0 - mu_s * mu_s, 0.0)), mu_s, 0.0};
    const double ground = distance_to_ground(r, dir[1], a_.bottom_radius_km);
    const double path =
        ground >= 0.0 ? ground : std::max(distance_to_sphere(r, dir[1], a_.top_radius_km), 0.0);
    const double c = dir[0] * sun[0] + dir[1] * sun[1];
    const double pr = rayleigh_phase(c);
    const double pm = mie_phase(a_.mie_g, c);
    const int steps = 1000;
    Rgb through{1.0, 1.0, 1.0};
    Rgb light{};
    double t_prev = 0.0;
    for (int i = 0; i < steps; ++i) {
        const double f = (i + 1.0) / steps;
        const double t_next = path * f * f;
        const double dt = t_next - t_prev;
        const double t = (t_prev + t_next) * 0.5;
        t_prev = t_next;
        const std::array<double, 3> p = {dir[0] * t, r + dir[1] * t, dir[2] * t};
        const double ri = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        const double mu_si = (p[0] * sun[0] + p[1] * sun[1]) / ri;
        const Medium m = medium_at(ri - a_.bottom_radius_km);
        const Rgb step_t = exp_neg(m.extinction * dt);
        const Rgb src = (m.rayleigh * pr + Rgb{m.mie * pm, m.mie * pm, m.mie * pm}) *
                            sun_transmittance(ri, mu_si) +
                        m.scattering * multi_scattering(ri, mu_si);
        for (std::size_t k = 0; k < 3; ++k) {
            const double e = std::max(m.extinction[k], 1e-12);
            light[k] += through[k] * src[k] * (1.0 - step_t[k]) / e;
        }
        through = through * step_t;
    }
    if (ground >= 0.0) {
        const std::array<double, 3> g = {dir[0] * ground, r + dir[1] * ground, dir[2] * ground};
        const double mu_g = (g[0] * sun[0] + g[1] * sun[1]) / a_.bottom_radius_km;
        light = light + through * sun_transmittance(a_.bottom_radius_km, mu_g) *
                            Rgb{a_.ground_albedo} * (std::max(mu_g, 0.0) / kPi);
    }
    return light * Rgb{a_.sun_illuminance};
}

std::array<double, 2> AtmosphereReference::transmittance_r_mu(double u, double v) const {
    return t_r_mu(a_, u, v, kTransmittanceWidth, kTransmittanceHeight);
}

std::array<double, 2> AtmosphereReference::sky_view_angles(double r, double u, double v) const {
    const double x = texel_to_unit(u, kSkyViewWidth);
    const double y = texel_to_unit(v, kSkyViewHeight);
    const double rho = std::sqrt(std::max(r * r - a_.bottom_radius_km * a_.bottom_radius_km, 0.0));
    const double zh = kPi - std::acos(std::clamp(rho / r, -1.0, 1.0));
    double zenith = 0.0;
    if (y < 0.5) {
        const double c = 1.0 - 2.0 * y;
        zenith = zh * (1.0 - c * c);
    } else {
        const double c = 2.0 * y - 1.0;
        zenith = zh + (kPi - zh) * c * c;
    }
    return {zenith, x * kPi};
}

}  // namespace fovea::test
