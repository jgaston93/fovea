// Runway light layouts (milestone 6 step 8, D-033) on made-up runways,
// against AC 150/5340-30J: the edge lights' spacing and colours, the
// threshold and end lights' green and red, a displaced threshold, the
// centreline's colour coding, the touchdown zone's bars and PAPIs. Then the
// real airport files, laid out whole.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <numbers>
#include <ranges>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lights.hpp"
#include "settings.hpp"

namespace fovea::compiler {
namespace {

constexpr double kFt = 0.3048;
using enum LightColour;

Runway runway(double width_ft, const std::string& edge = "HIGH") {
    Runway rw;
    rw.name = "18/36";
    rw.width_m = width_ft * kFt;
    rw.edge_lights = edge;
    rw.ends[0].id = "18";
    rw.ends[1].id = "36";
    rw.ends[0].markings = "PIR";
    rw.ends[1].markings = "PIR";
    return rw;
}

std::vector<RunwayLight> of(const RunwayLayout& layout, LightType type) {
    std::vector<RunwayLight> out;
    for (const RunwayLight& l : layout.lights) {
        if (l.type == type) {
            out.push_back(l);
        }
    }
    std::ranges::sort(out, {},
                      [](const RunwayLight& l) { return std::pair(l.at.along_m, l.at.across_m); });
    return out;
}

// One side's lights of a type, along the runway.
std::vector<RunwayLight> side(const std::vector<RunwayLight>& lights, double sign) {
    std::vector<RunwayLight> out;
    for (const RunwayLight& l : lights) {
        if (l.at.across_m * sign > 0.0) {
            out.push_back(l);
        }
    }
    return out;
}

// 2.3.1.2.1: two lines 10 ft outside the pavement's edges, mirrored, no
// more than 200 ft apart and uniform from the threshold lights' lines.
TEST(RunwayLights, EdgeLightsAreEvenAndSymmetricAtTheStandardsSpacing) {
    const double length = 3000.0;
    const RunwayLayout layout = layout_lights(runway(150.0), length);
    EXPECT_TRUE(layout.skipped.empty());
    const auto edges = of(layout, LightType::edge);
    const auto left = side(edges, -1.0);
    const auto right = side(edges, 1.0);
    ASSERT_EQ(left.size(), right.size());
    // 3000 m plus 10 ft at each end, at most 200 ft apart: 50 intervals.
    ASSERT_EQ(left.size(), 49U);
    const double step = (length + 20.0 * kFt) / 50.0;
    EXPECT_LE(step, 200.0 * kFt);
    for (std::size_t k = 0; k < left.size(); ++k) {
        EXPECT_NEAR(left[k].at.across_m, -(75.0 + 10.0) * kFt, 1e-9);
        EXPECT_NEAR(right[k].at.across_m, (75.0 + 10.0) * kFt, 1e-9);
        EXPECT_NEAR(left[k].at.along_m, right[k].at.along_m, 1e-9);
        EXPECT_NEAR(left[k].at.along_m, -10.0 * kFt + step * static_cast<double>(k + 1), 1e-6);
    }
}

// 2.3.2.1.1, 2.3.2.2.1, Figure A-3: four a side, 10 ft before each end, the
// outermost in line with the edge lights and the rest on 10 ft centres
// inward; green seen landing on that end, red seen rolling toward it.
TEST(RunwayLights, ThresholdLightsAreGreenOutwardAndRedInward) {
    const double length = 2500.0;
    const auto ends = of(layout_lights(runway(150.0), length), LightType::threshold);
    ASSERT_EQ(ends.size(), 16U);
    for (std::size_t e = 0; e < 2; ++e) {
        const std::vector<RunwayLight> bar(ends.begin() + static_cast<long>(e * 8),
                                           ends.begin() + static_cast<long>(e * 8 + 8));
        const double at = e == 0 ? -10.0 * kFt : length + 10.0 * kFt;
        const std::vector<double> across = {-85.0, -75.0, -65.0, -55.0, 55.0, 65.0, 75.0, 85.0};
        for (std::size_t k = 0; k < 8; ++k) {
            EXPECT_NEAR(bar[k].at.along_m, at, 1e-9);
            EXPECT_NEAR(bar[k].at.across_m, across[k] * kFt, 1e-9);
            EXPECT_EQ(bar[k].colour[e], green) << e;
            EXPECT_EQ(bar[k].colour[1 - e], red) << e;
        }
    }
    // Three a side with MIRL on a visual runway (2.3.2.2.1 item 5).
    Runway visual = runway(100.0, "MED");
    visual.ends[0].markings = "VFR";
    visual.ends[1].markings = "VFR";
    EXPECT_EQ(of(layout_lights(visual, length), LightType::threshold).size(), 12U);
}

// 2.3.1.1.2: yellow seen over the last 2,000 ft (or half a short runway)
// toward an instrument end's far end, white before; none on a visual end.
TEST(RunwayLights, EdgeLightsTurnYellowInTheCautionZone) {
    for (const double length : {3000.0, 1000.0}) {
        const double zone = std::min(2000.0 * kFt, 0.5 * length);
        const auto edges = of(layout_lights(runway(150.0), length), LightType::edge);
        for (const RunwayLight& l : edges) {
            const double left_from_first = length - l.at.along_m;
            EXPECT_EQ(l.colour[0], left_from_first <= zone ? yellow : white) << l.at.along_m;
            EXPECT_EQ(l.colour[1], l.at.along_m <= zone ? yellow : white) << l.at.along_m;
        }
    }
    Runway visual = runway(150.0);
    visual.ends[0].markings = "VFR";
    for (const RunwayLight& l : of(layout_lights(visual, 3000.0), LightType::edge)) {
        EXPECT_EQ(l.colour[0], white);  // landing on the visual end
    }
}

// 2.3.2.1.2, 2.3.2.2.2, Figure A-5: a threshold displaced 400 ft from the
// second end. Before it the edge lights are red toward its approach, its
// lights stand at the threshold, green toward the approach and outward from
// the edge line, and the runway's end there is red both ways.
TEST(RunwayLights, ADisplacedThresholdIsGreenWithRedBeforeIt) {
    const double length = 3000.0;
    const double displaced = 400.0 * kFt;
    Runway rw = runway(150.0);
    rw.ends[1].displaced_threshold_m = displaced;
    const RunwayLayout layout = layout_lights(rw, length);
    const double threshold_at = length - displaced;
    std::size_t before = 0;
    for (const RunwayLight& l : of(layout, LightType::edge)) {
        if (l.at.along_m > threshold_at) {
            ++before;
            EXPECT_EQ(l.colour[1], red);
            EXPECT_EQ(l.colour[0], yellow);  // the caution zone, from the other end
        }
    }
    EXPECT_EQ(before, 2U * 2U);  // 410 ft: three intervals of at most 200 ft
    std::size_t at_end = 0;
    std::size_t at_threshold = 0;
    for (const RunwayLight& l : of(layout, LightType::threshold)) {
        if (std::abs(l.at.along_m - (length + 10.0 * kFt)) < 1e-9) {
            ++at_end;
            EXPECT_EQ(l.colour[0], red);
            EXPECT_EQ(l.colour[1], red);
        } else if (std::abs(l.at.along_m - threshold_at) < 1e-9) {
            ++at_threshold;
            const double out = std::abs(l.at.across_m) - 85.0 * kFt;
            EXPECT_GT(out, -1e-9);  // in line with the edge lights or outward
            EXPECT_EQ(l.colour[1], green);
            EXPECT_EQ(l.colour[0], out < 1e-9 ? yellow : none);
        }
    }
    EXPECT_EQ(at_end, 8U);
    EXPECT_EQ(at_threshold, 8U);
    // Red and yellow on a visual runway too (2.3.2.2.2 item 2), whose bars
    // have three a side (2.3.2.2.1 item 5).
    rw.ends[0].markings = "VFR";
    rw.ends[1].markings = "VFR";
    const RunwayLayout visual = layout_lights(rw, length);
    for (const RunwayLight& l : of(visual, LightType::edge)) {
        EXPECT_EQ(l.colour[0], l.at.along_m > threshold_at ? yellow : white) << l.at.along_m;
    }
    EXPECT_EQ(of(visual, LightType::threshold).size(), 6U + 6U + 6U);
    // Centreline lights are blanked toward the approach over it (3.3.1.3.1),
    // and the touchdown zone starts from it (3.3.2.2).
    rw.ends[1].centreline_lights = true;
    rw.ends[1].touchdown_zone_lights = true;
    const RunwayLayout lit = layout_lights(rw, length);
    for (const RunwayLight& l : of(lit, LightType::centreline)) {
        EXPECT_EQ(l.colour[1] == none, l.at.along_m > threshold_at) << l.at.along_m;
        EXPECT_NE(l.colour[0], none);
    }
    double nearest = length;
    for (const RunwayLight& l : of(lit, LightType::touchdown_zone)) {
        nearest = std::min(nearest, threshold_at - l.at.along_m);
    }
    EXPECT_NEAR(nearest, 100.0 * kFt, 1e-6);
    // Thresholds displaced past each other are refused.
    rw.ends[0].displaced_threshold_m = length - displaced;
    const RunwayLayout overlap = layout_lights(rw, length);
    EXPECT_TRUE(overlap.lights.empty());
    EXPECT_EQ(overlap.skipped.size(), 1U);
}

// 3.3.1, Figure A-34: every 50 ft from 75 ft in from each end; seen toward
// either end, red over its last 1,000 ft, red and white alternating from
// 3,000 ft (red first), white before.
TEST(RunwayLights, CentrelineLightsAreColourCodedTowardEachEnd) {
    const double length = 12600.0 * kFt;
    Runway rw = runway(150.0);
    rw.ends[0].centreline_lights = true;
    const auto lights = of(layout_lights(rw, length), LightType::centreline);
    ASSERT_EQ(lights.size(), 250U);  // (12,600 - 150) / 50 + 1
    EXPECT_NEAR(lights.front().at.along_m, 75.0 * kFt, 1e-9);
    EXPECT_NEAR(lights.back().at.along_m, length - 75.0 * kFt, 1e-9);
    for (std::size_t e = 0; e < 2; ++e) {
        std::string seen;  // in travel order from end e
        for (std::size_t k = 0; k < lights.size(); ++k) {
            const auto& l = lights[e == 0 ? k : lights.size() - 1 - k];
            EXPECT_EQ(l.at.across_m, 0.0);
            EXPECT_TRUE(l.colour[e] == white || l.colour[e] == red);
            seen += l.colour[e] == white ? 'W' : 'R';
        }
        // The last 1,000 ft: lights at 75 to 975 ft from the end, 19; the
        // 2,000 ft before them: 40 more, alternating from red.
        std::string coded;
        for (int k = 0; k < 40; ++k) {
            coded += k % 2 == 0 ? 'R' : 'W';
        }
        EXPECT_EQ(seen, std::string(250 - 59, 'W') + coded + std::string(19, 'R')) << e;
    }
}

// 3.3.2, Figure A-35: 30 bars of three a side, every 100 ft from 100 ft past
// the threshold, 36, 41 and 46 ft from the centreline, seen only from that
// end's approach. On a runway shorter than 6,000 ft, over half of it.
TEST(RunwayLights, TouchdownZoneBarsRunFromTheThreshold) {
    const double length = 12600.0 * kFt;
    Runway rw = runway(150.0);
    rw.ends[1].touchdown_zone_lights = true;
    const auto lights = of(layout_lights(rw, length), LightType::touchdown_zone);
    ASSERT_EQ(lights.size(), 30U * 6U);
    std::map<long, int> bars;
    for (const RunwayLight& l : lights) {
        const double from_end = (length - l.at.along_m) / kFt;
        ++bars[std::lround(from_end)];
        const double across = std::abs(l.at.across_m) / kFt;
        EXPECT_TRUE(std::abs(across - 36.0) < 1e-6 || std::abs(across - 41.0) < 1e-6 ||
                    std::abs(across - 46.0) < 1e-6)
            << across;
        EXPECT_EQ(l.colour[1], white);
        EXPECT_EQ(l.colour[0], none);
    }
    ASSERT_EQ(bars.size(), 30U);
    EXPECT_EQ(bars.begin()->first, 100);
    EXPECT_EQ(bars.rbegin()->first, 3000);
    const auto short_one = of(layout_lights(rw, 5000.0 * kFt), LightType::touchdown_zone);
    EXPECT_EQ(short_one.size(), 25U * 6U);  // 2,500 ft
}

// 7.4.4, 7.5.4, Tables 7-2 and 7-3: on the side named, the inboard unit
// 50 ft from the edge, sited for the threshold crossing height on the
// lowest on-course signal; the unit nearest the runway aimed highest.
TEST(RunwayLights, PapiIsSitedForItsThresholdCrossingHeight) {
    const double length = 3000.0;
    Runway rw = runway(150.0);
    rw.ends[0].vgsi = "P4L";
    rw.ends[0].glide_path_deg = 3.0;
    rw.ends[0].threshold_crossing_height_m = 73.0 * kFt;
    rw.ends[1].vgsi = "P2R";
    rw.ends[1].threshold_crossing_height_m = 50.0 * kFt;  // no angle: 3 degrees
    rw.ends[1].displaced_threshold_m = 200.0 * kFt;
    const RunwayLayout layout = layout_lights(rw, length);
    EXPECT_TRUE(layout.skipped.empty());
    std::vector<RunwayLight> first;
    std::vector<RunwayLight> second;
    for (const RunwayLight& l : of(layout, LightType::papi)) {
        (l.colour[0] == white ? first : second).push_back(l);
    }
    ASSERT_EQ(first.size(), 4U);
    ASSERT_EQ(second.size(), 2U);
    // Landing on the first end, left is negative across; the inboard unit
    // is aimed 35' above the glide path, then 15' above, 15' and 35' below.
    std::ranges::sort(first, std::greater{}, [](const RunwayLight& l) { return l.at.across_m; });
    const double d1 = 73.0 * kFt / std::tan((3.0 - 10.0 / 60.0) * std::numbers::pi / 180.0);
    const std::vector<double> aims = {35.0, 15.0, -15.0, -35.0};
    for (std::size_t k = 0; k < 4; ++k) {
        EXPECT_NEAR(first[k].at.along_m, d1, 1e-6);
        EXPECT_NEAR(first[k].at.across_m, -(75.0 + 50.0 + 30.0 * static_cast<double>(k)) * kFt,
                    1e-9);
        EXPECT_NEAR(first[k].papi_angle_deg, 3.0 + aims[k] / 60.0, 1e-12);
        EXPECT_EQ(first[k].colour[1], none);
    }
    // Landing on the second end, right is negative across too; measured
    // from its displaced threshold.
    std::ranges::sort(second, std::greater{}, [](const RunwayLight& l) { return l.at.across_m; });
    const double d2 = 50.0 * kFt / std::tan((3.0 - 10.0 / 60.0) * std::numbers::pi / 180.0);
    EXPECT_NEAR(second[0].at.along_m, length - 200.0 * kFt - d2, 1e-6);
    EXPECT_NEAR(second[0].at.across_m, -125.0 * kFt, 1e-9);
    EXPECT_NEAR(second[0].papi_angle_deg, 3.25, 1e-12);
    EXPECT_NEAR(second[1].papi_angle_deg, 2.75, 1e-12);
    // The AC's worked example (7.5.4.3.6): TCH 45 ft, 3 degrees: 909.26 ft
    // (its 2 degrees 50' rounded to 2.833; exactly, 909.25).
    rw.ends[0].threshold_crossing_height_m = 45.0 * kFt;
    const auto example = of(layout_lights(rw, length), LightType::papi);
    EXPECT_NEAR(example.front().at.along_m / kFt, 909.26, 0.02);
    // What isn't laid out is said.
    rw.ends[0].vgsi = "VAS4";
    rw.ends[1].approach_lights = "MALSR";
    rw.ends[1].threshold_crossing_height_m = 0.0;
    EXPECT_EQ(layout_lights(rw, length).skipped.size(), 3U);
}

// The committed airport files, read as the compiler reads them and laid
// out whole: the systems each lists, and only its approach lights skipped.
TEST(RunwayLights, TheAirportFilesLayOut) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "fovea_runway_lights";
    fs::create_directories(dir / "dem");
    std::ofstream(dir / "dem" / "a.tif") << "x";
    const fs::path airports = fs::path(FOVEA_SOURCE_DIR) / "data" / "airports";
    const std::string settings =
        "name = \"t\"\narea = { south = 1.0, north = 2.0, west = 3.0, east = 4.0 }\n"
        "output = \"db\"\n[levels]\nroot = 3\nfinest = 5\n[datum]\nepoch = 2026.0\n"
        "[elevation]\nsamples = 65\ncache = \".\"\nsources = [\"dem/a.tif\"]\n"
        "geoid = \"g.tif\"\n[runways]\nairport = [\"" +
        (airports / "khsv.toml").generic_string() + "\", \"" +
        (airports / "khua.toml").generic_string() + "\"]\n";
    const SettingsResult r = parse_settings(settings, "s.toml", dir);
    ASSERT_TRUE(r.settings.has_value()) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_EQ(r.settings->runways.size(), 3U);
    std::map<std::string, std::map<LightType, std::size_t>> counts;
    std::map<std::string, std::size_t> skipped;
    for (const Runway& rw : r.settings->runways) {
        const RunwayLayout layout = layout_lights(rw, rw.length_m);
        for (const RunwayLight& l : layout.lights) {
            ++counts[rw.name][l.type];
        }
        skipped[rw.name] = layout.skipped.size();
    }
    // 18R/36L: ALSF-2 and MALSR skipped; centreline and both TDZs.
    EXPECT_EQ(skipped["KHSV 18R/36L"], 2U);
    EXPECT_EQ(counts["KHSV 18R/36L"][LightType::touchdown_zone], 2U * 30U * 6U);
    EXPECT_EQ(counts["KHSV 18R/36L"][LightType::centreline], 250U);
    EXPECT_EQ(counts["KHSV 18R/36L"][LightType::papi], 0U);
    // 18L/36R: two MALSRs skipped, two PAPIs, a displaced threshold.
    EXPECT_EQ(skipped["KHSV 18L/36R"], 2U);
    EXPECT_EQ(counts["KHSV 18L/36R"][LightType::papi], 8U);
    EXPECT_EQ(counts["KHSV 18L/36R"][LightType::threshold], 3U * 8U);
    // 17/35: one MALSR skipped, two PAPIs.
    EXPECT_EQ(skipped["KHUA 17/35"], 1U);
    EXPECT_EQ(counts["KHUA 17/35"][LightType::papi], 8U);
    EXPECT_EQ(counts["KHUA 17/35"][LightType::threshold], 16U);
}

}  // namespace
}  // namespace fovea::compiler
