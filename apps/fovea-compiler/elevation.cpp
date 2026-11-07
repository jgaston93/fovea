#include "elevation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <cpl_error.h>
#include <gdal.h>
#include <ogr_srs_api.h>

namespace fovea::compiler {

namespace {

std::string last_gdal_error() {
    const char* msg = CPLGetLastErrorMsg();
    return msg != nullptr && *msg != '\0' ? msg : "unknown GDAL error";
}

}  // namespace

void init_gdal() {
    GDALAllRegister();
}

struct ElevationSources::Source {
    std::filesystem::path file;
    GDALDatasetH dataset = nullptr;
    OGRCoordinateTransformationH to_source = nullptr;  // from NAD83 geographic
    std::array<double, 6> gt{};                        // north-up: gt[2] = gt[4] = 0
    int width = 0;
    int height = 0;
    bool has_nodata = false;
    float nodata = 0.0F;
    GeoBounds bounds;  // NAD83 geographic
};

std::unique_ptr<ElevationSources> ElevationSources::open(
    const std::vector<std::filesystem::path>& files, std::string& error) {
    std::unique_ptr<ElevationSources> s(new ElevationSources);
    OGRSpatialReferenceH geographic = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(geographic, 4269);  // NAD83
    OSRSetAxisMappingStrategy(geographic, OAMS_TRADITIONAL_GIS_ORDER);
    for (const auto& file : files) {
        s->sources_.emplace_back();  // closed by the destructor from here
        Source& src = s->sources_.back();
        src.file = file;
        src.dataset =
            GDALOpenEx(file.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (src.dataset == nullptr) {
            error = file.string() + ": " + last_gdal_error();
            break;
        }
        OGRSpatialReferenceH srs = GDALGetSpatialRef(src.dataset);
        if (GDALGetGeoTransform(src.dataset, src.gt.data()) != CE_None || srs == nullptr ||
            src.gt[2] != 0.0 || src.gt[4] != 0.0 || GDALGetRasterCount(src.dataset) < 1) {
            error = file.string() + ": not a north-up georeferenced raster";
            break;
        }
        src.width = GDALGetRasterXSize(src.dataset);
        src.height = GDALGetRasterYSize(src.dataset);
        int has_nodata = 0;
        const double nodata =
            GDALGetRasterNoDataValue(GDALGetRasterBand(src.dataset, 1), &has_nodata);
        src.has_nodata = has_nodata != 0;
        src.nodata = static_cast<float>(nodata);
        // The source's own axis order may differ (latitude first in EPSG's
        // geographic CRSs); x east, y north keeps the geotransform's meaning.
        OGRSpatialReferenceH own = OSRClone(srs);
        OSRSetAxisMappingStrategy(own, OAMS_TRADITIONAL_GIS_ORDER);
        src.to_source = OCTNewCoordinateTransformation(geographic, own);
        OGRCoordinateTransformationH back = OCTNewCoordinateTransformation(own, geographic);
        OSRDestroySpatialReference(own);
        const double w = src.gt[0];
        const double n = src.gt[3];
        const double e = src.gt[0] + src.gt[1] * src.width;
        const double sth = src.gt[3] + src.gt[5] * src.height;
        GeoBounds& b = src.bounds;
        const bool ok = src.to_source != nullptr && back != nullptr &&
                        OCTTransformBounds(back, std::min(w, e), std::min(n, sth), std::max(w, e),
                                           std::max(n, sth), &b.west_deg, &b.south_deg, &b.east_deg,
                                           &b.north_deg, 21) != 0;
        OCTDestroyCoordinateTransformation(back);
        if (!ok) {
            error = file.string() + ": cannot transform to and from it: " + last_gdal_error();
            break;
        }
    }
    OSRDestroySpatialReference(geographic);
    if (!error.empty()) {
        return nullptr;
    }
    return s;
}

ElevationSources::~ElevationSources() {
    for (const Source& src : sources_) {
        OCTDestroyCoordinateTransformation(src.to_source);
        GDALClose(src.dataset);
    }
}

bool ElevationSources::heights(const TileKey& key, std::uint32_t samples, std::vector<float>& out,
                               std::string& error) const {
    const GeoBounds b = tile_bounds(key);
    const double step = (b.east_deg - b.west_deg) / (samples - 1);
    const std::size_t count = std::size_t{samples} * samples;
    out.assign(count, std::numeric_limits<float>::quiet_NaN());

    // Each sample from the highest-priority source with all four pixels
    // around it: a sample depends on its point alone, so a tile and its
    // neighbour agree on their shared edge.
    std::vector<std::size_t> todo(count);
    for (std::size_t i = 0; i < count; ++i) {
        todo[i] = i;
    }
    std::vector<double> xs;
    std::vector<double> ys;
    std::vector<int> ok;
    std::vector<float> window;
    for (const Source& src : sources_) {
        if (todo.empty()) {
            break;
        }
        if (src.bounds.west_deg > b.east_deg || src.bounds.east_deg < b.west_deg ||
            src.bounds.south_deg > b.north_deg || src.bounds.north_deg < b.south_deg) {
            continue;  // elsewhere
        }
        xs.resize(todo.size());
        ys.resize(todo.size());
        ok.assign(todo.size(), 0);
        for (std::size_t i = 0; i < todo.size(); ++i) {
            const std::size_t row = todo[i] / samples;
            const std::size_t col = todo[i] % samples;
            xs[i] = b.west_deg + static_cast<double>(col) * step;
            ys[i] = b.south_deg + static_cast<double>(row) * step;
        }
        if (OCTTransformEx(src.to_source, static_cast<int>(todo.size()), xs.data(), ys.data(),
                           nullptr, ok.data()) == 0) {
            continue;  // none transforms: the source is elsewhere
        }
        // Pixel coordinates, centres at whole numbers.
        int c0 = std::numeric_limits<int>::max();
        int c1 = std::numeric_limits<int>::min();
        int r0 = c0;
        int r1 = c1;
        for (std::size_t i = 0; i < todo.size(); ++i) {
            xs[i] = (xs[i] - src.gt[0]) / src.gt[1] - 0.5;
            ys[i] = (ys[i] - src.gt[3]) / src.gt[5] - 0.5;
            if (ok[i] != 0 && std::isfinite(xs[i]) && std::isfinite(ys[i]) && xs[i] >= 0.0 &&
                ys[i] >= 0.0 && xs[i] < src.width - 1 && ys[i] < src.height - 1) {
                const int c = static_cast<int>(xs[i]);
                const int r = static_cast<int>(ys[i]);
                c0 = std::min(c0, c);
                c1 = std::max(c1, c + 1);
                r0 = std::min(r0, r);
                r1 = std::max(r1, r + 1);
            } else {
                ok[i] = 0;
            }
        }
        if (c0 > c1) {
            continue;
        }
        const int w = c1 - c0 + 1;
        const int h = r1 - r0 + 1;
        window.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        if (GDALRasterIO(GDALGetRasterBand(src.dataset, 1), GF_Read, c0, r0, w, h, window.data(), w,
                         h, GDT_Float32, 0, 0) != CE_None) {
            error = src.file.string() + ": " + last_gdal_error();
            return false;
        }
        const auto valid = [&](float v) {
            return std::isfinite(v) && (!src.has_nodata || v != src.nodata);
        };
        std::size_t kept = 0;
        for (std::size_t i = 0; i < todo.size(); ++i) {
            if (ok[i] != 0) {
                const int c = static_cast<int>(xs[i]);
                const int r = static_cast<int>(ys[i]);
                const std::size_t at =
                    static_cast<std::size_t>(r - r0) * static_cast<std::size_t>(w) +
                    static_cast<std::size_t>(c - c0);
                const float v00 = window[at];
                const float v01 = window[at + 1];
                const float v10 = window[at + static_cast<std::size_t>(w)];
                const float v11 = window[at + static_cast<std::size_t>(w) + 1];
                if (valid(v00) && valid(v01) && valid(v10) && valid(v11)) {
                    const double tx = xs[i] - c;
                    const double ty = ys[i] - r;
                    const double top =
                        static_cast<double>(v00) * (1 - tx) + static_cast<double>(v01) * tx;
                    const double bottom =
                        static_cast<double>(v10) * (1 - tx) + static_cast<double>(v11) * tx;
                    out[todo[i]] = static_cast<float>(top * (1 - ty) + bottom * ty);
                    continue;
                }
            }
            todo[kept++] = todo[i];
        }
        todo.resize(kept);
    }
    if (!todo.empty()) {
        error = "tile " + to_string(key) + ": " + std::to_string(todo.size()) + " of " +
                std::to_string(count) + " samples have no elevation source";
        return false;
    }
    return true;
}

std::optional<Geoid> Geoid::load(const std::filesystem::path& file, const GeoBounds& area,
                                 std::string& error) {
    GDALDatasetH ds =
        GDALOpenEx(file.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (ds == nullptr) {
        error = file.string() + ": " + last_gdal_error();
        return std::nullopt;
    }
    std::optional<Geoid> g;
    std::array<double, 6> gt{};
    const int width = GDALGetRasterXSize(ds);
    const int height = GDALGetRasterYSize(ds);
    if (GDALGetGeoTransform(ds, gt.data()) != CE_None || gt[2] != 0.0 || gt[4] != 0.0 ||
        GDALGetRasterCount(ds) < 1) {
        error = file.string() + ": not a north-up geographic grid";
        GDALClose(ds);
        return g;
    }
    g.emplace();
    // Grids east of 180 (0-360 longitudes, as GEOID18's) are read there.
    g->lon_shift_ = area.west_deg < gt[0] ? 360.0 : 0.0;
    // Pixel centres on either side of the area, and one more for margin.
    const auto column = [&](double lon) { return (lon + g->lon_shift_ - gt[0]) / gt[1] - 0.5; };
    const auto row = [&](double lat) { return (lat - gt[3]) / gt[5] - 0.5; };
    const int c0 =
        static_cast<int>(std::floor(std::min(column(area.west_deg), column(area.east_deg)))) - 1;
    const int c1 =
        static_cast<int>(std::ceil(std::max(column(area.west_deg), column(area.east_deg)))) + 1;
    const int r0 =
        static_cast<int>(std::floor(std::min(row(area.south_deg), row(area.north_deg)))) - 1;
    const int r1 =
        static_cast<int>(std::ceil(std::max(row(area.south_deg), row(area.north_deg)))) + 1;
    if (c0 < 0 || r0 < 0 || c1 >= width || r1 >= height) {
        error = file.string() + ": doesn't cover the area";
        GDALClose(ds);
        return std::nullopt;
    }
    g->columns_ = c1 - c0 + 1;
    g->rows_ = r1 - r0 + 1;
    g->origin_lon_ = gt[0] + (c0 + 0.5) * gt[1];
    g->origin_lat_ = gt[3] + (r0 + 0.5) * gt[5];
    g->step_lon_ = gt[1];
    g->step_lat_ = gt[5];
    g->values_.resize(static_cast<std::size_t>(g->columns_) * static_cast<std::size_t>(g->rows_));
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    const CPLErr read = GDALRasterIO(band, GF_Read, c0, r0, g->columns_, g->rows_,
                                     g->values_.data(), g->columns_, g->rows_, GDT_Float32, 0, 0);
    int has_nodata = 0;
    const double nodata = GDALGetRasterNoDataValue(band, &has_nodata);
    GDALClose(ds);
    if (read != CE_None) {
        error = file.string() + ": " + last_gdal_error();
        return std::nullopt;
    }
    const bool hole = std::any_of(g->values_.begin(), g->values_.end(), [&](float v) {
        return !std::isfinite(v) || (has_nodata != 0 && v == static_cast<float>(nodata));
    });
    if (hole) {
        error = file.string() + ": has no value somewhere over the area";
        return std::nullopt;
    }
    return g;
}

double Geoid::at(double lat_deg, double lon_deg) const {
    const double fx = (lon_deg + lon_shift_ - origin_lon_) / step_lon_;
    const double fy = (lat_deg - origin_lat_) / step_lat_;
    const int c = std::clamp(static_cast<int>(std::floor(fx)), 0, columns_ - 2);
    const int r = std::clamp(static_cast<int>(std::floor(fy)), 0, rows_ - 2);
    const double tx = std::clamp(fx - c, 0.0, 1.0);
    const double ty = std::clamp(fy - r, 0.0, 1.0);
    const auto v = [&](int rr, int cc) {
        return static_cast<double>(
            values_[static_cast<std::size_t>(rr) * static_cast<std::size_t>(columns_) +
                    static_cast<std::size_t>(cc)]);
    };
    const double top = v(r, c) * (1 - tx) + v(r, c + 1) * tx;
    const double bottom = v(r + 1, c) * (1 - tx) + v(r + 1, c + 1) * tx;
    return top * (1 - ty) + bottom * ty;
}

}  // namespace fovea::compiler
