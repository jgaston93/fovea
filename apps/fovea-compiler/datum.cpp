#include "datum.hpp"

#include <algorithm>
#include <cmath>

#include <cpl_error.h>
#include <ogr_srs_api.h>

namespace fovea::compiler {

namespace {

// The grid's spacing and how far it reaches past the area: runway blends
// and placements near the edges are converted too.
constexpr double kStepDeg = 0.01;
constexpr double kMarginDeg = 0.05;

// Null if PROJ's database doesn't have it (WGS 84 (G2139) needs EPSG 10.0
// or later).
OGRSpatialReferenceH geographic_3d(int epsg) {
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    if (OSRImportFromEPSG(srs, epsg) != OGRERR_NONE) {
        OSRDestroySpatialReference(srs);
        return nullptr;
    }
    OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
    return srs;
}

}  // namespace

std::optional<DatumShift> DatumShift::make(const GeoBounds& area, double epoch,
                                           std::string& error) {
    if (!std::isfinite(epoch) || epoch < 1990.0 || epoch > 2100.0) {
        error = "datum epoch " + std::to_string(epoch) + ": not a year from 1990 to 2100";
        return std::nullopt;
    }
    DatumShift d;
    d.epoch_ = epoch;
    d.area_ = {area.south_deg - kMarginDeg, area.north_deg + kMarginDeg, area.west_deg - kMarginDeg,
               area.east_deg + kMarginDeg};
    d.rows_ =
        static_cast<std::size_t>(std::ceil((d.area_.north_deg - d.area_.south_deg) / kStepDeg)) + 1;
    d.columns_ =
        static_cast<std::size_t>(std::ceil((d.area_.east_deg - d.area_.west_deg) / kStepDeg)) + 1;
    d.area_.north_deg = d.area_.south_deg + static_cast<double>(d.rows_ - 1) * kStepDeg;
    d.area_.east_deg = d.area_.west_deg + static_cast<double>(d.columns_ - 1) * kStepDeg;

    OGRSpatialReferenceH wgs84 = geographic_3d(9754);  // WGS 84 (G2139)
    OGRSpatialReferenceH nad83 = geographic_3d(6319);  // NAD83(2011)
    if (wgs84 == nullptr || nad83 == nullptr) {
        error = std::string("PROJ's database lacks EPSG:") + (wgs84 == nullptr ? "9754" : "6319") +
                (wgs84 == nullptr ? " (WGS 84 (G2139))" : " (NAD83(2011))") +
                "; PROJ 9.1 or later has both";
        OSRDestroySpatialReference(wgs84);
        OSRDestroySpatialReference(nad83);
        return std::nullopt;
    }
    OGRCoordinateTransformationOptionsH options = OCTNewCoordinateTransformationOptions();
    // PROJ's ballpark transformation between two datums changes nothing.
    OCTCoordinateTransformationOptionsSetBallparkAllowed(options, FALSE);
    OGRCoordinateTransformationH to_nad83 = OCTNewCoordinateTransformationEx(wgs84, nad83, options);
    OCTDestroyCoordinateTransformationOptions(options);
    OSRDestroySpatialReference(wgs84);
    OSRDestroySpatialReference(nad83);
    if (to_nad83 == nullptr) {
        const char* msg = CPLGetLastErrorMsg();
        error = std::string("no transformation from WGS 84 (G2139) to NAD83(2011): ") +
                (msg != nullptr && *msg != '\0' ? msg : "PROJ found none");
        return std::nullopt;
    }
    const std::size_t count = d.rows_ * d.columns_;
    std::vector<double> x(count);
    std::vector<double> y(count);
    std::vector<double> z(count, 0.0);
    std::vector<double> t(count, epoch);
    std::vector<int> ok(count, 0);
    for (std::size_t r = 0; r < d.rows_; ++r) {
        for (std::size_t c = 0; c < d.columns_; ++c) {
            x[r * d.columns_ + c] = d.area_.west_deg + static_cast<double>(c) * kStepDeg;
            y[r * d.columns_ + c] = d.area_.south_deg + static_cast<double>(r) * kStepDeg;
        }
    }
    const std::vector<double> lon0 = x;
    const std::vector<double> lat0 = y;
    (void)OCTTransform4D(to_nad83, static_cast<int>(count), x.data(), y.data(), z.data(), t.data(),
                         ok.data());
    OCTDestroyCoordinateTransformation(to_nad83);
    if (!std::all_of(ok.begin(), ok.end(), [](int v) { return v != 0; })) {
        error = "WGS 84 (G2139) to NAD83(2011) failed over the area";
        return std::nullopt;
    }
    d.grid_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        // A height of 0 in WGS-84 is z in NAD83: WGS-84's is NAD83's less z.
        d.grid_[i] = {y[i] - lat0[i], x[i] - lon0[i], -z[i]};
    }
    return d;
}

DatumShift DatumShift::none() {
    return {};
}

DatumShift::Shift DatumShift::at(double lat_deg, double lon_deg) const {
    if (grid_.empty()) {
        return {};
    }
    // Clamped at the grid's edge: past it the shift barely changes.
    const double fr =
        std::clamp((lat_deg - area_.south_deg) / kStepDeg, 0.0, static_cast<double>(rows_ - 1));
    const double fc =
        std::clamp((lon_deg - area_.west_deg) / kStepDeg, 0.0, static_cast<double>(columns_ - 1));
    const std::size_t r0 = std::min(static_cast<std::size_t>(fr), rows_ - 2);
    const std::size_t c0 = std::min(static_cast<std::size_t>(fc), columns_ - 2);
    const double v = fr - static_cast<double>(r0);
    const double u = fc - static_cast<double>(c0);
    const auto lerp = [&](double Shift::*field) {
        const auto g = [&](std::size_t r, std::size_t c) { return grid_[r * columns_ + c].*field; };
        return (g(r0, c0) * (1.0 - u) + g(r0, c0 + 1) * u) * (1.0 - v) +
               (g(r0 + 1, c0) * (1.0 - u) + g(r0 + 1, c0 + 1) * u) * v;
    };
    return {lerp(&Shift::lat), lerp(&Shift::lon), lerp(&Shift::height)};
}

Nad83Point DatumShift::to_nad83(double lat_deg, double lon_deg) const {
    const Shift s = at(lat_deg, lon_deg);
    return {lat_deg + s.lat, lon_deg + s.lon, s.height};
}

void DatumShift::to_wgs84(double& lat_deg, double& lon_deg) const {
    // The WGS-84 point whose shift lands on this one: the shift barely
    // changes over the metre between them, so two rounds settle it.
    const double lat_n = lat_deg;
    const double lon_n = lon_deg;
    for (int i = 0; i < 2; ++i) {
        const Shift s = at(lat_deg, lon_deg);
        lat_deg = lat_n - s.lat;
        lon_deg = lon_n - s.lon;
    }
}

}  // namespace fovea::compiler
