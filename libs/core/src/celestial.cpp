#include "fovea/core/celestial.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace fovea {

namespace {

constexpr double kDegree = std::numbers::pi / 180.0;
constexpr double kJ2000 = 2451545.0;
constexpr double kAuKm = 149597870.7;
// Terrestrial time ahead of UTC (Delta T): about 64 s in 2000, 69 s in
// the 2020s. The moon moves 0.5 degrees an hour, so 5 s off is 0.0007.
constexpr double kDeltaTSeconds = 69.0;

double sin_deg(double a) {
    return std::sin(a * kDegree);
}
double cos_deg(double a) {
    return std::cos(a * kDegree);
}

// Ecliptic of date to ECEF: through the obliquity to equatorial
// coordinates, then the Earth's rotation (Greenwich mean sidereal time).
DVec3 ecliptic_to_ecef(double lon_deg, double lat_deg, double obliquity_deg, double gmst_deg) {
    const double cl = cos_deg(lon_deg);
    const double sl = sin_deg(lon_deg);
    const double cb = cos_deg(lat_deg);
    const double sb = sin_deg(lat_deg);
    const double ce = cos_deg(obliquity_deg);
    const double se = sin_deg(obliquity_deg);
    const DVec3 eq(cb * cl, cb * sl * ce - sb * se, cb * sl * se + sb * ce);
    const double cg = cos_deg(gmst_deg);
    const double sg = sin_deg(gmst_deg);
    return {cg * eq.x + sg * eq.y, -sg * eq.x + cg * eq.y, eq.z};
}

// Meeus, table 47.A: multiples of D, M, M' and F, then the sine
// coefficient of longitude (1e-6 degrees) and the cosine coefficient of
// distance (1e-3 km).
struct LonDistTerm {
    std::int8_t d, m, mp, f;
    std::int32_t l, r;
};
constexpr std::array<LonDistTerm, 60> kLonDist{{
    {0, 0, 1, 0, 6288774, -20905355},
    {2, 0, -1, 0, 1274027, -3699111},
    {2, 0, 0, 0, 658314, -2955968},
    {0, 0, 2, 0, 213618, -569925},
    {0, 1, 0, 0, -185116, 48888},
    {0, 0, 0, 2, -114332, -3149},
    {2, 0, -2, 0, 58793, 246158},
    {2, -1, -1, 0, 57066, -152138},
    {2, 0, 1, 0, 53322, -170733},
    {2, -1, 0, 0, 45758, -204586},
    {0, 1, -1, 0, -40923, -129620},
    {1, 0, 0, 0, -34720, 108743},
    {0, 1, 1, 0, -30383, 104755},
    {2, 0, 0, -2, 15327, 10321},
    {0, 0, 1, 2, -12528, 0},
    {0, 0, 1, -2, 10980, 79661},
    {4, 0, -1, 0, 10675, -34782},
    {0, 0, 3, 0, 10034, -23210},
    {4, 0, -2, 0, 8548, -21636},
    {2, 1, -1, 0, -7888, 24208},
    {2, 1, 0, 0, -6766, 30824},
    {1, 0, -1, 0, -5163, -8379},
    {1, 1, 0, 0, 4987, -16675},
    {2, -1, 1, 0, 4036, -12831},
    {2, 0, 2, 0, 3994, -10445},
    {4, 0, 0, 0, 3861, -11650},
    {2, 0, -3, 0, 3665, 14403},
    {0, 1, -2, 0, -2689, -7003},
    {2, 0, -1, 2, -2602, 0},
    {2, -1, -2, 0, 2390, 10056},
    {1, 0, 1, 0, -2348, 6322},
    {2, -2, 0, 0, 2236, -9884},
    {0, 1, 2, 0, -2120, 5751},
    {0, 2, 0, 0, -2069, 0},
    {2, -2, -1, 0, 2048, -4950},
    {2, 0, 1, -2, -1773, 4130},
    {2, 0, 0, 2, -1595, 0},
    {4, -1, -1, 0, 1215, -3958},
    {0, 0, 2, 2, -1110, 0},
    {3, 0, -1, 0, -892, 3258},
    {2, 1, 1, 0, -810, 2616},
    {4, -1, -2, 0, 759, -1897},
    {0, 2, -1, 0, -713, -2117},
    {2, 2, -1, 0, -700, 2354},
    {2, 1, -2, 0, 691, 0},
    {2, -1, 0, -2, 596, 0},
    {4, 0, 1, 0, 549, -1423},
    {0, 0, 4, 0, 537, -1117},
    {4, -1, 0, 0, 520, -1571},
    {1, 0, -2, 0, -487, -1739},
    {2, 1, 0, -2, -399, 0},
    {0, 0, 2, -2, -381, -4421},
    {1, 1, 1, 0, 351, 0},
    {3, 0, -2, 0, -340, 0},
    {4, 0, -3, 0, 330, 0},
    {2, -1, 2, 0, 327, 0},
    {0, 2, 1, 0, -323, 1165},
    {1, 1, -1, 0, 299, 0},
    {2, 0, 3, 0, 294, 0},
    {2, 0, -1, -2, 0, 8752},
}};

// Meeus, table 47.B: latitude's sine coefficients (1e-6 degrees).
struct LatTerm {
    std::int8_t d, m, mp, f;
    std::int32_t b;
};
constexpr std::array<LatTerm, 60> kLat{{
    {0, 0, 0, 1, 5128122}, {0, 0, 1, 1, 280602},  {0, 0, 1, -1, 277693}, {2, 0, 0, -1, 173237},
    {2, 0, -1, 1, 55413},  {2, 0, -1, -1, 46271}, {2, 0, 0, 1, 32573},   {0, 0, 2, 1, 17198},
    {2, 0, 1, -1, 9266},   {0, 0, 2, -1, 8822},   {2, -1, 0, -1, 8216},  {2, 0, -2, -1, 4324},
    {2, 0, 1, 1, 4200},    {2, 1, 0, -1, -3359},  {2, -1, -1, 1, 2463},  {2, -1, 0, 1, 2211},
    {2, -1, -1, -1, 2065}, {0, 1, -1, -1, -1870}, {4, 0, -1, -1, 1828},  {0, 1, 0, 1, -1794},
    {0, 0, 0, 3, -1749},   {0, 1, -1, 1, -1565},  {1, 0, 0, 1, -1491},   {0, 1, 1, 1, -1475},
    {0, 1, 1, -1, -1410},  {0, 1, 0, -1, -1344},  {1, 0, 0, -1, -1335},  {0, 0, 3, 1, 1107},
    {4, 0, 0, -1, 1021},   {4, 0, -1, 1, 833},    {0, 0, 1, -3, 777},    {4, 0, -2, 1, 671},
    {2, 0, 0, -3, 607},    {2, 0, 2, -1, 596},    {2, -1, 1, -1, 491},   {2, 0, -2, 1, -451},
    {0, 0, 3, -1, 439},    {2, 0, 2, 1, 422},     {2, 0, -3, -1, 421},   {2, 1, -1, 1, -366},
    {2, 1, 0, 1, -351},    {4, 0, 0, 1, 331},     {2, -1, 1, 1, 315},    {2, -2, 0, -1, 302},
    {0, 0, 1, 3, -283},    {2, 1, 1, -1, -229},   {1, 1, 0, -1, 223},    {1, 1, 0, 1, 223},
    {0, 1, -2, -1, -220},  {2, 1, -1, -1, -220},  {1, 0, 1, 1, -185},    {2, -1, -2, -1, 181},
    {0, 1, 2, 1, -177},    {4, 0, -2, -1, 176},   {4, -1, -1, -1, 166},  {1, 0, 1, -1, -164},
    {4, 0, 1, -1, 132},    {1, 0, -1, -1, -119},  {4, -1, 0, -1, 115},   {2, -2, 0, 1, 107},
}};

}  // namespace

SkyBodies sky_bodies(double julian_date_utc) noexcept {
    // The sun (Astronomical Almanac, section C, low-precision formulas):
    // its apparent ecliptic longitude, latitude taken as 0, and distance.
    const double n = julian_date_utc - kJ2000;
    const double l_sun = 280.460 + 0.9856474 * n;
    const double g = 357.528 + 0.9856003 * n;
    const double lambda_sun = l_sun + 1.915 * sin_deg(g) + 0.020 * sin_deg(2.0 * g);
    const double r_sun_au = 1.00014 - 0.01671 * cos_deg(g) - 0.00014 * cos_deg(2.0 * g);
    const double obliquity = 23.439 - 0.0000004 * n;
    // Greenwich mean sidereal time (the Almanac's), from UT taken as UTC.
    const double gmst_deg = 15.0 * (18.697374558 + 24.06570982441908 * n);

    SkyBodies out;
    out.sun = ecliptic_to_ecef(lambda_sun, 0.0, obliquity, gmst_deg);

    // The moon (Meeus ch. 47), in terrestrial time.
    const double t = (julian_date_utc + kDeltaTSeconds / 86400.0 - kJ2000) / 36525.0;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double t4 = t3 * t;
    const double lp =
        218.3164477 + 481267.88123421 * t - 0.0015786 * t2 + t3 / 538841.0 - t4 / 65194000.0;
    const double d =
        297.8501921 + 445267.1114034 * t - 0.0018819 * t2 + t3 / 545868.0 - t4 / 113065000.0;
    const double m = 357.5291092 + 35999.0502909 * t - 0.0001536 * t2 + t3 / 24490000.0;
    const double mp =
        134.9633964 + 477198.8675055 * t + 0.0087414 * t2 + t3 / 69699.0 - t4 / 14712000.0;
    const double f =
        93.2720950 + 483202.0175233 * t - 0.0036539 * t2 - t3 / 3526000.0 + t4 / 863310000.0;
    const double a1 = 119.75 + 131.849 * t;
    const double a2 = 53.09 + 479264.290 * t;
    const double a3 = 313.45 + 481266.484 * t;
    // The eccentricity of the Earth's orbit, falling: terms in M scale by
    // it once per multiple.
    const double e = 1.0 - 0.002516 * t - 0.0000074 * t2;
    const double e2 = e * e;
    const auto e_factor = [e, e2](int mult) {
        if (mult == 0) {
            return 1.0;
        }
        return mult == 1 || mult == -1 ? e : e2;
    };

    double sum_l = 0.0;
    double sum_r = 0.0;
    for (const LonDistTerm& k : kLonDist) {
        const double arg = k.d * d + k.m * m + k.mp * mp + k.f * f;
        const double ef = e_factor(k.m);
        sum_l += k.l * ef * sin_deg(arg);
        sum_r += k.r * ef * cos_deg(arg);
    }
    double sum_b = 0.0;
    for (const LatTerm& k : kLat) {
        sum_b += k.b * e_factor(k.m) * sin_deg(k.d * d + k.m * m + k.mp * mp + k.f * f);
    }
    sum_l += 3958.0 * sin_deg(a1) + 1962.0 * sin_deg(lp - f) + 318.0 * sin_deg(a2);
    sum_b += -2235.0 * sin_deg(lp) + 382.0 * sin_deg(a3) + 175.0 * sin_deg(a1 - f) +
             175.0 * sin_deg(a1 + f) + 127.0 * sin_deg(lp - mp) - 115.0 * sin_deg(lp + mp);
    const double lambda_moon = lp + sum_l / 1e6;
    const double beta_moon = sum_b / 1e6;
    const double dist_km = 385000.56 + sum_r / 1000.0;
    // The mean obliquity (Meeus 22.2, to T): the Almanac's above is the same
    // to 1e-5 degrees over the century.
    const double obliquity_moon = 23.4392911 - 0.0130042 * t;
    out.moon_m =
        ecliptic_to_ecef(lambda_moon, beta_moon, obliquity_moon, gmst_deg) * (dist_km * 1000.0);

    // The lit fraction from the phase angle, the sun-moon-Earth angle
    // (Meeus 48.2 and 48.3), from the elongation.
    const double cos_elong = std::cos(beta_moon * kDegree) * cos_deg(lambda_moon - lambda_sun);
    const double elong = std::acos(std::clamp(cos_elong, -1.0, 1.0));
    const double r_sun_km = r_sun_au * kAuKm;
    const double phase = std::atan2(r_sun_km * std::sin(elong), dist_km - r_sun_km * cos_elong);
    out.moon_lit = (1.0 + std::cos(phase)) / 2.0;
    return out;
}

}  // namespace fovea
