#include "fovea/terrain/database.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <ranges>
#include <sstream>

#include <toml++/toml.hpp>

#include "fovea/core/geodesy.hpp"

namespace fovea {

bool DatabaseManifest::has_tile(const TileKey& key) const {
    return std::any_of(levels.begin(), levels.end(),
                       [&](const LevelTiles& l) { return l.contains(key); });
}

std::optional<std::uint8_t> DatabaseManifest::finest_level() const {
    if (levels.empty()) {
        return std::nullopt;
    }
    return std::max_element(
               levels.begin(), levels.end(),
               [](const LevelTiles& a, const LevelTiles& b) { return a.level < b.level; })
        ->level;
}

std::filesystem::path tile_path(const TileKey& key) {
    return std::filesystem::path("tiles") / std::to_string(key.level) / std::to_string(key.x) /
           (std::to_string(key.y) + ".fvt");
}

std::string manifest_text(const DatabaseManifest& m) {
    std::ostringstream s;
    s.precision(17);
    s << "# A Fovea terrain database (D-018); written by fovea-compiler.\n"
      << "schema = " << m.schema << "\n"
      << "name = \"" << m.name << "\"\n\n"
      << "[area]\nsouth = " << m.area.south_deg << "\nnorth = " << m.area.north_deg
      << "\nwest = " << m.area.west_deg << "\neast = " << m.area.east_deg << "\n\n"
      << "[elevation]\nsamples = " << m.elevation_samples << "\n";
    for (const LevelTiles& l : m.levels) {
        s << "\n[[level]]\nlevel = " << int{l.level} << "\nx = [" << l.x0 << ", " << l.x1
          << "]\ny = [" << l.y0 << ", " << l.y1 << "]\n";
    }
    return s.str();
}

bool write_manifest(const std::filesystem::path& dir, const DatabaseManifest& m) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / "manifest.toml", std::ios::trunc);
    out << manifest_text(m);
    return static_cast<bool>(out);
}

ManifestResult parse_manifest(const std::string& text, const std::string& source) {
    ManifestResult r;
    auto error = [&](const toml::source_region& at, const std::string& msg) {
        r.errors.push_back(source + ":" + std::to_string(at.begin.line) + ": " + msg);
    };
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        error(e.source(), std::string(e.description()));
        return r;
    }
    DatabaseManifest m;
    const auto schema = root["schema"].value<std::int64_t>();
    if (!schema) {
        error(root.source(), "missing 'schema'");
    } else if (*schema != kDatabaseSchema) {
        error(root.get("schema")->source(), "database schema " + std::to_string(*schema) +
                                                ", this Fovea reads " +
                                                std::to_string(kDatabaseSchema));
    }
    m.name = root["name"].value_or(std::string{});
    const auto number = [&](toml::node_view<toml::node> n, const char* what) {
        const auto v = n.value<double>();
        if (!v) {
            error(root.source(), std::string("missing or not a number: '") + what + "'");
        }
        return v.value_or(0.0);
    };
    m.area = {number(root["area"]["south"], "area.south"),
              number(root["area"]["north"], "area.north"),
              number(root["area"]["west"], "area.west"), number(root["area"]["east"], "area.east")};
    m.elevation_samples =
        static_cast<std::uint32_t>(root["elevation"]["samples"].value_or(std::int64_t{0}));
    if (m.elevation_samples < 2 || m.elevation_samples > 8193) {
        error(root.source(), "'elevation.samples' must be 2 to 8193");
    }
    if (const auto* list = root["level"].as_array()) {
        for (const auto& node : *list) {
            const auto* t = node.as_table();
            const auto level = t != nullptr ? (*t)["level"].value<std::int64_t>() : std::nullopt;
            const auto* xs = t != nullptr ? (*t)["x"].as_array() : nullptr;
            const auto* ys = t != nullptr ? (*t)["y"].as_array() : nullptr;
            if (!level || *level < 0 || *level > kMaxTileLevel || xs == nullptr || ys == nullptr ||
                xs->size() != 2 || ys->size() != 2) {
                error(node.source(),
                      "a [[level]] needs level (0-30), x = [first, last] and "
                      "y = [first, last]");
                continue;
            }
            const auto at = [](const toml::array& a, std::size_t i) {
                return a[i].value<std::int64_t>().value_or(-1);
            };
            const auto lv = static_cast<std::uint8_t>(*level);
            const std::int64_t x0 = at(*xs, 0);
            const std::int64_t x1 = at(*xs, 1);
            const std::int64_t y0 = at(*ys, 0);
            const std::int64_t y1 = at(*ys, 1);
            if (x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0 || x1 >= tile_columns(lv) ||
                y1 >= tile_rows(lv)) {
                error(node.source(), "level " + std::to_string(lv) +
                                         ": tile ranges out of order "
                                         "or past the level's tiles");
                continue;
            }
            m.levels.push_back({lv, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(x1),
                                static_cast<std::uint32_t>(y0), static_cast<std::uint32_t>(y1)});
        }
    }
    if (m.levels.empty()) {
        error(root.source(), "no [[level]]: the database has no tiles");
    }
    std::sort(m.levels.begin(), m.levels.end(),
              [](const LevelTiles& a, const LevelTiles& b) { return a.level < b.level; });
    if (r.errors.empty()) {
        r.manifest = std::move(m);
    }
    return r;
}

ManifestResult TerrainDatabase::open(const std::filesystem::path& dir,
                                     std::unique_ptr<TerrainDatabase>& out) {
    const auto file = dir / "manifest.toml";
    std::ifstream in(file);
    if (!in) {
        return {std::nullopt, {file.string() + ": cannot read the database manifest"}};
    }
    std::ostringstream text;
    text << in.rdbuf();
    ManifestResult r = parse_manifest(text.str(), file.string());
    if (r.manifest) {
        out.reset(new TerrainDatabase(dir, *r.manifest));
    }
    return r;
}

TileResult TerrainDatabase::read(const TileKey& key) const {
    if (!manifest_.has_tile(key)) {
        return {std::nullopt, TileError::cannot_read,
                "tile " + to_string(key) + " isn't in the database"};
    }
    return read_tile(dir_ / tile_path(key), key);
}

const ElevationGrid* DatabaseTerrain::grid(const TileKey& key) const {
    const std::lock_guard lock(mutex_);
    if (const auto it = grids_.find(key); it != grids_.end()) {
        return it->second.get();
    }
    std::unique_ptr<ElevationGrid> grid;
    TileResult r = db_.read(key);
    if (r.tile) {
        const TileLayer* layer = r.tile->find(kElevationLayer);
        if (layer == nullptr) {
            problems_.push_back(to_string(key) + ": no elevation layer");
        } else if (auto g = decode_elevation(*layer)) {
            grid = std::make_unique<ElevationGrid>(std::move(*g));
        } else {
            problems_.push_back(to_string(key) + ": the elevation layer doesn't decode");
        }
    } else {
        problems_.push_back(r.detail);
    }
    const ElevationGrid* out = grid.get();
    grids_.emplace(key, std::move(grid));  // a failure is remembered too
    return out;
}

std::vector<std::string> DatabaseTerrain::problems() const {
    const std::lock_guard lock(mutex_);
    return problems_;
}

std::optional<double> DatabaseTerrain::height_at(double lat_deg, double lon_deg) const {
    const auto& levels = db_.manifest().levels;
    for (const LevelTiles& level : std::views::reverse(levels)) {  // finest first
        const TileKey key = tile_at(lat_deg, lon_deg, level.level);
        if (!level.contains(key)) {
            continue;
        }
        if (const ElevationGrid* g = grid(key)) {
            return g->sample(tile_bounds(key), lat_deg, lon_deg);
        }
    }
    return std::nullopt;
}

std::optional<TerrainHit> DatabaseTerrain::intersect(const DRay& ray, double max_distance_m) const {
    const double length = glm::length(ray.direction);
    if (length == 0.0) {
        return std::nullopt;
    }
    const DVec3 unit = ray.direction / length;
    // Height above the terrain at distance t along the ray; nothing where
    // the database has no terrain.
    auto above = [&](double t) -> std::optional<double> {
        const Geodetic g = ecef_to_geodetic(ray.origin + unit * t);
        const auto h = height_at(g.lat_deg, g.lon_deg);
        if (!h) {
            return std::nullopt;
        }
        return g.alt_m - *h;
    };
    // The finest grid's spacing on the ground, for the smallest step.
    const auto finest = db_.manifest().finest_level().value_or(0);
    const double spacing_m = tile_size_deg(finest) / (db_.manifest().elevation_samples - 1) *
                             (wgs84::kA * std::numbers::pi / 180.0);
    const double min_step = std::max(spacing_m * 0.5, 0.01);
    constexpr double kNoTerrainStep = 50.0;

    const auto start = above(0.0);
    if (start && *start <= 0.0) {
        return TerrainHit{ray.origin, 0.0, kUnknownMaterial};  // starts on or below
    }
    double t = 0.0;
    std::optional<double> h = start;
    while (t < max_distance_m) {
        const double step = h ? std::clamp(*h * 0.5, min_step, 1000.0) : kNoTerrainStep;
        const double next_t = std::min(t + step, max_distance_m);
        const auto next = above(next_t);
        if (next && *next <= 0.0) {
            // Crossed between t and next_t: bisect to a millimetre.
            double lo = t;
            double hi = next_t;
            while (hi - lo > 1e-3) {
                const double mid = 0.5 * (lo + hi);
                const auto m = above(mid);
                if (m && *m <= 0.0) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            return TerrainHit{ray.origin + unit * hi, hi, kUnknownMaterial};
        }
        if (next_t >= max_distance_m) {
            break;
        }
        t = next_t;
        h = next;
    }
    return std::nullopt;
}

}  // namespace fovea
