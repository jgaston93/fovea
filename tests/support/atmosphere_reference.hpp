#pragma once

// A CPU reference for the sky's tables (milestone 6 step 2, D-030): the same
// model as shaders/sky.slang, integrated in double with many more steps and
// directions, to check the GPU's tables against.

#include <array>
#include <vector>

#include "fovea/render/atmosphere.hpp"

namespace fovea::test {

using Rgb = std::array<double, 3>;

class AtmosphereReference {
public:
    // Builds its own transmittance and multiple-scattering tables, finer
    // than the GPU's (about a second).
    explicit AtmosphereReference(const Atmosphere& a);

    // The transmittance from radius r (km) along zenith cosine mu to the
    // top of the atmosphere, integrated directly.
    Rgb transmittance(double r, double mu) const;

    // The sky's radiance seen from radius r along (zenith, azimuth from the
    // sun), the sun at zenith cosine mu_s: the sun's illuminance included,
    // the sun's disc and the ground's own light not.
    Rgb sky_radiance(double r, double mu_s, double zenith, double azimuth) const;

    // Multiple scattering per unit of sun illuminance, from its table.
    Rgb multi_scattering(double r, double mu_s) const;

    // The GPU tables' parameterizations (atmosphere.slang), texel centres.
    std::array<double, 2> transmittance_r_mu(double u, double v) const;
    std::array<double, 2> sky_view_angles(double r, double u, double v) const;

private:
    struct Medium {
        Rgb scattering, extinction, rayleigh;
        double mie = 0.0;
    };
    Medium medium_at(double height_km) const;
    Rgb sun_transmittance(double r, double mu_s) const;  // from its table

    Atmosphere a_;
    int t_w_ = 256;
    int t_h_ = 64;
    std::vector<Rgb> t_table_;  // by (mu, r) on a regular grid
    int ms_n_ = 32;
    std::vector<Rgb> ms_table_;  // by (mu_s, r), as the GPU's
};

}  // namespace fovea::test
