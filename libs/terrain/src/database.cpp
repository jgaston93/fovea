#include "fovea/terrain/database.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <ranges>
#include <sstream>
#include <string_view>

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

namespace {

// A TOML basic string: quoted, with quotes, backslashes and control
// characters escaped.
std::string toml_string(const std::string& v) {
    std::string out = "\"";
    for (const char c : v) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            static constexpr std::string_view kHex = "0123456789ABCDEF";
            out += "\\u00";
            out += kHex[(c >> 4) & 0xF];
            out += kHex[c & 0xF];
        } else {
            out += c;
        }
    }
    return out + "\"";
}

}  // namespace

std::string manifest_text(const DatabaseManifest& m) {
    std::ostringstream s;
    s.precision(17);
    s << "# A Fovea terrain database (D-018); written by fovea-compiler.\n"
      << "schema = " << m.schema << "\n"
      << "name = " << toml_string(m.name) << "\n\n"
      << "[area]\nsouth = " << m.area.south_deg << "\nnorth = " << m.area.north_deg
      << "\nwest = " << m.area.west_deg << "\neast = " << m.area.east_deg << "\n\n"
      << "[layers.elevation]\nformat = \"float32\"\nsamples = " << m.elevation_samples << "\n";
    for (const SourceInfo& src : m.sources) {
        s << "\n[[source]]\nlayer = " << toml_string(src.layer)
          << "\nname = " << toml_string(src.name) << "\ncredit = " << toml_string(src.credit)
          << "\n";
    }
    for (const LevelTiles& l : m.levels) {
        s << "\n[[level]]\nlevel = " << int{l.level} << "\nx = [" << l.x0 << ", " << l.x1
          << "]\ny = [" << l.y0 << ", " << l.y1 << "]\n";
    }
    return s.str();
}

bool write_manifest(const std::filesystem::path& dir, const DatabaseManifest& m) {
    const std::string text = manifest_text(m);
    return write_bytes_atomically(
        dir / "manifest.toml", {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
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
    const auto elevation = root["layers"]["elevation"];
    if (elevation["format"].value_or(std::string{}) != "float32") {
        error(root.source(), "[layers.elevation] needs format = \"float32\"");
    }
    // Checked before narrowing: a value past 32 bits mustn't wrap into range.
    const auto samples = elevation["samples"].value<std::int64_t>();
    if (!samples || *samples < 2 || *samples > 8193) {
        error(root.source(), "[layers.elevation] needs samples from 2 to 8193");
    } else {
        m.elevation_samples = static_cast<std::uint32_t>(*samples);
    }
    if (const auto* list = root["source"].as_array()) {
        for (const auto& node : *list) {
            if (const auto* t = node.as_table()) {
                m.sources.push_back({(*t)["layer"].value_or(std::string{}),
                                     (*t)["name"].value_or(std::string{}),
                                     (*t)["credit"].value_or(std::string{})});
            }
        }
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
    {
        const std::lock_guard lock(mutex_);
        if (const auto it = grids_.find(key); it != grids_.end()) {
            return it->second.get();
        }
    }
    // Read and decode without the lock, so other threads' cached lookups
    // don't wait behind the disk. Two threads may read the same tile once
    // each; the first to finish is kept.
    std::unique_ptr<ElevationGrid> grid;
    std::string problem;
    TileResult r = db_.read(key);
    if (!r.tile) {
        problem = r.detail;
    } else if (const TileLayer* layer = r.tile->find(kElevationLayer); layer == nullptr) {
        problem = to_string(key) + ": no elevation layer";
    } else if (auto g = decode_elevation(*layer); !g) {
        problem = to_string(key) + ": the elevation layer doesn't decode";
    } else if (g->size != db_.manifest().elevation_samples) {
        problem = to_string(key) + ": a " + std::to_string(g->size) + "-sample grid where the " +
                  "manifest says " + std::to_string(db_.manifest().elevation_samples);
    } else {
        grid = std::make_unique<ElevationGrid>(std::move(*g));
    }
    const std::lock_guard lock(mutex_);
    const auto [it, inserted] = grids_.emplace(key, std::move(grid));  // failures remembered too
    if (inserted && !problem.empty()) {
        problems_.push_back(problem);
    }
    return it->second.get();
}

std::vector<std::string> DatabaseTerrain::problems() const {
    const std::lock_guard lock(mutex_);
    return problems_;
}

std::optional<double> DatabaseTerrain::height_at(double lat_deg, double lon_deg) const {
    if (!std::isfinite(lat_deg) || !std::isfinite(lon_deg)) {
        return std::nullopt;
    }
    lon_deg = wrap_longitude(lon_deg);  // the tile and the sample agree
    for (const LevelTiles& level : std::views::reverse(db_.manifest().levels)) {  // finest first
        const TileKey key = tile_at(lat_deg, lon_deg, level.level);
        // A point on a tile's north or east edge is assigned to the tile
        // beyond it, but the tile it bounds holds that edge too: on the
        // database's own north or east edge, that's the one to use.
        for (const auto& [dx, dy] : {std::pair{0, 0}, {-1, 0}, {0, -1}, {-1, -1}}) {
            const auto k = neighbour(key, dx, dy);
            if (!k || !level.contains(*k)) {
                continue;
            }
            const GeoBounds b = tile_bounds(*k);
            if (lat_deg < b.south_deg || lat_deg > b.north_deg) {
                continue;
            }
            if (dx != 0 && lon_deg != b.east_deg && (b.east_deg != 180.0 || lon_deg != -180.0)) {
                continue;
            }
            if (dy != 0 && lat_deg != b.north_deg) {
                continue;
            }
            if (const ElevationGrid* g = grid(*k)) {
                return g->sample(b, lat_deg, lon_deg);
            }
        }
    }
    return std::nullopt;
}

std::optional<TerrainHit> DatabaseTerrain::intersect(const DRay& ray, double max_distance_m) const {
    const double length = glm::length(ray.direction);
    if (length == 0.0 || !std::isfinite(length) || !(max_distance_m > 0.0)) {
        return std::nullopt;
    }
    const DVec3 unit = ray.direction / length;
    // Where any tile reaches (tiles extend past the nominal area to their
    // own edges).
    GeoBounds area{90.0, -90.0, 180.0, -180.0};
    for (const LevelTiles& l : db_.manifest().levels) {
        const GeoBounds sw = tile_bounds({l.level, l.x0, l.y0});
        const GeoBounds ne = tile_bounds({l.level, l.x1, l.y1});
        area = {std::min(area.south_deg, sw.south_deg), std::max(area.north_deg, ne.north_deg),
                std::min(area.west_deg, sw.west_deg), std::max(area.east_deg, ne.east_deg)};
    }
    const auto outside = [&](const Geodetic& g) {
        return g.lat_deg < area.south_deg || g.lat_deg > area.north_deg ||
               g.lon_deg < area.west_deg || g.lon_deg > area.east_deg;
    };
    // Height above the terrain at distance t along the ray; nothing where
    // the database has no terrain.
    struct Sample {
        std::optional<double> above;
        bool outside_area = false;
    };
    auto sample = [&](double t) {
        const Geodetic g = ecef_to_geodetic(ray.origin + unit * t);
        const auto h = height_at(g.lat_deg, g.lon_deg);
        return Sample{h ? std::optional<double>(g.alt_m - *h) : std::nullopt, outside(g)};
    };
    // The finest grid's spacing on the ground, for the smallest step.
    const auto finest = db_.manifest().finest_level().value_or(0);
    const double spacing_m = tile_size_deg(finest) / (db_.manifest().elevation_samples - 1) *
                             (wgs84::kA * std::numbers::pi / 180.0);
    const double min_step = std::max(spacing_m * 0.5, 0.01);
    constexpr double kNoTerrainStep = 50.0;
    // A straight ray that has been over the tiles and left them doesn't
    // come back (their extent is small and convex), so the march stops
    // there; and it
    // never takes more steps than this, whatever the distance asked.
    constexpr int kMaxSteps = 1'000'000;

    Sample here = sample(0.0);
    if (here.above && *here.above <= 0.0) {
        return TerrainHit{ray.origin, 0.0, kUnknownMaterial};  // starts on or below
    }
    bool was_inside = !here.outside_area;
    double t = 0.0;
    for (int steps = 0; steps < kMaxSteps && t < max_distance_m; ++steps) {
        const double step =
            here.above ? std::clamp(*here.above * 0.5, min_step, 1000.0) : kNoTerrainStep;
        const double next_t = std::min(t + step, max_distance_m);
        const Sample next = sample(next_t);
        if (next.above && *next.above <= 0.0) {
            // Crossed between t and next_t: bisect to a millimetre.
            double lo = t;
            double hi = next_t;
            while (hi - lo > 1e-3) {
                const double mid = 0.5 * (lo + hi);
                const auto m = sample(mid).above;
                if (m && *m <= 0.0) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            return TerrainHit{ray.origin + unit * hi, hi, kUnknownMaterial};
        }
        if (was_inside && next.outside_area) {
            break;  // left the area for good
        }
        was_inside = was_inside || !next.outside_area;
        t = next_t;
        here = next;
    }
    return std::nullopt;
}

}  // namespace fovea
