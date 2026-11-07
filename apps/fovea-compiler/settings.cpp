#include "settings.hpp"

#include <algorithm>
#include <bit>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>

#include <toml++/toml.hpp>

namespace fovea::compiler {

bool wildcard_match(std::string_view pattern, std::string_view name) {
    // Greedy with backtracking to the last '*': linear enough for file names.
    std::size_t p = 0;
    std::size_t n = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resume = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = n;
        } else if (p < pattern.size() && pattern[p] == name[n]) {
            ++p;
            ++n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

namespace {

namespace fs = std::filesystem;

std::optional<std::string> read_text(const fs::path& file) {
    std::ifstream in(file);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

SettingsResult parse_settings(const std::string& text, const std::string& source,
                              const fs::path& base) {
    SettingsResult r;
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
    CompileSettings s;
    s.name = root["name"].value_or(std::string{});
    if (s.name.empty()) {
        error(root.source(), "missing 'name'");
    }
    const auto path = [&](toml::node_view<toml::node> n, const char* what) -> fs::path {
        const auto v = n.value<std::string>();
        if (!v || v->empty()) {
            error(root.source(), std::string("missing '") + what + "'");
            return {};
        }
        return base / *v;
    };
    s.output = path(root["output"], "output");

    // The area: a table here, or the [area] of another file (the sources).
    toml::table area_file;
    const toml::table* area = root["area"].as_table();
    if (const auto file = root["area"].value<std::string>()) {
        const fs::path f = base / *file;
        const auto t = read_text(f);
        try {
            area_file = t ? toml::parse(*t, f.string()) : toml::table{};
            area = area_file["area"].as_table();
        } catch (const toml::parse_error& e) {
            r.errors.push_back(f.string() + ": " + std::string(e.description()));
        }
        if (!t) {
            error(root.get("area")->source(), "cannot read " + f.string());
        }
    }
    if (area == nullptr) {
        error(root.source(), "'area' needs south, north, west and east (a table or a file)");
    } else {
        const auto deg = [&](const char* key) { return (*area)[key].value<double>(); };
        const auto south = deg("south");
        const auto north = deg("north");
        const auto west = deg("west");
        const auto east = deg("east");
        if (!south || !north || !west || !east || !(*south < *north) || !(*west < *east) ||
            *south < -90.0 || *north > 90.0 || *west < -180.0 || *east > 180.0) {
            error(area->source(), "the area needs south < north and west < east, in degrees");
        } else {
            s.area = {*south, *north, *west, *east};
        }
    }

    // Checked as 64-bit values before narrowing.
    const auto root_level = root["levels"]["root"].value<std::int64_t>();
    const auto finest_level = root["levels"]["finest"].value<std::int64_t>();
    if (!root_level || !finest_level || *root_level < 0 || *finest_level > kMaxTileLevel ||
        *root_level > *finest_level || *finest_level - *root_level > 8) {
        error(root.source(), "[levels] needs root <= finest, up to 30, at most 8 levels apart");
    } else {
        s.root_level = static_cast<std::uint8_t>(*root_level);
        s.finest_level = static_cast<std::uint8_t>(*finest_level);
    }

    const auto elevation = root["elevation"];
    const auto samples = elevation["samples"].value<std::int64_t>();
    // Odd: a coarser tile's samples fall on every second sample of its
    // children.
    // At most 1025: a worker holds up to 16 grids at once (64 MB at 1025).
    if (!samples || *samples < 3 || *samples > 1025 || *samples % 2 == 0) {
        error(root.source(), "[elevation] needs odd samples from 3 to 1025");
    } else {
        s.samples = static_cast<std::uint32_t>(*samples);
    }
    const fs::path cache = path(elevation["cache"], "elevation.cache");
    s.geoid = cache / elevation["geoid"].value_or(std::string{});
    if (elevation["geoid"].value_or(std::string{}).empty()) {
        error(root.source(), "missing 'elevation.geoid'");
    }

    // Each pattern names at least one file; files in the same directories
    // that none names are listed, as they may be new data left out.
    std::set<fs::path> named;  // by any layer
    std::set<fs::path> dirs;
    // A group of files per pattern (files an earlier pattern of the same
    // layer took aside; layers don't take from each other).
    const auto resolve = [&](const toml::array* patterns, const fs::path& from,
                             const char* section) {
        std::set<fs::path> taken;
        std::vector<std::vector<fs::path>> groups;
        if (patterns == nullptr || patterns->empty()) {
            error(root.source(),
                  std::string("[") + section + "] needs sources = [\"dir/pattern\", ...]");
            return groups;
        }
        for (const auto& node : *patterns) {
            const auto p = node.value<std::string>();
            const fs::path rel = p.value_or(std::string{});
            if (!p || !rel.has_filename()) {
                error(node.source(), "a source is a \"directory/file pattern\" string");
                continue;
            }
            // Normal paths, so two spellings of a directory or file are one.
            const fs::path dir = (from / rel.parent_path()).lexically_normal();
            std::vector<fs::path> found;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (entry.is_regular_file() &&
                    wildcard_match(rel.filename().string(), entry.path().filename().string())) {
                    found.push_back(entry.path().lexically_normal());
                }
            }
            if (ec) {
                error(node.source(), "cannot list " + dir.string() + ": " + ec.message());
                continue;
            }
            if (found.empty()) {
                error(node.source(),
                      "no file in " + dir.string() + " matches '" + rel.filename().string() + "'");
                continue;
            }
            dirs.insert(dir);
            std::sort(found.begin(), found.end());
            auto& group = groups.emplace_back();
            for (auto& f : found) {
                named.insert(f);
                if (taken.insert(f).second) {
                    group.push_back(std::move(f));
                }
            }
        }
        return groups;
    };
    for (auto& group : resolve(elevation["sources"].as_array(), cache, "elevation")) {
        for (auto& f : group) {
            s.elevation.push_back(std::move(f));
        }
    }

    if (const auto imagery = root["imagery"]; imagery.is_table()) {
        CompileSettings::Imagery im;
        const auto format = imagery_format_from_name(imagery["format"].value_or(std::string{}));
        if (!format) {
            error(imagery.node()->source(), R"([imagery] needs format = "bc7", "bc1" or "rgba8")");
        } else {
            im.format = *format;
        }
        const auto texels = imagery["texels"].value<std::int64_t>();
        if (!texels || *texels < 4 || *texels > 4096 ||
            !std::has_single_bit(static_cast<std::uint64_t>(*texels))) {
            error(imagery.node()->source(), "[imagery] needs texels a power of two from 4 to 4096");
        } else {
            im.texels = static_cast<std::uint32_t>(*texels);
        }
        const fs::path from = path(imagery["cache"], "imagery.cache");
        im.sources = resolve(imagery["sources"].as_array(), from, "imagery");
        s.imagery = std::move(im);
    }

    if (const auto runways = root["runways"]; runways.is_table()) {
        const auto blend = runways["blend_m"].value<double>();
        if (blend && !(*blend >= 0.0 && *blend <= 1000.0)) {
            error(runways.node()->source(), "[runways] blend_m is from 0 to 1000");
        }
        s.runway_blend_m = blend.value_or(s.runway_blend_m);
        const fs::path f = path(runways["airport"], "runways.airport");
        const auto t = f.empty() ? std::nullopt : read_text(f);
        if (!f.empty() && !t) {
            error(runways.node()->source(), "cannot read " + f.string());
        }
        toml::table airport;
        bool parsed = false;
        if (t) {
            try {
                airport = toml::parse(*t, f.string());
                parsed = true;
            } catch (const toml::parse_error& e) {
                r.errors.push_back(f.string() + ":" + std::to_string(e.source().begin.line) + ": " +
                                   std::string(e.description()));
            }
        }
        const auto bad = [&](const toml::node& at, const std::string& msg) {
            r.errors.push_back(f.string() + ":" + std::to_string(at.source().begin.line) + ": " +
                               msg);
        };
        const auto* list = airport["runway"].as_array();
        if (parsed && (list == nullptr || list->empty())) {
            r.errors.push_back(f.string() + ": no [[runway]]");
        }
        if (list != nullptr) {
            for (const auto& node : *list) {
                const auto* rt = node.as_table();
                const auto* ends = rt != nullptr ? (*rt)["end"].as_array() : nullptr;
                const auto width = rt != nullptr ? (*rt)["width_ft"].value<double>() : std::nullopt;
                if (rt == nullptr || !(*rt)["name"].is_string() || !width || !(*width > 0.0) ||
                    ends == nullptr || ends->size() != 2) {
                    bad(node, "a [[runway]] needs name, width_ft and two [[runway.end]]");
                    continue;
                }
                Runway rw;
                rw.name = (*rt)["name"].value_or(std::string{});
                rw.width_m = *width * 0.3048;
                rw.length_m = (*rt)["length_ft"].value_or(0.0) * 0.3048;
                bool ok = true;
                for (std::size_t i = 0; i < 2; ++i) {
                    const auto* et = (*ends)[i].as_table();
                    const auto lat = et != nullptr ? parse_degrees_minutes(
                                                         (*et)["lat"].value_or(std::string{}), true)
                                                   : std::nullopt;
                    const auto lon =
                        et != nullptr
                            ? parse_degrees_minutes((*et)["lon"].value_or(std::string{}), false)
                            : std::nullopt;
                    const auto elev =
                        et != nullptr ? (*et)["elevation_ft"].value<double>() : std::nullopt;
                    if (et == nullptr || !(*et)["id"].is_string() || !lat || !lon || !elev) {
                        bad((*ends)[i],
                            "a [[runway.end]] needs id, lat and lon (\"34-39.167130N\", "
                            "\"086-46.926028W\") and elevation_ft");
                        ok = false;
                        continue;
                    }
                    rw.ends[i] = {(*et)["id"].value_or(std::string{}), *lat, *lon, *elev * 0.3048};
                }
                if (ok) {
                    s.runways.push_back(std::move(rw));
                }
            }
        }
    }

    for (const auto& dir : dirs) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_regular_file() && !named.contains(entry.path().lexically_normal())) {
                r.warnings.push_back(entry.path().string() + ": not in any source");
            }
        }
    }
    std::sort(r.warnings.begin(), r.warnings.end());

    if (const auto* list = root["credit"].as_array()) {
        for (const auto& node : *list) {
            const auto* t = node.as_table();
            if (t == nullptr || !(*t)["layer"].is_string() || !(*t)["name"].is_string() ||
                !(*t)["credit"].is_string()) {
                error(node.source(), "a [[credit]] needs layer, name and credit");
                continue;
            }
            s.credits.push_back({(*t)["layer"].value_or(std::string{}),
                                 (*t)["name"].value_or(std::string{}),
                                 (*t)["credit"].value_or(std::string{})});
        }
    }

    if (r.errors.empty()) {
        r.settings = std::move(s);
    }
    return r;
}

SettingsResult load_settings(const fs::path& file) {
    const auto text = read_text(file);
    if (!text) {
        SettingsResult r;
        r.errors.push_back(file.string() + ": cannot read the settings");
        return r;
    }
    return parse_settings(*text, file.string(), file.parent_path());
}

}  // namespace fovea::compiler
