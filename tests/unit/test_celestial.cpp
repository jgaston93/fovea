#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "fovea/core/celestial.hpp"
#include "fovea/core/geodesy.hpp"

namespace {

using fovea::DVec3;

constexpr double kDegree = std::numbers::pi / 180.0;

// JPL Horizons (ssd.jpl.nasa.gov/api/horizons.api, queried 2026-11-08):
// observer at KHSV (34.6372 N, 86.7751 W, 190 m), apparent azimuth and
// elevation without refraction (APPARENT='AIRLESS'), and the moon's
// illuminated fraction; times UT. Body 10 is the sun, 301 the moon.
struct Reference {
    int year;
    unsigned month, day, hour, minute;
    double sun_az, sun_el;
    double moon_az, moon_el, moon_lit_percent;
};
constexpr std::array<Reference, 16> kHorizons{{
    {2000, 1, 1, 12, 0, 110.504488043, -10.990509549, 143.760099804, 36.190392519, 22.59553},
    {2005, 7, 4, 3, 30, 324.475292648, -23.409246659, 353.601826786, -28.998157159, 5.33736},
    {2010, 11, 15, 14, 45, 136.498655372, 23.683307235, 41.670736485, -49.957962050, 67.97421},
    {2016, 2, 29, 22, 10, 247.382675127, 17.477775347, 325.509548829, -67.596445088, 60.27592},
    {2020, 12, 21, 16, 0, 152.842459671, 26.758115269, 80.387175686, -24.935186749, 47.42324},
    {2024, 4, 8, 18, 30, 201.725363875, 61.339065569, 202.071102075, 61.184539153, 0.00038},
    {2026, 3, 20, 11, 58, 90.530061706, 0.683815854, 72.573842603, -7.315039251, 2.91739},
    {2026, 3, 20, 15, 30, 127.899986060, 41.658356364, 101.632158984, 35.022098251, 3.51230},
    {2026, 6, 21, 18, 0, 192.839004545, 78.544835664, 94.343699233, 2.750186366, 49.24141},
    {2026, 9, 23, 0, 5, 273.612440550, -5.213692271, 135.223611456, 23.506719042, 86.01215},
    {2026, 12, 21, 17, 0, 167.881942284, 30.937652242, 25.903544271, -27.148458849, 91.82728},
    {2027, 8, 2, 9, 20, 51.832540936, -18.225698290, 51.906791767, -18.710058309, 0.00186},
    {2030, 5, 17, 1, 0, 296.656838686, -3.817846942, 121.434843510, 7.328866399, 99.73501},
    {2033, 10, 10, 20, 40, 235.851634201, 29.510058347, 15.877970330, -39.741926141, 91.42746},
    {2037, 1, 25, 6, 15, 13.378656366, -73.850841491, 278.661851222, 5.397769108, 55.78627},
    {2040, 7, 14, 13, 5, 81.090952294, 26.404489506, 57.503813992, -26.651941490, 23.38927},
}};

constexpr fovea::Geodetic kKhsv{34.6372, -86.7751, 190.0};

// A direction from azimuth and elevation at KHSV, in ECEF.
DVec3 from_az_el(double az_deg, double el_deg) {
    const double az = az_deg * kDegree;
    const double el = el_deg * kDegree;
    const DVec3 ned(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), -std::sin(el));
    return fovea::ned_to_ecef(kKhsv.lat_deg, kKhsv.lon_deg) * ned;
}

double degrees_between(const DVec3& a, const DVec3& b) {
    return std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0)) /
           kDegree;
}

double julian(const Reference& r) {
    return fovea::julian_date(r.year, r.month, r.day, r.hour * 3600.0 + r.minute * 60.0);
}

TEST(Celestial, JulianDatesAreTheStandardOnes) {
    EXPECT_DOUBLE_EQ(fovea::julian_date(2000, 1, 1, 12 * 3600.0), 2451545.0);  // J2000
    EXPECT_DOUBLE_EQ(fovea::julian_date(1970, 1, 1, 0.0), 2440587.5);          // the Unix epoch
    EXPECT_DOUBLE_EQ(fovea::julian_date(2024, 2, 29, 0.0) + 1.0,
                     fovea::julian_date(2024, 3, 1, 0.0));
}

TEST(Celestial, TheSunIsWhereHorizonsPutsIt) {
    double worst = 0.0;
    for (const Reference& r : kHorizons) {
        const fovea::SkyBodies sky = fovea::sky_bodies(julian(r));
        const double off = degrees_between(sky.sun, from_az_el(r.sun_az, r.sun_el));
        worst = std::max(worst, off);
        EXPECT_LT(off, 0.05) << r.year << "-" << r.month << "-" << r.day;
    }
    std::printf("sun: worst %.4f degrees from Horizons\n", worst);
}

TEST(Celestial, TheMoonIsWhereHorizonsPutsItSeenFromTheGround) {
    const DVec3 eye = fovea::geodetic_to_ecef(kKhsv);
    double worst = 0.0;
    double worst_geocentric = 0.0;
    double worst_lit = 0.0;
    for (const Reference& r : kHorizons) {
        const fovea::SkyBodies sky = fovea::sky_bodies(julian(r));
        const DVec3 seen = from_az_el(r.moon_az, r.moon_el);
        const double off = degrees_between(sky.moon_m - eye, seen);
        worst = std::max(worst, off);
        // From the Earth's centre instead: the parallax the eye corrects.
        worst_geocentric = std::max(worst_geocentric, degrees_between(sky.moon_m, seen));
        EXPECT_LT(off, 0.05) << r.year << "-" << r.month << "-" << r.day;
        const double lit = std::abs(sky.moon_lit - r.moon_lit_percent / 100.0);
        worst_lit = std::max(worst_lit, lit);
        EXPECT_LT(lit, 0.01) << r.year << "-" << r.month << "-" << r.day;
    }
    // The parallax matters: without it the moon is off by up to a degree.
    EXPECT_GT(worst_geocentric, 0.5);
    std::printf("moon: worst %.4f degrees (%.3f without parallax), lit fraction %.4f\n", worst,
                worst_geocentric, worst_lit);
}

}  // namespace
