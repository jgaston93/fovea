#include "runways.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

#include "fovea/core/geodesy.hpp"

namespace fovea::compiler {

namespace {

// The fit: a station every 2 m along the runway, each a local regression
// over 30 m either side (tricube weights), the sources sampled every metre.
constexpr double kStationStep = 2.0;
constexpr double kHalfWindow = 30.0;
constexpr double kSampleStep = 1.0;
// Robust weights (Tukey's bisquare at 4.685 robust standard deviations of
// the whole runway's residuals), refitted this many times; the scale never below 1 cm, so a
// perfectly smooth runway isn't fitted to its rounding.
constexpr int kRobustPasses = 4;
constexpr double kBisquare = 4.685;
constexpr double kMinScale = 0.01;

constexpr std::size_t kTerms = 6;  // 1, du, du^2, v, |v|, v^2
using Normal = std::array<std::array<double, kTerms + 1>, kTerms>;

// Solves the normal equations in place (Gaussian elimination, partial
// pivoting). False if singular.
bool solve(Normal& m, std::array<double, kTerms>& x) {
    for (std::size_t col = 0; col < kTerms; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < kTerms; ++r) {
            if (std::abs(m[r][col]) > std::abs(m[pivot][col])) {
                pivot = r;
            }
        }
        if (std::abs(m[pivot][col]) < 1e-12) {
            return false;
        }
        std::swap(m[col], m[pivot]);
        for (std::size_t r = col + 1; r < kTerms; ++r) {
            const double f = m[r][col] / m[col][col];
            for (std::size_t c = col; c <= kTerms; ++c) {
                m[r][c] -= f * m[col][c];
            }
        }
    }
    for (std::size_t r = kTerms; r-- > 0;) {
        double v = m[r][kTerms];
        for (std::size_t c = r + 1; c < kTerms; ++c) {
            v -= m[r][c] * x[c];
        }
        x[r] = v / m[r][r];
    }
    return true;
}

}  // namespace

std::optional<double> parse_degrees_minutes(std::string_view text, bool latitude) {
    const auto dash = text.find('-');
    if (dash == std::string_view::npos || dash == 0 || text.size() < dash + 3) {
        return std::nullopt;
    }
    const char hemisphere = text.back();
    const bool negative = latitude ? hemisphere == 'S' : hemisphere == 'W';
    if (latitude ? (hemisphere != 'N' && hemisphere != 'S')
                 : (hemisphere != 'E' && hemisphere != 'W')) {
        return std::nullopt;
    }
    int degrees = 0;
    double minutes = 0.0;
    const char* d_end = text.data() + dash;
    const auto [dp, de] = std::from_chars(text.data(), d_end, degrees);
    const char* m_begin = d_end + 1;
    const char* m_end = text.data() + text.size() - 1;
    const auto [mp, me] = std::from_chars(m_begin, m_end, minutes);
    if (de != std::errc{} || dp != d_end || me != std::errc{} || mp != m_end || degrees < 0 ||
        degrees > (latitude ? 90 : 180) || std::isnan(minutes) || minutes < 0.0 ||
        minutes >= 60.0) {
        return std::nullopt;
    }
    const double v = degrees + minutes / 60.0;
    if (v > (latitude ? 90.0 : 180.0)) {
        return std::nullopt;
    }
    return negative ? -v : v;
}

std::optional<RunwaySurface> RunwaySurface::fit(const Runway& runway, double blend_m,
                                                const PointHeights& heights, std::string& error) {
    RunwaySurface s;
    s.runway_ = runway;
    s.blend_ = std::max(blend_m, 0.0);
    const RunwayEnd& e0 = runway.ends[0];
    const RunwayEnd& e1 = runway.ends[1];
    s.origin_ecef_ = geodetic_to_ecef({e0.lat_deg, e0.lon_deg, 0.0});
    s.to_local_ = glm::transpose(
        ned_to_ecef((e0.lat_deg + e1.lat_deg) / 2.0, (e0.lon_deg + e1.lon_deg) / 2.0));
    const DVec3 far =
        s.to_local_ * (geodetic_to_ecef({e1.lat_deg, e1.lon_deg, 0.0}) - s.origin_ecef_);
    s.length_ = std::hypot(far.x, far.y);
    // At least 5 m wide: six samples across, so three distances from the
    // centreline for the crown's terms.
    if (!(s.length_ > 2.0 * kStationStep) || !(runway.width_m >= 5.0) ||
        runway.width_m > s.length_) {
        error = "runway " + runway.name +
                ": needs a length over 4 m and a width from 5 m to its length";
        return std::nullopt;
    }
    // A published length catches a mistyped end.
    if (runway.length_m > 0.0 && std::abs(s.length_ - runway.length_m) > 0.005 * runway.length_m) {
        error = "runway " + runway.name + ": its ends are " + std::to_string(s.length_) +
                " m apart, but its length is " + std::to_string(runway.length_m) + " m";
        return std::nullopt;
    }
    s.axis_north_ = far.x / s.length_;
    s.axis_east_ = far.y / s.length_;

    // Its reach: the rectangle and the blend around it.
    const double half = runway.width_m / 2.0;
    s.reach_ = {90.0, -90.0, 180.0, -180.0};
    for (const double u : {-s.blend_, s.length_ + s.blend_}) {
        for (const double v : {-half - s.blend_, half + s.blend_}) {
            const Geodetic g = ecef_to_geodetic(s.to_ecef(u, v));
            s.reach_.south_deg = std::min(s.reach_.south_deg, g.lat_deg);
            s.reach_.north_deg = std::max(s.reach_.north_deg, g.lat_deg);
            s.reach_.west_deg = std::min(s.reach_.west_deg, g.lon_deg);
            s.reach_.east_deg = std::max(s.reach_.east_deg, g.lon_deg);
        }
    }

    // The sources every metre over the rectangle, edges included.
    const auto nu = static_cast<std::size_t>(std::ceil(s.length_ / kSampleStep)) + 1;
    const auto nv = static_cast<std::size_t>(std::ceil(runway.width_m / kSampleStep)) + 1;
    const double du = s.length_ / static_cast<double>(nu - 1);
    const double dv = runway.width_m / static_cast<double>(nv - 1);
    std::vector<double> lat(nu * nv);
    std::vector<double> lon(nu * nv);
    for (std::size_t i = 0; i < nu; ++i) {
        for (std::size_t j = 0; j < nv; ++j) {
            const Geodetic g = ecef_to_geodetic(
                s.to_ecef(static_cast<double>(i) * du, -half + static_cast<double>(j) * dv));
            lat[i * nv + j] = g.lat_deg;
            lon[i * nv + j] = g.lon_deg;
        }
    }
    std::vector<float> h;
    if (!heights(lat, lon, h, error)) {
        error = "runway " + runway.name + ": " + error;
        return std::nullopt;
    }

    // Each station's local regression, in units that keep it well
    // conditioned: du over the window, v over the half width. Robust as
    // LOWESS is: each pass weighs every sample by its residual from the
    // whole surface the pass before, scaled by the spread over the whole
    // runway (mostly the survey's noise), so what stood on the runway drops
    // out even where it fills a station's window.
    const auto ns = static_cast<std::size_t>(std::ceil(s.length_ / kStationStep)) + 1;
    s.station_step_ = s.length_ / static_cast<double>(ns - 1);
    s.stations_.resize(ns);
    const auto window = static_cast<std::ptrdiff_t>(std::ceil(kHalfWindow / du));
    std::vector<double> robust(nu * nv, 1.0);
    std::vector<double> residual(nu * nv, 0.0);
    std::vector<double> scratch;
    for (int pass = 0; pass <= kRobustPasses; ++pass) {
        for (std::size_t k = 0; k < ns; ++k) {
            const double uk = static_cast<double>(k) * s.station_step_;
            const auto centre = static_cast<std::ptrdiff_t>(std::lround(uk / du));
            const auto i0 = static_cast<std::size_t>(std::max<std::ptrdiff_t>(centre - window, 0));
            const auto i1 = static_cast<std::size_t>(
                std::min<std::ptrdiff_t>(centre + window, static_cast<std::ptrdiff_t>(nu) - 1));
            Normal m{};
            for (std::size_t i = i0; i <= i1; ++i) {
                const double x = (static_cast<double>(i) * du - uk) / kHalfWindow;
                const double c = std::abs(x) < 1.0 ? 1.0 - std::abs(x * x * x) : 0.0;
                const double kernel = c * c * c;
                if (kernel <= 0.0) {
                    continue;
                }
                for (std::size_t j = 0; j < nv; ++j) {
                    const double w = kernel * robust[i * nv + j];
                    if (w <= 0.0) {
                        continue;
                    }
                    const double y = (-half + static_cast<double>(j) * dv) / half;
                    const std::array<double, kTerms> t{1.0, x, x * x, y, std::abs(y), y * y};
                    const auto z = static_cast<double>(h[i * nv + j]);
                    for (std::size_t a = 0; a < kTerms; ++a) {
                        for (std::size_t b = a; b < kTerms; ++b) {
                            m[a][b] += w * t[a] * t[b];
                        }
                        m[a][kTerms] += w * t[a] * z;
                    }
                }
            }
            for (std::size_t a = 0; a < kTerms; ++a) {
                for (std::size_t b = 0; b < a; ++b) {
                    m[a][b] = m[b][a];
                }
            }
            std::array<double, kTerms> p{};
            if (!solve(m, p)) {
                error = "runway " + runway.name + ": the fit is singular at " + std::to_string(uk) +
                        " m";
                return std::nullopt;
            }
            // At the station (du = 0), in metres across.
            s.stations_[k] = {p[0], p[3] / half, p[4] / half, p[5] / (half * half)};
        }
        if (pass == kRobustPasses) {
            break;
        }
        for (std::size_t i = 0; i < nu; ++i) {
            for (std::size_t j = 0; j < nv; ++j) {
                residual[i * nv + j] =
                    static_cast<double>(h[i * nv + j]) -
                    s.surface(static_cast<double>(i) * du, -half + static_cast<double>(j) * dv);
            }
        }
        scratch.resize(residual.size());
        std::transform(residual.begin(), residual.end(), scratch.begin(),
                       [](double r) { return std::abs(r); });
        auto mid = scratch.begin() + static_cast<std::ptrdiff_t>(scratch.size() / 2);
        std::nth_element(scratch.begin(), mid, scratch.end());
        const double scale = std::max(1.4826 * *mid, kMinScale) * kBisquare;
        for (std::size_t n = 0; n < residual.size(); ++n) {
            const double r = residual[n] / scale;
            robust[n] = std::abs(r) < 1.0 ? (1.0 - r * r) * (1.0 - r * r) : 0.0;
        }
    }

    double sum = 0.0;
    for (std::size_t i = 0; i < nu; ++i) {
        for (std::size_t j = 0; j < nv; ++j) {
            const double r =
                static_cast<double>(h[i * nv + j]) -
                s.surface(static_cast<double>(i) * du, -half + static_cast<double>(j) * dv);
            sum += r * r;
            s.max_abs_ = std::max(s.max_abs_, std::abs(r));
        }
    }
    s.rms_ = std::sqrt(sum / static_cast<double>(nu * nv));
    s.sources_ = std::move(h);
    s.nu_ = nu;
    s.nv_ = nv;
    s.du_ = du;
    s.dv_ = dv;
    return s;
}

DVec3 RunwaySurface::to_ecef(double along_m, double across_m) const {
    const double north = along_m * axis_north_ - across_m * axis_east_;
    const double east = along_m * axis_east_ + across_m * axis_north_;
    return origin_ecef_ + glm::transpose(to_local_) * DVec3(north, east, 0.0);
}

RunwayPoint RunwaySurface::to_runway(double lat_deg, double lon_deg) const {
    const DVec3 l = to_local_ * (geodetic_to_ecef({lat_deg, lon_deg, 0.0}) - origin_ecef_);
    return {l.x * axis_north_ + l.y * axis_east_, -l.x * axis_east_ + l.y * axis_north_};
}

Geodetic RunwaySurface::to_geodetic(const RunwayPoint& p) const {
    Geodetic g = ecef_to_geodetic(to_ecef(p.along_m, p.across_m));
    g.alt_m = 0.0;  // the frame is a plane, rising off the ellipsoid away from the first end
    return g;
}

double RunwaySurface::heading_deg(double lat_deg, double lon_deg) const {
    // The axis in that point's own north and east: meridians converge, so
    // it differs from the midpoint's by a hundredth of a degree a km here.
    const DVec3 axis = glm::transpose(to_local_) * DVec3(axis_north_, axis_east_, 0.0);
    const DVec3 there = glm::transpose(ned_to_ecef(lat_deg, lon_deg)) * axis;
    const double h = glm::degrees(std::atan2(there.y, there.x));
    return h < 0.0 ? h + 360.0 : h;
}

bool RunwaySurface::touches(const GeoBounds& b) const {
    return reach_.west_deg <= b.east_deg && reach_.east_deg >= b.west_deg &&
           reach_.south_deg <= b.north_deg && reach_.north_deg >= b.south_deg;
}

double RunwaySurface::surface(double along_m, double across_m) const {
    const double half = runway_.width_m / 2.0;
    const double u = std::clamp(along_m, 0.0, length_);
    const double v = std::clamp(across_m, -half, half);
    const double f = u / station_step_;
    const auto k = std::min(static_cast<std::size_t>(f), stations_.size() - 2);
    const double t = f - static_cast<double>(k);
    const auto at = [&](const Station& st) {
        return st.a + st.b * v + st.c * std::abs(v) + st.d * v * v;
    };
    return at(stations_[k]) * (1.0 - t) + at(stations_[k + 1]) * t;
}

double RunwaySurface::source_at(double along_m, double across_m) const {
    const double fu = std::clamp(along_m / du_, 0.0, static_cast<double>(nu_ - 1));
    const double fv =
        std::clamp((across_m + runway_.width_m / 2.0) / dv_, 0.0, static_cast<double>(nv_ - 1));
    const auto i = std::min(static_cast<std::size_t>(fu), nu_ - 2);
    const auto j = std::min(static_cast<std::size_t>(fv), nv_ - 2);
    const double tu = fu - static_cast<double>(i);
    const double tv = fv - static_cast<double>(j);
    const auto at = [&](std::size_t a, std::size_t b) {
        return static_cast<double>(sources_[a * nv_ + b]);
    };
    return (at(i, j) * (1.0 - tv) + at(i, j + 1) * tv) * (1.0 - tu) +
           (at(i + 1, j) * (1.0 - tv) + at(i + 1, j + 1) * tv) * tu;
}

double RunwaySurface::blended(double lat_deg, double lon_deg, double raw) const {
    const RunwayPoint p = to_runway(lat_deg, lon_deg);
    const double half = runway_.width_m / 2.0;
    const double out = std::hypot(std::max({0.0, -p.along_m, p.along_m - length_}),
                                  std::max(0.0, std::abs(p.across_m) - half));
    if (out <= 0.0) {
        return surface(p.along_m, p.across_m);
    }
    if (out >= blend_) {
        return raw;
    }
    // The correction at the nearest edge point, fading out (smoothstep):
    // at the edge the height is the surface's, past the blend the sources'.
    const double u = std::clamp(p.along_m, 0.0, length_);
    const double v = std::clamp(p.across_m, -half, half);
    const double t = out / blend_;
    const double w = 1.0 - t * t * (3.0 - 2.0 * t);
    return raw + w * (surface(u, v) - source_at(u, v));
}

void apply_runways(std::span<const RunwaySurface> runways, const TileKey& key,
                   std::uint32_t samples, std::vector<float>& heights) {
    const GeoBounds b = tile_bounds(key);
    const double step = (b.east_deg - b.west_deg) / (samples - 1);
    for (const RunwaySurface& r : runways) {
        if (!r.touches(b)) {
            continue;
        }
        for (std::uint32_t row = 0; row < samples; ++row) {
            for (std::uint32_t col = 0; col < samples; ++col) {
                const double lat = b.south_deg + row * step;
                const double lon = b.west_deg + col * step;
                if (!r.touches({lat, lat, lon, lon})) {
                    continue;
                }
                float& h = heights[std::size_t{row} * samples + col];
                h = static_cast<float>(r.blended(lat, lon, static_cast<double>(h)));
            }
        }
    }
}

}  // namespace fovea::compiler
