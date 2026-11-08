// The atmosphere (milestone 6, D-030; Hillaire, "A Scalable and Production
// Ready Sky and Atmosphere Rendering Technique", 2020): its medium, phase
// functions and the tables' parameterizations. Distances in km. Mirrored on
// the CPU in fovea/render/atmosphere.hpp, which the tests check the tables
// against.

static const float kPi = 3.14159265358979;

// Mirrors AtmosphereGpu in sky.cpp (std140).
struct Atmosphere {
    float3 rayleigh_scattering;  // per km, at the ground
    float bottom_radius;         // km
    float3 ozone_absorption;     // per km, at the ozone layer's peak
    float top_radius;
    float3 ground_albedo;
    float rayleigh_scale_height;  // km
    float3 sun_illuminance;       // at the top of the atmosphere
    float mie_scale_height;
    float mie_scattering;  // per km, at the ground
    float mie_extinction;
    float mie_g;
    float ozone_centre;  // km
    float ozone_half_width;
    float sun_angular_radius;  // radians
    float2 unused;
};

struct Medium {
    float3 scattering;  // Rayleigh and Mie
    float3 extinction;
    float3 rayleigh;    // Rayleigh's scattering alone
    float mie;          // Mie's
};

Medium medium_at(Atmosphere a, float height) {
    const float h = max(height, 0.0);
    const float rd = exp(-h / a.rayleigh_scale_height);
    const float md = exp(-h / a.mie_scale_height);
    const float od = max(0.0, 1.0 - abs(h - a.ozone_centre) / a.ozone_half_width);
    Medium m;
    m.rayleigh = a.rayleigh_scattering * rd;
    m.mie = a.mie_scattering * md;
    m.scattering = m.rayleigh + m.mie;
    m.extinction = m.rayleigh + a.mie_extinction * md + a.ozone_absorption * od;
    return m;
}

float rayleigh_phase(float c) {
    return 3.0 / (16.0 * kPi) * (1.0 + c * c);
}

// Cornette-Shanks.
float mie_phase(float g, float c) {
    const float g2 = g * g;
    return 3.0 / (8.0 * kPi) * (1.0 - g2) * (1.0 + c * c) /
           ((2.0 + g2) * pow(1.0 + g2 - 2.0 * g * c, 1.5));
}

// The discriminant of a ray from radius r, zenith cosine mu, against a
// sphere: written as (r mu)^2 - (r - R)(r + R), not r^2 (mu^2 - 1) + R^2,
// which loses everything to cancellation in floats near the ground (r^2 is
// 4e7 km^2).
float sphere_discriminant(float r, float mu, float radius) {
    return (r * mu) * (r * mu) - (r - radius) * (r + radius);
}

// Distance along the ray to the sphere of radius `radius` (outward); -1 if
// it misses.
float distance_to_sphere(float r, float mu, float radius) {
    const float d = sphere_discriminant(r, mu, radius);
    if (d < 0.0) {
        return -1.0;
    }
    return -r * mu + sqrt(d);
}

// Whether a ray from radius r with zenith cosine mu goes below the horizon,
// into the ground.
bool hits_ground(float r, float mu, float bottom) {
    return mu < -sqrt(max(1.0 - (bottom / r) * (bottom / r), 0.0));
}

// To the ground, or -1 if the ray misses it.
float distance_to_ground(float r, float mu, float bottom) {
    if (!hits_ground(r, mu, bottom)) {
        return -1.0;
    }
    return max(-r * mu - sqrt(max(sphere_discriminant(r, mu, bottom), 0.0)), 0.0);
}

// A unit coordinate to texel centres and back, so 0 and 1 land on the
// first and last texel's centres.
float unit_to_texel(float x, float size) {
    return 0.5 / size + x * (1.0 - 1.0 / size);
}
float texel_to_unit(float u, float size) {
    return (u - 0.5 / size) / (1.0 - 1.0 / size);
}

static const float2 kTransmittanceSize = float2(256.0, 64.0);
static const float2 kMultiScatteringSize = float2(32.0, 32.0);
static const float2 kSkyViewSize = float2(192.0, 108.0);

// The transmittance table: (radius, zenith cosine) to uv, Bruneton's
// mapping (rays that don't hit the ground).
float2 transmittance_uv(Atmosphere a, float r, float mu) {
    const float h = sqrt(a.top_radius * a.top_radius - a.bottom_radius * a.bottom_radius);
    const float rho = sqrt(max(r * r - a.bottom_radius * a.bottom_radius, 0.0));
    const float d = max(distance_to_sphere(r, mu, a.top_radius), 0.0);
    const float d_min = a.top_radius - r;
    const float d_max = rho + h;
    const float x_mu = (d - d_min) / (d_max - d_min);
    const float x_r = rho / h;
    return float2(unit_to_texel(x_mu, kTransmittanceSize.x),
                  unit_to_texel(x_r, kTransmittanceSize.y));
}

float2 transmittance_r_mu(Atmosphere a, float2 uv) {
    const float x_mu = texel_to_unit(uv.x, kTransmittanceSize.x);
    const float x_r = texel_to_unit(uv.y, kTransmittanceSize.y);
    const float h = sqrt(a.top_radius * a.top_radius - a.bottom_radius * a.bottom_radius);
    const float rho = h * x_r;
    const float r = sqrt(rho * rho + a.bottom_radius * a.bottom_radius);
    const float d_min = a.top_radius - r;
    const float d_max = rho + h;
    const float d = d_min + x_mu * (d_max - d_min);
    const float mu = d == 0.0 ? 1.0 : clamp((h * h - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
    return float2(r, mu);
}

// The multiple-scattering table: (sun zenith cosine, radius).
float2 multi_scattering_uv(Atmosphere a, float r, float mu_s) {
    const float x = mu_s * 0.5 + 0.5;
    const float y = (r - a.bottom_radius) / (a.top_radius - a.bottom_radius);
    return float2(unit_to_texel(saturate(x), kMultiScatteringSize.x),
                  unit_to_texel(saturate(y), kMultiScatteringSize.y));
}

// The sky-view table: (view zenith angle, azimuth from the sun) for an eye
// at radius r. The upper half is above the geometric horizon, the lower
// below it, each squeezed toward the horizon where the sky changes most;
// azimuth runs 0 (toward the sun) to pi (away), the sky being symmetric
// about the sun's vertical.
float horizon_zenith(Atmosphere a, float r) {
    const float rho = sqrt(max(r * r - a.bottom_radius * a.bottom_radius, 0.0));
    return kPi - acos(clamp(rho / r, -1.0, 1.0));
}

float2 sky_view_uv(Atmosphere a, float r, float zenith, float azimuth) {
    const float zh = horizon_zenith(a, r);
    float v;
    if (zenith < zh) {
        v = (1.0 - sqrt(saturate(1.0 - zenith / zh))) * 0.5;
    } else {
        v = (1.0 + sqrt(saturate((zenith - zh) / (kPi - zh)))) * 0.5;
    }
    const float u = saturate(azimuth / kPi);
    return float2(unit_to_texel(u, kSkyViewSize.x), unit_to_texel(v, kSkyViewSize.y));
}

// uv to (zenith, azimuth).
float2 sky_view_angles(Atmosphere a, float r, float2 uv) {
    const float u = texel_to_unit(uv.x, kSkyViewSize.x);
    const float v = texel_to_unit(uv.y, kSkyViewSize.y);
    const float zh = horizon_zenith(a, r);
    float zenith;
    if (v < 0.5) {
        const float c = 1.0 - 2.0 * v;
        zenith = zh * (1.0 - c * c);
    } else {
        const float c = 2.0 * v - 1.0;
        zenith = zh + (kPi - zh) * c * c;
    }
    return float2(zenith, u * kPi);
}

float luminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}
