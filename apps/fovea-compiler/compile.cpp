#include "compile.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>

#include "datum.hpp"
#include "elevation.hpp"
#include "fovea/terrain/tile_file.hpp"
#include "imagery.hpp"
#include "runways.hpp"
#include "texture.hpp"

namespace fovea::compiler {

namespace fs = std::filesystem;

LevelTiles root_tiles(const GeoBounds& area, std::uint8_t level) {
    const TileKey sw = tile_at(area.south_deg, area.west_deg, level);
    TileKey ne = tile_at(area.north_deg, area.east_deg, level);
    // An area ending on a tile edge doesn't take the tile beyond it.
    const GeoBounds nb = tile_bounds(ne);
    if (nb.west_deg == area.east_deg && ne.x > sw.x) {
        --ne.x;
    }
    if (nb.south_deg == area.north_deg && ne.y > sw.y) {
        --ne.y;
    }
    return {level, sw.x, ne.x, sw.y, ne.y};
}

LevelTiles descendants(const LevelTiles& t, std::uint8_t levels) {
    const std::uint32_t f = std::uint32_t{1} << levels;
    return {static_cast<std::uint8_t>(t.level + levels), t.x0 * f, (t.x1 + 1) * f - 1, t.y0 * f,
            (t.y1 + 1) * f - 1};
}

namespace {

// Tiles of the level below the one being built, read as needed: a tile's
// four children and the edges of its neighbours' children.
class ChildLevel {
public:
    ChildLevel(fs::path dir, const LevelTiles& tiles) : dir_(std::move(dir)), tiles_(tiles) {}

    // nullptr outside the level's tiles; an error if one can't be read.
    const ElevationGrid* get(const TileKey& key, std::uint32_t samples, std::string& error) {
        if (!tiles_.contains(key)) {
            return nullptr;
        }
        auto& slot = held_[key];
        if (!slot) {
            const TileResult read = read_tile(dir_ / tile_path(key), key);
            const TileLayer* layer = read.tile ? read.tile->find(kElevationLayer) : nullptr;
            auto g = layer != nullptr ? decode_elevation(*layer) : std::nullopt;
            if (!g || g->size != samples) {
                error = "tile " + to_string(key) + ": can't read it back" +
                        (read.detail.empty() ? "" : ": " + read.detail);
                return nullptr;
            }
            slot = std::make_unique<ElevationGrid>(std::move(*g));
        }
        return slot.get();
    }
    // Keeps the memory bounded; workers take tiles along rows, so recent
    // ones are the likeliest to be asked for again.
    const LevelTiles& tiles() const { return tiles_; }
    void trim() {
        if (held_.size() > 48) {
            held_.clear();
        }
    }

private:
    fs::path dir_;
    LevelTiles tiles_;
    std::unordered_map<TileKey, std::unique_ptr<ElevationGrid>> held_;
};

// A tile from the level below: its grid points fall on every second child
// sample, each the [1 2 1] / 4 filter (both ways) of the child samples
// around it. Across a tile edge that reads the neighbour's children, so
// both tiles filter the same samples.
std::optional<ElevationGrid> filter_children(ChildLevel& below, const TileKey& key, std::uint32_t n,
                                             std::string& error) {
    const std::int64_t cells = n - 1;
    const auto level = static_cast<std::uint8_t>(key.level + 1);
    // A child-level sample by its index across the tile's 2 x 2 children
    // (0 to 2 * cells; one further on either side reaches the neighbours).
    const auto sample = [&](std::int64_t row, std::int64_t col) -> std::optional<double> {
        const auto split = [&](std::int64_t i, std::uint32_t first, std::uint32_t& tile,
                               std::uint32_t& local) {
            std::int64_t q = i >= 0 ? i / cells : -1;
            q = std::min<std::int64_t>(q, i == 2 * cells ? 1 : 2);
            tile = static_cast<std::uint32_t>(std::int64_t{first} + q);
            local = static_cast<std::uint32_t>(i - q * cells);
        };
        std::uint32_t tx = 0;
        std::uint32_t ty = 0;
        std::uint32_t c = 0;
        std::uint32_t r = 0;
        split(col, key.x * 2, tx, c);
        split(row, key.y * 2, ty, r);
        const ElevationGrid* g = below.get({level, tx, ty}, n, error);
        if (g == nullptr) {
            if (error.empty()) {
                error = "tile " + to_string(TileKey{level, tx, ty}) + ": missing";
            }
            return std::nullopt;
        }
        return static_cast<double>(g->at(r, c));
    };
    // Beyond the database's edge, the line through the two samples inside
    // carries on (a plane stays a plane), along whichever axis leaves it:
    // the tiles either side of an edge that meets the database's edge then
    // extrapolate the same samples.
    const LevelTiles& db = below.tiles();
    const auto outside = [&](std::int64_t i, std::uint32_t first, std::uint32_t lo,
                             std::uint32_t hi) {
        std::int64_t tile = first;
        if (i < 0) {
            --tile;
        } else if (i > 2 * cells) {
            tile += 2;
        }
        return tile < std::int64_t{lo} || tile > std::int64_t{hi};
    };
    const auto at = [&](std::int64_t row, std::int64_t col) -> std::optional<double> {
        const bool row_out = outside(row, key.y * 2, db.y0, db.y1);
        const bool col_out = outside(col, key.x * 2, db.x0, db.x1);
        const std::int64_t rc = row_out ? std::clamp<std::int64_t>(row, 0, 2 * cells) : row;
        const std::int64_t cc = col_out ? std::clamp<std::int64_t>(col, 0, 2 * cells) : col;
        const auto edge = sample(rc, cc);
        if (!row_out && !col_out) {
            return edge;
        }
        const auto inner = sample(2 * rc - row, 2 * cc - col);
        if (!edge || !inner) {
            return std::nullopt;
        }
        return 2.0 * *edge - *inner;
    };
    ElevationGrid grid;
    grid.size = n;
    grid.heights.resize(std::size_t{n} * n);
    static constexpr std::array<double, 3> kWeight = {0.25, 0.5, 0.25};
    for (std::int64_t r = 0; r < n; ++r) {
        for (std::int64_t c = 0; c < n; ++c) {
            double sum = 0.0;
            for (std::size_t i = 0; i < kWeight.size(); ++i) {
                for (std::size_t j = 0; j < kWeight.size(); ++j) {
                    const auto v = at(2 * r + static_cast<std::int64_t>(i) - 1,
                                      2 * c + static_cast<std::int64_t>(j) - 1);
                    if (!v) {
                        return std::nullopt;
                    }
                    sum += kWeight[i] * kWeight[j] * *v;
                }
            }
            grid.heights[static_cast<std::size_t>(r * n + c)] = static_cast<float>(sum);
        }
    }
    return grid;
}

// The largest height difference between a tile and the children it stands
// for, plus theirs: how far the tile can be from the finest data. Both are
// bilinear within each child cell, so the largest difference is at a
// child sample.
std::optional<float> error_from_children(ChildLevel& below, const TileKey& key,
                                         const ElevationGrid& grid, std::string& error) {
    const GeoBounds bounds = tile_bounds(key);
    double worst = 0.0;
    for (const TileKey& child : children(key)) {
        const ElevationGrid* c = below.get(child, grid.size, error);
        if (c == nullptr) {
            if (error.empty()) {
                error = "tile " + to_string(child) + ": missing";
            }
            return std::nullopt;
        }
        const GeoBounds cb = tile_bounds(child);
        const double step = (cb.east_deg - cb.west_deg) / (c->size - 1);
        double diff = 0.0;
        for (std::uint32_t r = 0; r < c->size; ++r) {
            for (std::uint32_t col = 0; col < c->size; ++col) {
                const double lat = cb.south_deg + r * step;
                const double lon = cb.west_deg + col * step;
                diff = std::max(diff, std::abs(static_cast<double>(c->at(r, col)) -
                                               grid.sample(bounds, lat, lon)));
            }
        }
        worst = std::max(worst, diff + static_cast<double>(c->error_m));
    }
    return static_cast<float>(worst);
}

// Runs a job per index on `jobs` threads, each thread's job made by
// `worker` (which can fail, setting `why`); stops at the first failure.
using Job = std::function<bool(std::size_t, std::string&)>;
bool run_parallel(std::size_t count, unsigned jobs, std::vector<std::string>& errors,
                  const std::function<Job(std::string&)>& worker) {
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex mutex;
    const auto run = [&] {
        std::string why;
        const Job job = worker(why);
        while (why.empty() && !failed) {
            const std::size_t i = next++;
            if (i >= count || !job(i, why)) {
                break;
            }
        }
        if (!why.empty()) {
            failed = true;
            // Once: every thread opening the same bad source says the same.
            const std::lock_guard lock(mutex);
            if (std::find(errors.begin(), errors.end(), why) == errors.end()) {
                errors.push_back(why);
            }
        }
    };
    std::vector<std::jthread> workers;
    for (unsigned j = 0; j < std::min<std::size_t>(jobs, count); ++j) {
        workers.emplace_back(run);
    }
    workers.clear();  // joins
    return !failed;
}

// The imagery pass: adds the layer to tiles the elevation pass wrote.
class ImageryBuild {
public:
    ImageryBuild(const CompileSettings& s, const DatumShift& datum, fs::path part)
        : s_(s), im_(*s.imagery), datum_(datum), part_(std::move(part)) {}

    // Encodes a tile's image with its mips into its file; returns its half,
    // for its parent.
    std::optional<Rgba8Image> finish(const TileKey& key, Rgba8Image image, std::string& why) {
        std::vector<Rgba8Image> mips = mip_chain(std::move(image));
        const fs::path file = part_ / tile_path(key);
        TileResult read = read_tile(file, key);
        if (!read.tile) {
            why = file.string() + ": can't read it back: " + read.detail;
            return std::nullopt;
        }
        read.tile->layers.push_back(encode_imagery(encode_texture(mips, im_.format)));
        bytes_ += read.tile->layers.back().data.size();
        if (!write_tile(file, *read.tile)) {
            why = file.string() + ": cannot write";
            return std::nullopt;
        }
        ++tiles_;
        return std::move(mips[1]);
    }

    // A tile and all below it, depth first, so only a few images per level
    // are held at once.
    std::optional<Rgba8Image> subtree(const ImagerySources& sources, const TileKey& key,
                                      std::string& why) {
        if (key.level == s_.finest_level) {
            Rgba8Image image;
            if (!sources.texture(key, im_.texels, datum_, image, why)) {
                return std::nullopt;
            }
            return finish(key, std::move(image), why);
        }
        std::array<Rgba8Image, 4> halves;
        const auto kids = children(key);
        for (std::size_t i = 0; i < kids.size(); ++i) {
            auto half = subtree(sources, kids[i], why);
            if (!half) {
                return std::nullopt;
            }
            halves[i] = std::move(*half);
        }
        return finish(key, from_children(halves[0], halves[1], halves[2], halves[3]), why);
    }

    std::size_t tiles() const { return tiles_; }
    std::size_t bytes() const { return bytes_; }

private:
    const CompileSettings& s_;
    const CompileSettings::Imagery& im_;
    const DatumShift& datum_;
    fs::path part_;
    std::atomic<std::size_t> tiles_{0};
    std::atomic<std::size_t> bytes_{0};
};

bool build_imagery(const CompileSettings& s, const DatumShift& datum, const fs::path& part,
                   const LevelTiles& roots, unsigned jobs, CompileResult& result) {
    const auto start = std::chrono::steady_clock::now();
    ImageryBuild build(s, datum, part);
    const auto level_tiles = [&](int lv) {
        return descendants(roots, static_cast<std::uint8_t>(lv - s.root_level));
    };
    // Subtrees, depth first, from the first level with a few per thread;
    // then the levels above from their halves, held until then (at most
    // about 16 per thread, or none: the roots' halves aren't kept).
    int top = s.root_level;
    while (top < s.finest_level && level_tiles(top).count() < std::size_t{4} * jobs) {
        ++top;
    }
    std::mutex mutex;
    std::unordered_map<TileKey, Rgba8Image> halves;
    const LevelTiles tops = level_tiles(top);
    const bool ok = run_parallel(tops.count(), jobs, result.errors, [&](std::string& why) -> Job {
        std::shared_ptr<ImagerySources> sources = ImagerySources::open(s.imagery->sources, why);
        return [&, sources](std::size_t i, std::string& err) {
            const TileKey key = tops.at(i);
            auto half = build.subtree(*sources, key, err);
            if (!half) {
                return false;
            }
            if (top > s.root_level) {
                const std::lock_guard lock(mutex);
                halves.emplace(key, std::move(*half));
            }
            return true;
        };
    });
    if (!ok) {
        return false;
    }
    for (int lv = top - 1; lv >= s.root_level; --lv) {
        const LevelTiles tiles = level_tiles(lv);
        std::unordered_map<TileKey, Rgba8Image> above;
        const bool level_ok =
            run_parallel(tiles.count(), jobs, result.errors, [&](std::string&) -> Job {
                return [&](std::size_t i, std::string& err) {
                    const TileKey key = tiles.at(i);
                    const auto kids = children(key);
                    // `halves` is only read until the level is done.
                    auto half = build.finish(key,
                                             from_children(halves.at(kids[0]), halves.at(kids[1]),
                                                           halves.at(kids[2]), halves.at(kids[3])),
                                             err);
                    if (!half) {
                        return false;
                    }
                    if (lv > s.root_level) {
                        const std::lock_guard lock(mutex);
                        above.emplace(key, std::move(*half));
                    }
                    return true;
                };
            });
        if (!level_ok) {
            return false;
        }
        halves = std::move(above);
    }
    result.imagery = ImageryReport{
        build.tiles(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
        build.bytes()};
    return true;
}

// The sources' heights at WGS-84 points, above WGS-84's ellipsoid: each
// point taken to NAD83(2011), the sources' frame (D-037), its height made
// ellipsoidal there (h = H + N, D-020) and moved to WGS-84's. One rule for
// the tiles and the runways' fit, so the blend meets the tiles.
bool ellipsoidal_at(const ElevationSources& sources, const Geoid& geoid, const DatumShift& datum,
                    std::span<const double> lat, std::span<const double> lon,
                    std::vector<float>& out, std::string& error) {
    std::vector<double> nad_lat(lat.size());
    std::vector<double> nad_lon(lat.size());
    std::vector<double> shift(lat.size());
    for (std::size_t i = 0; i < lat.size(); ++i) {
        const Nad83Point p = datum.to_nad83(lat[i], lon[i]);
        nad_lat[i] = p.lat_deg;
        nad_lon[i] = p.lon_deg;
        shift[i] = p.height_shift_m;
    }
    if (!sources.at_points(nad_lat, nad_lon, out, error)) {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(static_cast<double>(out[i]) + geoid.at(nad_lat[i], nad_lon[i]) +
                                    shift[i]);
    }
    return true;
}

}  // namespace

CompileResult compile(const CompileSettings& s, unsigned jobs,
                      const std::function<void(const LevelReport&)>& progress) {
    CompileResult result;
    // "db/khsv/" names the same directory as "db/khsv": the build goes
    // beside it, not inside it.
    fs::path output = s.output.lexically_normal();
    if (!output.has_filename()) {
        output = output.parent_path();
    }
    const fs::path part = fs::path(output).concat(".part");
    const fs::path old = fs::path(output).concat(".old");
    std::error_code ec;
    if (fs::exists(old)) {
        result.errors.push_back(old.string() +
                                ": left by an interrupted build; restore or remove it first");
        return result;
    }
    if (fs::exists(output) && !fs::exists(output / "manifest.toml")) {
        result.errors.push_back(output.string() +
                                ": exists and isn't a database; not replacing it");
        return result;
    }

    init_gdal();
    const LevelTiles roots = root_tiles(s.area, s.root_level);
    const GeoBounds sw = tile_bounds({roots.level, roots.x0, roots.y0});
    const GeoBounds ne = tile_bounds({roots.level, roots.x1, roots.y1});
    const GeoBounds extent{sw.south_deg, ne.north_deg, sw.west_deg, ne.east_deg};
    std::string error;
    const auto geoid = Geoid::load(s.geoid, extent, error);
    if (!geoid) {
        result.errors.push_back(error);
        return result;
    }
    const auto datum = DatumShift::make(extent, s.datum_epoch, error);
    if (!datum) {
        result.errors.push_back(error);
        return result;
    }
    fs::remove_all(part, ec);
    fs::create_directories(part, ec);
    if (ec) {
        result.errors.push_back(part.string() + ": " + ec.message());
        return result;
    }
    // A failed build leaves nothing behind.
    const auto fail = [&](std::string why) {
        result.errors.push_back(std::move(why));
        std::error_code ignored;
        fs::remove_all(part, ignored);
        return result;
    };
    if (jobs == 0) {
        jobs = std::max(1U, std::thread::hardware_concurrency());
    }

    // The runways (D-022), fitted once to the sources before the finest
    // level uses them.
    std::vector<RunwaySurface> runways;
    if (!s.runways.empty()) {
        const auto sources = ElevationSources::open(s.elevation, error);
        if (!sources) {
            return fail(error);
        }
        const PointHeights heights = [&](std::span<const double> lat, std::span<const double> lon,
                                         std::vector<float>& out, std::string& err) {
            return ellipsoidal_at(*sources, *geoid, *datum, lat, lon, out, err);
        };
        for (Runway r : s.runways) {
            // The FAA's ends are NAD83(2011).
            for (RunwayEnd& end : r.ends) {
                datum->to_wgs84(end.lat_deg, end.lon_deg);
            }
            auto fitted = RunwaySurface::fit(r, s.runway_blend_m, heights, error);
            if (!fitted) {
                return fail(error);
            }
            result.runways.push_back(
                {r.name, fitted->length_m(), fitted->rms_m(), fitted->max_abs_m()});
            runways.push_back(std::move(*fitted));
        }
        // Each is fitted to the sources alone, so where two meet (crossing,
        // or one's blend reaching the other) the second would leave a step
        // on the first: not yet supported.
        for (std::size_t i = 0; i < runways.size(); ++i) {
            for (std::size_t j = i + 1; j < runways.size(); ++j) {
                if (runways[i].touches(runways[j].reach())) {
                    return fail("runways " + runways[i].runway().name + " and " +
                                runways[j].runway().name +
                                " come within their blend of each other: crossing runways "
                                "aren't supported yet");
                }
            }
        }
    }

    DatabaseManifest& m = result.manifest;
    m.name = s.name;
    m.area = s.area;
    m.frame = DatabaseManifest::Frame{"WGS 84 (G2139)", s.datum_epoch};
    m.elevation_samples = s.samples;
    m.sources = s.credits;

    for (int lv = s.finest_level; lv >= s.root_level; --lv) {
        const auto start = std::chrono::steady_clock::now();
        const LevelTiles tiles = descendants(roots, static_cast<std::uint8_t>(lv - s.root_level));
        const bool finest = lv == s.finest_level;

        std::mutex mutex;
        LevelReport report{static_cast<std::uint8_t>(lv),
                           tiles.count(),
                           0.0,
                           0.0F,
                           std::numeric_limits<float>::max(),
                           std::numeric_limits<float>::lowest()};
        // Each thread's own: the sources (the finest level) or the level
        // below (the others).
        struct State {
            std::unique_ptr<ElevationSources> sources;
            ChildLevel below;
            std::vector<double> lat;
            std::vector<double> lon;
        };
        const bool ok =
            run_parallel(tiles.count(), jobs, result.errors, [&](std::string& why) -> Job {
                auto st = std::make_shared<State>(State{
                    finest ? ElevationSources::open(s.elevation, why) : nullptr,
                    ChildLevel(
                        part, descendants(roots, static_cast<std::uint8_t>(lv + 1 - s.root_level))),
                    {},
                    {}});
                return [&, st](std::size_t i, std::string& err) {
                    const TileKey key = tiles.at(i);
                    ElevationGrid grid;
                    if (finest) {
                        const GeoBounds b = tile_bounds(key);
                        const double step = (b.east_deg - b.west_deg) / (s.samples - 1);
                        st->lat.resize(std::size_t{s.samples} * s.samples);
                        st->lon.resize(st->lat.size());
                        for (std::uint32_t r = 0; r < s.samples; ++r) {
                            for (std::uint32_t c = 0; c < s.samples; ++c) {
                                const std::size_t at = std::size_t{r} * s.samples + c;
                                st->lat[at] = b.south_deg + r * step;
                                st->lon[at] = b.west_deg + c * step;
                            }
                        }
                        if (!ellipsoidal_at(*st->sources, *geoid, *datum, st->lat, st->lon,
                                            grid.heights, err)) {
                            err.insert(0, "tile " + to_string(key) + ": ");
                            return false;
                        }
                        grid.size = s.samples;
                        apply_runways(runways, key, s.samples, grid.heights);
                    } else {
                        auto filtered = filter_children(st->below, key, s.samples, err);
                        const auto e = filtered
                                           ? error_from_children(st->below, key, *filtered, err)
                                           : std::nullopt;
                        st->below.trim();
                        if (!e) {
                            return false;
                        }
                        grid = std::move(*filtered);
                        grid.error_m = *e;
                    }
                    const auto [lo, hi] =
                        std::minmax_element(grid.heights.begin(), grid.heights.end());
                    grid.min_m = *lo;
                    grid.max_m = *hi;
                    if (!write_tile(part / tile_path(key), {key, {encode_elevation(grid)}})) {
                        err = (part / tile_path(key)).string() + ": cannot write";
                        return false;
                    }
                    const std::lock_guard lock(mutex);
                    report.max_error_m = std::max(report.max_error_m, grid.error_m);
                    report.min_m = std::min(report.min_m, grid.min_m);
                    report.max_m = std::max(report.max_m, grid.max_m);
                    return true;
                };
            });
        if (!ok) {
            fs::remove_all(part, ec);
            return result;
        }
        report.seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        result.levels.push_back(report);
        m.levels.insert(m.levels.begin(), tiles);  // coarsest first
        if (progress) {
            progress(report);
        }
    }

    if (s.imagery) {
        if (!build_imagery(s, *datum, part, roots, jobs, result)) {
            fs::remove_all(part, ec);
            return result;
        }
        m.imagery = DatabaseManifest::Imagery{s.imagery->format, s.imagery->texels};
    }
    if (s.placements) {
        const std::uint8_t level = s.placements->level.value_or(s.root_level);
        PlacementReport report;
        // Read off the imagery, NAD83(2011).
        std::vector<PlacementSpec> placements = s.placements->placements;
        for (PlacementSpec& p : placements) {
            datum->to_wgs84(p.lat_deg, p.lon_deg);
        }
        if (!bake_placements(placements, s.placements->models, part, m, level, s.finest_level,
                             report, result.errors)) {
            fs::remove_all(part, ec);
            return result;
        }
        m.placements = DatabaseManifest::Placements{level};
        result.placements = report;
    }
    if (!write_manifest(part, m)) {
        return fail((part / "manifest.toml").string() + ": cannot write");
    }
    // The older database is moved aside, not deleted, until the new one is
    // in place: interrupted at any point, one of them is left whole.
    const bool replacing = fs::exists(output);
    if (replacing) {
        fs::rename(output, old, ec);
        if (ec) {
            return fail(output.string() +
                        ": cannot move the older database aside: " + ec.message());
        }
    }
    fs::rename(part, output, ec);
    if (ec) {
        std::error_code ignored;
        if (replacing) {
            fs::rename(old, output, ignored);
        }
        return fail(output.string() + ": cannot move the database into place: " + ec.message());
    }
    if (replacing) {
        fs::remove_all(old, ec);
    }
    result.ok = true;
    return result;
}

}  // namespace fovea::compiler
