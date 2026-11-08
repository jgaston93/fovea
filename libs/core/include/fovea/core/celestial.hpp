#pragma once

// The sun and moon from the date and time (milestone 6, D-029): where they
// are, in ECEF, for lighting and for drawing them. Geometric directions,
// unrefracted. The sun from the Astronomical Almanac's low-precision
// formulas (about 0.01 degrees, 1950 to 2050); the moon from Meeus's
// truncated lunar theory (Astronomical Algorithms, 2nd ed., ch. 47, about
// 10 arc-seconds). The Earth's rotation is mean sidereal time from UTC, with
// nutation left out of both the bodies and the rotation (they cancel to
// first order) and polar motion ignored.

#include <cstdint>

#include "fovea/core/math.hpp"

namespace fovea {

// Days since 1970-01-01 of a Gregorian date (Hinnant's days_from_civil).
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// The Julian date of a UTC date and time of day (seconds since midnight).
constexpr double julian_date(std::int64_t year, unsigned month, unsigned day,
                             double seconds) noexcept {
    return static_cast<double>(days_from_civil(year, month, day)) + 2440587.5 + seconds / 86400.0;
}

struct SkyBodies {
    DVec3 sun{0.0, 0.0, 1.0};  // towards the sun from the Earth's centre, unit, ECEF
    DVec3 moon_m{0.0};         // the moon's centre, ECEF metres (its direction from an eye
                               // there is moon_m - eye: the parallax, up to a degree)
    double moon_lit = 0.0;     // the fraction of the moon's disc lit, 0 to 1
};

// Where the sun and moon are at a UTC Julian date. Good from 1950 to
// 2050 (the Almanac's sun, and a fixed Delta T).
SkyBodies sky_bodies(double julian_date_utc) noexcept;

// How much of a body's disc, of the given radius, is above the level
// horizon of a place whose vertical is `up` (unit vectors): 0 below, 1
// above, in proportion to its centre's height across the disc between.
// It lights only the part above: a sun under the horizon lights nothing,
// whichever way a wall faces it.
double disc_above_horizon(const DVec3& towards, const DVec3& up, double radius_deg) noexcept;

inline constexpr double kSunRadiusDeg = 0.267;

}  // namespace fovea
