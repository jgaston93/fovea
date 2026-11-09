#include "imagery.hpp"

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

struct ImagerySources::Source {
    std::filesystem::path file;
    std::size_t group = 0;
    GDALDatasetH dataset = nullptr;
    OGRCoordinateTransformationH to_source = nullptr;  // from NAD83 geographic
    std::array<double, 6> gt{};
    int width = 0;
    int height = 0;
    GeoBounds bounds;  // NAD83 geographic
    double pixel_lon_deg = 0.0;
    double pixel_lat_deg = 0.0;
};

std::unique_ptr<ImagerySources> ImagerySources::open(
    const std::vector<std::vector<std::filesystem::path>>& groups, std::string& error) {
    std::unique_ptr<ImagerySources> s(new ImagerySources);
    OGRSpatialReferenceH geographic = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(geographic, 4269);  // NAD83, as the elevation
    OSRSetAxisMappingStrategy(geographic, OAMS_TRADITIONAL_GIS_ORDER);
    for (std::size_t g = 0; g < groups.size() && error.empty(); ++g) {
        for (const auto& file : groups[g]) {
            s->sources_.emplace_back();  // closed by the destructor from here
            Source& src = s->sources_.back();
            src.file = file;
            src.group = g;
            src.dataset = GDALOpenEx(file.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                                     nullptr, nullptr);
            if (src.dataset == nullptr) {
                error = file.string() + ": " + last_gdal_error();
                break;
            }
            OGRSpatialReferenceH srs = GDALGetSpatialRef(src.dataset);
            if (GDALGetGeoTransform(src.dataset, src.gt.data()) != CE_None || srs == nullptr ||
                src.gt[2] != 0.0 || src.gt[4] != 0.0) {
                error = file.string() + ": not a north-up georeferenced raster";
                break;
            }
            src.width = GDALGetRasterXSize(src.dataset);
            src.height = GDALGetRasterYSize(src.dataset);
            bool rgb = GDALGetRasterCount(src.dataset) >= 3 && src.width >= 2 && src.height >= 2;
            const std::array<GDALColorInterp, 3> want = {GCI_RedBand, GCI_GreenBand, GCI_BlueBand};
            for (int b = 1; rgb && b <= 3; ++b) {
                GDALRasterBandH band = GDALGetRasterBand(src.dataset, b);
                rgb =
                    GDALGetRasterDataType(band) == GDT_Byte &&
                    GDALGetRasterColorInterpretation(band) == want[static_cast<std::size_t>(b - 1)];
            }
            if (!rgb) {
                error = file.string() +
                        ": needs red, green and blue as its first bands, 8 bits each, at least "
                        "2 x 2 pixels";
                break;
            }
            // No masks yet: every pixel is taken as imagery.
            if (GDALGetMaskFlags(GDALGetRasterBand(src.dataset, 1)) != GMF_ALL_VALID) {
                error = file.string() + ": has no-data pixels or a mask, not supported yet";
                break;
            }
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
                            OCTTransformBounds(back, std::min(w, e), std::min(n, sth),
                                               std::max(w, e), std::max(n, sth), &b.west_deg,
                                               &b.south_deg, &b.east_deg, &b.north_deg, 21) != 0;
            OCTDestroyCoordinateTransformation(back);
            if (!ok) {
                error = file.string() + ": cannot transform to and from it: " + last_gdal_error();
                break;
            }
            src.pixel_lon_deg = (b.east_deg - b.west_deg) / src.width;
            src.pixel_lat_deg = (b.north_deg - b.south_deg) / src.height;
        }
    }
    OSRDestroySpatialReference(geographic);
    if (!error.empty()) {
        return nullptr;
    }
    return s;
}

ImagerySources::~ImagerySources() {
    for (const Source& src : sources_) {
        OCTDestroyCoordinateTransformation(src.to_source);
        GDALClose(src.dataset);
    }
}

bool ImagerySources::texture(const TileKey& key, std::uint32_t texels, const DatumShift& datum,
                             Rgba8Image& out, std::string& error) const {
    const GeoBounds b = tile_bounds(key);
    // Padded by more than the datums' metre or two apart.
    constexpr double kPad = 1e-4;
    const auto overlaps = [&](const Source& src) {
        return src.bounds.west_deg < b.east_deg + kPad && src.bounds.east_deg > b.west_deg - kPad &&
               src.bounds.south_deg < b.north_deg + kPad &&
               src.bounds.north_deg > b.south_deg - kPad;
    };
    const double texel_deg = (b.east_deg - b.west_deg) / texels;
    const std::uint32_t side = texels;
    const std::size_t count = std::size_t{side} * side;
    const double step = texel_deg;

    // For each sample, the chosen source and where in it.
    constexpr auto kNone = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> pick(count, kNone);
    // Pixels to the nearest edge, then to the nearest edge on the other
    // axis: where files share an edge (the coverage's outer edge), the
    // first ties, and the second puts the seam midway.
    std::vector<std::pair<double, double>> depth(count, {-1.0, -1.0});
    std::vector<double> px(count);
    std::vector<double> py(count);
    std::vector<double> xs;
    std::vector<double> ys;
    std::vector<int> ok;
    // Each texel centre in the sources' NAD83(2011) (D-037), once.
    std::vector<double> nad_lat(count);
    std::vector<double> nad_lon(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Nad83Point p =
            datum.to_nad83(b.south_deg + (static_cast<double>(std::size_t{i / side}) + 0.5) * step,
                           b.west_deg + (static_cast<double>(i % side) + 0.5) * step);
        nad_lat[i] = p.lat_deg;
        nad_lon[i] = p.lon_deg;
    }
    for (std::size_t si = 0; si < sources_.size(); ++si) {
        const Source& src = sources_[si];
        if (!overlaps(src)) {
            continue;
        }
        xs = nad_lon;  // transformed in place
        ys = nad_lat;
        ok.assign(count, 0);
        // False when any point fails (GDAL 3.8), not only all: each point's
        // `ok` decides.
        (void)OCTTransformEx(src.to_source, static_cast<int>(count), xs.data(), ys.data(), nullptr,
                             ok.data());
        for (std::size_t i = 0; i < count; ++i) {
            if (pick[i] != kNone && sources_[pick[i]].group < src.group) {
                continue;  // a higher-priority group has it
            }
            const double x = (xs[i] - src.gt[0]) / src.gt[1] - 0.5;
            const double y = (ys[i] - src.gt[3]) / src.gt[5] - 0.5;
            const double dx = std::min(x, src.width - 1 - x);
            const double dy = std::min(y, src.height - 1 - y);
            const std::pair<double, double> d{std::min(dx, dy), std::max(dx, dy)};
            if (ok[i] != 0 && std::isfinite(d.second) && d.first >= 0.0 && d > depth[i]) {
                pick[i] = si;
                depth[i] = d;
                px[i] = x;
                py[i] = y;
            }
        }
    }

    const auto missing = static_cast<std::size_t>(std::count(pick.begin(), pick.end(), kNone));
    if (missing != 0) {
        error = "tile " + to_string(key) + ": " + std::to_string(missing) + " of " +
                std::to_string(count) + " imagery samples have no source";
        return false;
    }
    // Texels no further apart than about the chosen sources' pixels:
    // coarser texels would need each to average its area, or they alias.
    std::vector<bool> used(sources_.size(), false);
    for (const std::size_t si : pick) {
        used[si] = true;
    }
    for (std::size_t si = 0; si < sources_.size(); ++si) {
        const Source& src = sources_[si];
        if (used[si] &&
            (texel_deg > 1.5 * src.pixel_lon_deg || texel_deg > 1.5 * src.pixel_lat_deg)) {
            error = src.file.string() + ": pixels finer than the finest level's texels (" +
                    std::to_string(texel_deg) + " degrees); raise texels or the finest level";
            return false;
        }
    }

    // Each source's samples from one window of it.
    out = Rgba8Image(texels);
    std::vector<std::uint8_t> window;
    for (std::size_t si = 0; si < sources_.size(); ++si) {
        int c0 = std::numeric_limits<int>::max();
        int c1 = std::numeric_limits<int>::min();
        int r0 = c0;
        int r1 = c1;
        const Source& src = sources_[si];
        const auto cell = [&](std::size_t i, int& c, int& r) {
            c = std::min(static_cast<int>(px[i]), src.width - 2);
            r = std::min(static_cast<int>(py[i]), src.height - 2);
        };
        for (std::size_t i = 0; i < count; ++i) {
            if (pick[i] == si) {
                int c = 0;
                int r = 0;
                cell(i, c, r);
                c0 = std::min(c0, c);
                c1 = std::max(c1, c + 1);
                r0 = std::min(r0, r);
                r1 = std::max(r1, r + 1);
            }
        }
        if (c0 > c1) {
            continue;
        }
        const int w = c1 - c0 + 1;
        const int h = r1 - r0 + 1;
        window.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
        std::array<int, 3> bands = {1, 2, 3};
        if (GDALDatasetRasterIO(src.dataset, GF_Read, c0, r0, w, h, window.data(), w, h, GDT_Byte,
                                3, bands.data(), 3, w * 3, 1) != CE_None) {
            error = src.file.string() + ": " + last_gdal_error();
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (pick[i] != si) {
                continue;
            }
            int c = 0;
            int r = 0;
            cell(i, c, r);
            const double tx = px[i] - c;
            const double ty = py[i] - r;
            const auto v = [&](int rr, int cc, std::size_t ch) {
                return static_cast<double>(
                    window[(static_cast<std::size_t>(rr - r0) * static_cast<std::size_t>(w) +
                            static_cast<std::size_t>(cc - c0)) *
                               3 +
                           ch]);
            };
            for (std::size_t ch = 0; ch < 3; ++ch) {
                const double top = v(r, c, ch) * (1 - tx) + v(r, c + 1, ch) * tx;
                const double bottom = v(r + 1, c, ch) * (1 - tx) + v(r + 1, c + 1, ch) * tx;
                // Interpolated in the source's sRGB values.
                out.texels[i * 4 + ch] =
                    static_cast<std::uint8_t>(std::lround(top * (1 - ty) + bottom * ty));
            }
        }
    }
    return true;
}

}  // namespace fovea::compiler
