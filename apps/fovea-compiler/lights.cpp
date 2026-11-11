#include "lights.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>
#include <unordered_map>

#include "finest_heights.hpp"

namespace fovea::compiler {

namespace {

constexpr double kFoot = 0.3048;

// Edge lights (AC 150/5340-30J 2.3.1.2.1): 2 to 10 ft outside the
// pavement's edge, 10 ft recommended where jets use the runway (both
// airports' runways), and no more than 200 ft apart.
constexpr double kEdgeOffset = 10.0 * kFoot;
constexpr double kMaxEdgeSpacing = 200.0 * kFoot;
// Threshold and runway end lights (2.3.2.2.1): 2 to 10 ft before the
// threshold, on 10 ft centres. 10 ft, like the edge lights.
constexpr double kThresholdSetback = 10.0 * kFoot;
constexpr double kThresholdSpacing = 10.0 * kFoot;
// The edge lights' caution zone (2.3.1.1.2): the last 2,000 ft or half the
// runway, whichever is less.
constexpr double kCautionZone = 2000.0 * kFoot;
// Centreline lights (3.3.1.1, Figure A-34): every 50 ft, the first and last
// 75 ft from the runway's ends. Colour coded (3.3.1.2) over the last
// 3,000 ft, red over the last 1,000; blanked toward the approach over a
// displaced threshold of 700 ft or less (3.3.1.3.1).
constexpr double kCentrelineSpacing = 50.0 * kFoot;
constexpr double kCentrelineFromEnd = 75.0 * kFoot;
constexpr double kCentrelineCoded = 3000.0 * kFoot;
constexpr double kCentrelineRed = 1000.0 * kFoot;
constexpr double kCentrelineBlanked = 700.0 * kFoot;
// Touchdown zone lights (3.3.2, Figure A-35): bars of three every 100 ft
// from 100 ft past the threshold to 3,000 ft (half the runway, if shorter
// than 6,000 ft), the lights 36, 41 and 46 ft either side of the
// centreline.
constexpr double kTdzBarSpacing = 100.0 * kFoot;
constexpr double kTdzZone = 3000.0 * kFoot;
constexpr double kTdzShortRunway = 6000.0 * kFoot;
constexpr std::array<double, 3> kTdzAcross = {36.0 * kFoot, 41.0 * kFoot, 46.0 * kFoot};
// PAPI (7.5.4.7.1, 7.5.4.7.2): the inboard unit 50 ft from the runway's
// edge, the units 20 to 30 ft apart (30 here).
constexpr double kPapiFromEdge = 50.0 * kFoot;
constexpr double kPapiSpacing = 30.0 * kFoot;
// The standard visual glide path (7.5.4.3.4), when none is given.
constexpr double kStandardGlidePath = 3.0;
// The lowest on-course signal, minutes of arc below the glide path, for
// siting (7.5.4.3.3).
constexpr double kLowestOnCourse = 10.0;

double radians(double deg) {
    return deg * std::numbers::pi / 180.0;
}

// Instrument runway ends (2.3.1.1.2 item 3: their edge lights get a
// caution zone), from the end's markings.
bool instrument(const RunwayEnd& end) {
    return end.markings == "PIR" || end.markings == "NPI";
}

class Layout {
public:
    Layout(const Runway& runway, double length_m, RunwayLayout& out)
        : rw_(runway), length_(length_m), out_(out) {
        edge_across_ = 0.5 * rw_.width_m + kEdgeOffset;
    }

    // Metres along the runway (from the first end) of a point `s` metres
    // in from end `i`; and the sign of "right" travelling from end i.
    double along(std::size_t i, double s) const { return i == 0 ? s : length_ - s; }
    static double right(std::size_t i) { return i == 0 ? 1.0 : -1.0; }
    double from(std::size_t i, double along_m) const {
        return i == 0 ? along_m : length_ - along_m;
    }

    void add(double along_m, double across_m, LightType type, LightColour first, LightColour second,
             double papi_angle_deg = 0.0) {
        out_.lights.push_back({{along_m, across_m}, type, {first, second}, papi_angle_deg});
    }
    void add_toward(std::size_t i, double along_m, double across_m, LightType type,
                    LightColour toward_i, LightColour toward_other, double papi_angle_deg = 0.0) {
        if (i == 0) {
            add(along_m, across_m, type, toward_i, toward_other, papi_angle_deg);
        } else {
            add(along_m, across_m, type, toward_other, toward_i, papi_angle_deg);
        }
    }

    void skip(const std::string& what) { out_.skipped.push_back(rw_.name + ": " + what); }

    // The edge lights' colour seen travelling from end i (2.3.1.1, 2.3.2.1.2).
    LightColour edge_colour(std::size_t i, double along_m) const {
        const double s = from(i, along_m);
        const RunwayEnd& end = rw_.ends[i];
        if (s < end.displaced_threshold_m) {
            return LightColour::red;  // before the threshold: red toward the approach
        }
        if (length_ - s < rw_.ends[1 - i].displaced_threshold_m) {
            // Before the far end's displaced threshold: yellow, on any
            // runway (2.3.2.2.2 item 2, Figure A-5).
            return LightColour::yellow;
        }
        const bool caution = (rw_.edge_lights == "HIGH" || rw_.edge_lights == "MED") &&
                             instrument(end) &&
                             length_ - s <= std::min(kCautionZone, 0.5 * length_);
        return caution ? LightColour::yellow : LightColour::white;
    }

    void edges() {
        if (rw_.edge_lights.empty()) {
            return;
        }
        if (rw_.edge_lights != "HIGH" && rw_.edge_lights != "MED" && rw_.edge_lights != "LOW") {
            skip("edge lights " + rw_.edge_lights + ": not a system laid out here");
            return;
        }
        // The lines of threshold and runway end lights, and of displaced
        // thresholds, are the reference points the edge lights are spaced
        // from (2.3.1.2.1 item 5), uniformly between each (2.3.2.2.2
        // item 3).
        std::vector<double> lines = {-kThresholdSetback, length_ + kThresholdSetback};
        for (std::size_t i = 0; i < 2; ++i) {
            const double d = rw_.ends[i].displaced_threshold_m;
            if (d > 0.0) {
                lines.push_back(along(i, d));
            }
            threshold(i);
        }
        std::ranges::sort(lines);
        for (std::size_t l = 0; l + 1 < lines.size(); ++l) {
            const double span = lines[l + 1] - lines[l];
            const auto n = static_cast<int>(std::ceil(span / kMaxEdgeSpacing - 1e-9));
            for (int k = 1; k < n; ++k) {
                const double a = lines[l] + span * k / n;
                for (const double side : {-1.0, 1.0}) {
                    add(a, side * edge_across_, LightType::edge, edge_colour(0, a),
                        edge_colour(1, a));
                }
            }
        }
    }

    // Threshold and runway end lights at end i (2.3.2, Figures A-3, A-5).
    void threshold(std::size_t i) {
        const RunwayEnd& end = rw_.ends[i];
        const std::size_t other = 1 - i;
        // Four a side on instrument runways with HIRL or MIRL (2.3.2.2.1
        // item 6), three on visual runways (item 5; Figure A-3 note 1).
        const bool four = rw_.edge_lights != "LOW" && instrument(end);
        const int count = four ? 4 : 3;
        const double at_end = along(i, -kThresholdSetback);
        const double d = end.displaced_threshold_m;
        for (const double side : {-1.0, 1.0}) {
            for (int k = 0; k < count; ++k) {
                // From the line of edge lights inward (2.3.2.2.1 items 1, 2).
                const double across = side * (edge_across_ - k * kThresholdSpacing);
                if (d > 0.0) {
                    // The runway's end, before a displaced threshold: red
                    // (Figure A-5).
                    add(at_end, across, LightType::threshold, LightColour::red, LightColour::red);
                } else {
                    // Green outward, red toward the runway (2.3.2.1.1).
                    add_toward(i, at_end, across, LightType::threshold, LightColour::green,
                               LightColour::red);
                }
            }
            if (d > 0.0) {
                // A displaced threshold's lights: at the threshold (Figure
                // A-5, note 6), the innermost in line with the edge lights
                // and the rest outward (2.3.2.2.2 item 1), green toward the
                // approach (2.3.2.1.2 item 2). The innermost is also an edge
                // light the other way; the rest are unidirectional.
                const double a = along(i, d);
                for (int k = 0; k < count; ++k) {
                    const double across = side * (edge_across_ + k * kThresholdSpacing);
                    add_toward(i, a, across, LightType::threshold, LightColour::green,
                               k == 0 ? edge_colour(other, a) : LightColour::none);
                }
            }
        }
    }

    // Centreline lights (3.3.1), where either end lists them.
    void centreline() {
        if (!rw_.ends[0].centreline_lights && !rw_.ends[1].centreline_lights) {
            return;
        }
        const double span = length_ - 2.0 * kCentrelineFromEnd;
        if (span <= 0.0) {
            return;
        }
        const auto n = std::max(1, static_cast<int>(std::lround(span / kCentrelineSpacing)));
        const std::size_t first = out_.lights.size();
        for (int k = 0; k <= n; ++k) {
            add(kCentrelineFromEnd + span * k / n, 0.0, LightType::centreline, LightColour::none,
                LightColour::none);
        }
        for (std::size_t i = 0; i < 2; ++i) {
            const double d = rw_.ends[i].displaced_threshold_m;
            // In travel order from end i, counting the alternating lights.
            int alternating = 0;
            for (int k = 0; k <= n; ++k) {
                RunwayLight& light =
                    out_.lights[first + static_cast<std::size_t>(i == 0 ? k : n - k)];
                const double s = from(i, light.at.along_m);
                const double remaining = length_ - s;
                LightColour c = LightColour::white;
                // Over a longer displaced area they are on a circuit of their
                // own, to be turned off for landings (3.3.1.3.2): lit here.
                if (d > 0.0 && d <= kCentrelineBlanked && s < d) {
                    c = LightColour::none;
                } else if (remaining <= kCentrelineRed) {
                    c = LightColour::red;
                } else if (remaining <= kCentrelineCoded) {
                    // Alternating, starting with red (3.3.1.2).
                    c = alternating++ % 2 == 0 ? LightColour::red : LightColour::white;
                }
                light.colour[i] = c;
            }
        }
    }

    // Touchdown zone lights (3.3.2) at end i: unidirectional, toward its
    // approach (3.3.2.1). White: the AC names the fixture, L-850B
    // (3.5.2.2), whose colour is AC 150/5345-46's (not fetched). Their 4
    // degree toe-in (3.3.2.3) is left to the renderer.
    void touchdown_zone(std::size_t i) {
        const RunwayEnd& end = rw_.ends[i];
        if (!end.touchdown_zone_lights) {
            return;
        }
        const double zone = length_ < kTdzShortRunway ? 0.5 * length_ : kTdzZone;
        for (int k = 1; k * kTdzBarSpacing <= zone + 1e-6; ++k) {
            const double a = along(i, end.displaced_threshold_m + k * kTdzBarSpacing);
            for (const double side : {-1.0, 1.0}) {
                for (const double across : kTdzAcross) {
                    add_toward(i, a, side * across, LightType::touchdown_zone, LightColour::white,
                               LightColour::none);
                }
            }
        }
    }

    // A PAPI at end i (7.4.4, 7.5.4): "P4L" is four units on the left,
    // "P2R" two on the right, seen from the approach.
    void papi(std::size_t i) {
        const RunwayEnd& end = rw_.ends[i];
        if (end.vgsi.empty()) {
            return;
        }
        const std::string& v = end.vgsi;
        if (v.size() != 3 || v[0] != 'P' || (v[1] != '2' && v[1] != '4') ||
            (v[2] != 'L' && v[2] != 'R')) {
            skip(end.id + ": VGSI " + v + ": not a system laid out here");
            return;
        }
        if (!(end.threshold_crossing_height_m > 0.0)) {
            skip(end.id + ": PAPI: needs threshold_crossing_height_ft");
            return;
        }
        const double glide = end.glide_path_deg > 0.0 ? end.glide_path_deg : kStandardGlidePath;
        // Aiming, in minutes of arc from the glide path, the unit nearest
        // the runway first: the standard installation of Table 7-2 (four
        // units) and Table 7-3 (two).
        static constexpr std::array<double, 4> kFour = {35.0, 15.0, -15.0, -35.0};
        static constexpr std::array<double, 2> kTwo = {15.0, -15.0};
        const bool four = v[1] == '4';
        const std::span<const double> aims =
            four ? std::span<const double>(kFour) : std::span<const double>(kTwo);
        // The distance from the threshold that gives the threshold crossing
        // height (7.5.4.3.1): TCH x cot(the lowest on-course signal), the
        // third unit's for four units, 10' below the glide path (7.5.4.3.3,
        // worked in 7.5.4.3.6), the farthest's for two (7.5.4.4.4). Table
        // 7-2 aims the third unit 15' below; the siting paragraph's 10' is
        // used here, as its example does. A runway with an ILS sites its PAPI at the glide
        // slope's touchdown point instead (7.5.4.2), which NASR doesn't
        // give; and the gradient correction (7.5.4.7.6) is left out.
        const double lowest = glide - kLowestOnCourse / 60.0;
        const double distance = end.threshold_crossing_height_m / std::tan(radians(lowest));
        const double a = along(i, end.displaced_threshold_m + distance);
        const double side = v[2] == 'R' ? right(i) : -right(i);
        for (std::size_t k = 0; k < aims.size(); ++k) {
            const double across =
                side * (0.5 * rw_.width_m + kPapiFromEdge + static_cast<double>(k) * kPapiSpacing);
            add_toward(i, a, across, LightType::papi, LightColour::white, LightColour::none,
                       glide + aims[k] / 60.0);
        }
    }

    void unsupported(std::size_t i) {
        const RunwayEnd& end = rw_.ends[i];
        if (!end.approach_lights.empty()) {
            skip(end.id + ": approach lights " + end.approach_lights +
                 ": laid out in FAA JO 6850.2, not yet here");
        }
        if (end.reil) {
            skip(end.id + ": REIL: not yet laid out (Figure A-79)");
        }
    }

private:
    const Runway& rw_;
    double length_;
    RunwayLayout& out_;
    double edge_across_ = 0.0;
};

}  // namespace

RunwayLayout layout_lights(const Runway& runway, double length_m) {
    RunwayLayout out;
    if (runway.ends[0].displaced_threshold_m + runway.ends[1].displaced_threshold_m >= length_m) {
        out.skipped.push_back(runway.name + ": its displaced thresholds overlap: no lights");
        return out;
    }
    Layout layout(runway, length_m, out);
    layout.edges();
    layout.centreline();
    for (std::size_t i = 0; i < 2; ++i) {
        layout.touchdown_zone(i);
        layout.papi(i);
        layout.unsupported(i);
    }
    return out;
}

bool bake_lights(std::span<const RunwaySurface> runways, const std::filesystem::path& part,
                 const DatabaseManifest& manifest, std::uint8_t level, std::uint8_t finest,
                 LightsReport& report, std::vector<std::string>& errors) {
    FinestHeights heights(part, finest);
    std::unordered_map<TileKey, std::vector<TileLight>> by_tile;
    for (const RunwaySurface& surface : runways) {
        const Runway& rw = surface.runway();
        RunwayLayout layout = layout_lights(rw, surface.length_m());
        report.skipped.insert(report.skipped.end(), layout.skipped.begin(), layout.skipped.end());
        for (const RunwayLight& light : layout.lights) {
            const Geodetic g = surface.to_geodetic(light.at);
            const TileKey key = tile_at(g.lat_deg, g.lon_deg, level);
            const auto ground = heights.at(g.lat_deg, g.lon_deg);
            if (!manifest.has_tile(key) || !ground) {
                errors.push_back("runway " + rw.name + ": a light is outside the database");
                return false;
            }
            // One a hair under 360 that rounds to it as a float is 0.
            auto heading = static_cast<float>(surface.heading_deg(g.lat_deg, g.lon_deg));
            heading = heading >= 360.0F ? 0.0F : heading;
            by_tile[key].push_back({g.lat_deg, g.lon_deg, *ground, heading,
                                    static_cast<float>(light.papi_angle_deg), light.type,
                                    light.colour});
            ++report.lights;
        }
    }
    for (const auto& [key, lights] : by_tile) {
        const std::filesystem::path file = part / tile_path(key);
        TileResult r = read_tile(file, key);
        const auto layer = encode_lights(lights);
        if (!r.tile || !layer) {
            errors.push_back(file.string() + ": cannot add its lights");
            return false;
        }
        r.tile->layers.push_back(*layer);
        if (!write_tile(file, *r.tile)) {
            errors.push_back(file.string() + ": cannot write");
            return false;
        }
    }
    report.tiles = by_tile.size();
    return true;
}

}  // namespace fovea::compiler
