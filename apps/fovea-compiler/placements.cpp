#include "placements.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <unordered_map>

#include <toml++/toml.hpp>

#include "finest_heights.hpp"
#include "fovea/core/geodesy.hpp"
#include "fovea/models/model.hpp"

namespace fovea::compiler {

namespace fs = std::filesystem;

PlacementFileResult parse_placement_file(const std::string& text, const fs::path& file) {
    PlacementFileResult r;
    toml::table root;
    try {
        root = toml::parse(text, file.string());
    } catch (const toml::parse_error& e) {
        r.errors.push_back(file.string() + ":" + std::to_string(e.source().begin.line) + ": " +
                           std::string(e.description()));
        return r;
    }
    const auto* list = root["placement"].as_array();
    if (list == nullptr || list->empty()) {
        r.errors.push_back(file.string() + ": no [[placement]]");
        return r;
    }
    for (const auto& node : *list) {
        const std::string where = file.string() + ":" + std::to_string(node.source().begin.line);
        const auto* t = node.as_table();
        PlacementSpec p;
        p.where = where;
        const auto model = t != nullptr ? (*t)["model"].value<std::string>() : std::nullopt;
        const auto lat = t != nullptr ? (*t)["lat"].value<double>() : std::nullopt;
        const auto lon = t != nullptr ? (*t)["lon"].value<double>() : std::nullopt;
        const auto heading = t != nullptr ? (*t)["heading"].value<double>() : std::nullopt;
        if (!model || !lat || !lon || !heading) {
            r.errors.push_back(where + ": a [[placement]] needs model, lat, lon and heading");
            continue;
        }
        p.model = *model;
        p.lat_deg = *lat;
        p.lon_deg = *lon;
        // Into [0, 360): -0.9 is 359.1, and one a hair under 360 that
        // rounds to it as a float is 0.
        double h = std::fmod(*heading, 360.0);
        h = h < 0.0 ? h + 360.0 : h;
        p.heading_deg = static_cast<float>(h);
        if (p.heading_deg >= 360.0F) {
            p.heading_deg = 0.0F;
        }
        // valid_placement's checks, but the height (found later).
        if (!valid_placement({p.model, p.lat_deg, p.lon_deg, 0.0, p.heading_deg})) {
            r.errors.push_back(where +
                               ": needs a model name of letters, digits, _ and - (at most 64), a "
                               "point on the Earth and a heading from 0 up to 360");
            continue;
        }
        r.placements.push_back(std::move(p));
    }
    return r;
}

bool bake_placements(const std::vector<PlacementSpec>& placements, const fs::path& models_dir,
                     const fs::path& part, const DatabaseManifest& manifest, std::uint8_t level,
                     std::uint8_t finest, PlacementReport& report,
                     std::vector<std::string>& errors) {
    // Each model once: its footprint (bounds across and front to back) and
    // a copy in the database.
    struct Footprint {
        double x0, x1, z0, z1;
    };
    std::map<std::string, Footprint> models;
    std::error_code ec;
    fs::create_directories(part / "models", ec);
    for (const PlacementSpec& p : placements) {
        if (models.contains(p.model)) {
            continue;
        }
        const fs::path from = models_dir / (p.model + ".glb");
        const ModelResult m = read_model_file(from);
        if (!m.model) {
            errors.push_back(p.where + ": model '" + p.model + "': " + m.error);
            return false;
        }
        fs::copy_file(from, part / model_path(p.model), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            errors.push_back(p.where + ": cannot copy " + from.string() + ": " + ec.message());
            return false;
        }
        const auto& lo = m.model->min;
        const auto& hi = m.model->max;
        models.emplace(p.model, Footprint{static_cast<double>(lo[0]), static_cast<double>(hi[0]),
                                          static_cast<double>(lo[2]), static_cast<double>(hi[2])});
    }

    // Each on the lowest ground under its footprint: the finest level at
    // its corners and every metre between (KHSV's grid is 1.2 m).
    FinestHeights heights(part, finest);
    std::unordered_map<TileKey, std::vector<Placement>> by_tile;
    for (const PlacementSpec& p : placements) {
        const TileKey key = tile_at(p.lat_deg, p.lon_deg, level);
        if (!manifest.has_tile(key)) {
            errors.push_back(p.where + ": outside the database");
            return false;
        }
        const Footprint& f = models.at(p.model);
        // The model's frame: +Z its front, along the heading; +X its left.
        const double h = glm::radians(static_cast<double>(p.heading_deg));
        const DMat3 ned = ned_to_ecef(p.lat_deg, p.lon_deg);
        const DVec3 origin = geodetic_to_ecef({p.lat_deg, p.lon_deg, 0.0});
        const auto steps = [](double a, double b) {
            return std::max(1, static_cast<int>(std::ceil(b - a)));  // a metre: under the grid's
        };
        const int nx = steps(f.x0, f.x1);
        const int nz = steps(f.z0, f.z1);
        double lowest = 1e300;
        double highest = -1e300;
        for (int i = 0; i <= nx; ++i) {
            for (int j = 0; j <= nz; ++j) {
                const double x = f.x0 + (f.x1 - f.x0) * i / nx;
                const double z = f.z0 + (f.z1 - f.z0) * j / nz;
                const double north = z * std::cos(h) + x * std::sin(h);
                const double east = z * std::sin(h) - x * std::cos(h);
                const Geodetic g = ecef_to_geodetic(origin + ned * DVec3(north, east, 0.0));
                const auto ground = heights.at(g.lat_deg, g.lon_deg);
                if (!ground) {
                    errors.push_back(p.where + ": its footprint reaches past the database");
                    return false;
                }
                lowest = std::min(lowest, *ground);
                highest = std::max(highest, *ground);
            }
        }
        if (highest - lowest > report.largest_rise_m) {
            report.largest_rise_m = highest - lowest;
            report.largest_rise_model = p.model;
        }
        by_tile[key].push_back({p.model, p.lat_deg, p.lon_deg, lowest, p.heading_deg});
    }

    for (const auto& [key, placed] : by_tile) {
        const fs::path file = part / tile_path(key);
        TileResult r = read_tile(file, key);
        const auto layer = encode_placements(placed);
        if (!r.tile || !layer) {
            errors.push_back(file.string() + ": cannot add its placements");
            return false;
        }
        r.tile->layers.push_back(*layer);
        if (!write_tile(file, *r.tile)) {
            errors.push_back(file.string() + ": cannot write");
            return false;
        }
    }
    report.placements = placements.size();
    report.models = models.size();
    report.tiles = by_tile.size();
    return true;
}

}  // namespace fovea::compiler
