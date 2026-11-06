#include "script.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string_view>
#include <utility>

#include <toml++/toml.hpp>

#include "fovea/core/ipv4.hpp"

namespace fovea::test_host {

namespace {

class Reader {
public:
    explicit Reader(std::string source) : source_(std::move(source)) {}

    void error(const toml::source_region& at, const std::string& message) {
        errors_.emplace_back(at.begin.line,
                             source_ + ":" + std::to_string(at.begin.line) + ": " + message);
    }

    // A whole number in [min, max] (a port); `fallback` when absent.
    std::optional<std::uint16_t> port(const toml::table& t, std::string_view key,
                                      const std::string& where, std::int64_t min,
                                      std::optional<std::uint16_t> fallback) {
        const toml::node* n = t.get(key);
        if (n == nullptr) {
            if (!fallback) {
                error(t.source(), "missing '" + std::string(key) + "' in " + where);
            }
            return fallback;
        }
        const auto* i = n->as_integer();
        if (i == nullptr || i->get() < min || i->get() > 65535) {
            error(n->source(), "'" + std::string(key) + "' in " + where +
                                   " must be a whole number from " + std::to_string(min) +
                                   " to 65535");
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(i->get());
    }

    const toml::table* table(const toml::table& root, std::string_view key, bool required) {
        const toml::node* n = root.get(key);
        if (n == nullptr) {
            if (required) {
                error(root.source(), "missing [" + std::string(key) + "]");
            }
            return nullptr;
        }
        if (const auto* t = n->as_table()) {
            return t;
        }
        error(n->source(), "'" + std::string(key) + "' must be a table");
        return nullptr;
    }

    void check_keys(const toml::table& t, std::initializer_list<std::string_view> allowed,
                    const std::string& where) {
        for (const auto& [key, node] : t) {
            bool known = false;
            for (const auto a : allowed) {
                known = known || key.str() == a;
            }
            if (!known) {
                error(node.source(), "unknown key '" + std::string(key.str()) + "' in " + where);
            }
        }
    }

    // A number in [min, max]; `fallback` when absent and not required.
    std::optional<double> number(const toml::table& t, std::string_view key,
                                 const std::string& where, double min, double max,
                                 std::optional<double> fallback) {
        const toml::node* n = t.get(key);
        if (n == nullptr) {
            if (!fallback) {
                error(t.source(), "missing '" + std::string(key) + "' in " + where);
            }
            return fallback;
        }
        std::optional<double> v;
        if (const auto* i = n->as_integer()) {
            v = static_cast<double>(i->get());
        } else if (const auto* f = n->as_floating_point()) {
            v = f->get();
        }
        if (!v || !std::isfinite(*v) || *v < min || *v > max) {
            std::ostringstream m;
            m << "'" << key << "' in " << where << " must be a number from " << min << " to "
              << max;
            error(n->source(), m.str());
            return std::nullopt;
        }
        return v;
    }

    std::optional<std::string> string(const toml::table& t, std::string_view key,
                                      const std::string& where) {
        const toml::node* n = t.get(key);
        if (n == nullptr) {
            error(t.source(), "missing '" + std::string(key) + "' in " + where);
            return std::nullopt;
        }
        if (const auto* s = n->as_string()) {
            return s->get();
        }
        error(n->source(), "'" + std::string(key) + "' in " + where + " must be a string");
        return std::nullopt;
    }

    // In file order (they're found in check order).
    std::vector<std::string> take_errors() {
        std::stable_sort(errors_.begin(), errors_.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::string> out;
        for (auto& e : errors_) {
            out.push_back(std::move(e.second));
        }
        return out;
    }

private:
    std::string source_;
    std::vector<std::pair<std::uint32_t, std::string>> errors_;
};

void read_segment(Reader& r, const toml::table& t, const std::string& where, Segment& out) {
    r.check_keys(t, {"kind", "duration", "speed", "climb", "turn_rate", "roll_rate"}, where);
    const auto kind = r.string(t, "kind", where);
    const std::initializer_list<std::pair<std::string_view, Segment::Kind>> kinds = {
        {"hold", Segment::Kind::hold},
        {"straight", Segment::Kind::straight},
        {"turn", Segment::Kind::turn},
        {"roll", Segment::Kind::roll},
        {"figure_eight", Segment::Kind::figure_eight}};
    if (kind) {
        bool found = false;
        for (const auto& [name, k] : kinds) {
            if (*kind == name) {
                out.kind = k;
                found = true;
            }
        }
        if (!found) {
            r.error(t.get("kind")->source(), "'kind' in " + where +
                                                 " must be hold, straight, turn, roll or "
                                                 "figure_eight (got \"" +
                                                 *kind + "\")");
        }
    }
    out.speed_mps = r.number(t, "speed", where, 0.0, 400.0, 0.0).value_or(0.0);
    out.climb_mps = r.number(t, "climb", where, -100.0, 100.0, 0.0).value_or(0.0);
    if (std::abs(out.climb_mps) > out.speed_mps && out.climb_mps != 0.0) {
        r.error(t.source(), where + ": 'climb' can't be faster than 'speed'");
    }
    out.turn_rate_dps = r.number(t, "turn_rate", where, -90.0, 90.0, 0.0).value_or(0.0);
    out.roll_rate_dps = r.number(t, "roll_rate", where, -720.0, 720.0, 0.0).value_or(0.0);
    if (out.kind == Segment::Kind::figure_eight) {
        if (out.turn_rate_dps == 0.0) {
            r.error(t.source(), where + ": a figure_eight needs a 'turn_rate'");
        } else {
            out.duration_s = 720.0 / std::abs(out.turn_rate_dps);
        }
        if (t.get("duration") != nullptr) {
            r.error(t.get("duration")->source(),
                    where + ": a figure_eight's duration is two full turns; leave it out");
        }
    } else {
        out.duration_s = r.number(t, "duration", where, 0.01, 86400.0, std::nullopt).value_or(1.0);
    }
}

}  // namespace

ScriptResult parse_script(const std::string& text, const std::string& source) {
    ScriptResult result;
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        result.errors.push_back(source + ":" + std::to_string(e.source().begin.line) + ": " +
                                std::string(e.description()));
        return result;
    }
    Reader r(source);
    Script s;
    r.check_keys(root, {"ig", "host", "start", "flight", "segment"}, "the script");
    if (const auto* ig = r.table(root, "ig", true)) {
        r.check_keys(*ig, {"address", "port"}, "[ig]");
        if (const auto a = r.string(*ig, "address", "[ig]")) {
            if (const auto ip = parse_ipv4(*a)) {
                s.ig_address = *ip;
            } else {
                r.error(ig->get("address")->source(),
                        "'address' in [ig] must be an IPv4 address (got \"" + *a + "\")");
            }
        }
        s.ig_port = r.port(*ig, "port", "[ig]", 1, std::nullopt).value_or(std::uint16_t{1});
    }
    if (const auto* host = r.table(root, "host", false)) {
        r.check_keys(*host, {"port", "rate_hz"}, "[host]");
        s.host_port =
            r.port(*host, "port", "[host]", 0, std::uint16_t{0}).value_or(std::uint16_t{0});
        s.rate_hz = r.number(*host, "rate_hz", "[host]", 1.0, 1000.0, 60.0).value_or(60.0);
    }
    if (const auto* start = r.table(root, "start", true)) {
        r.check_keys(*start, {"lat", "lon", "alt", "heading", "speed"}, "[start]");
        s.start.position.lat_deg =
            r.number(*start, "lat", "[start]", -89.0, 89.0, std::nullopt).value_or(0.0);
        s.start.position.lon_deg =
            r.number(*start, "lon", "[start]", -180.0, 180.0, std::nullopt).value_or(0.0);
        s.start.position.alt_m =
            r.number(*start, "alt", "[start]", -500.0, 20000.0, std::nullopt).value_or(0.0);
        s.start.attitude.yaw_deg =
            r.number(*start, "heading", "[start]", 0.0, 360.0, 0.0).value_or(0.0);
        s.start.speed_mps = r.number(*start, "speed", "[start]", 0.0, 400.0, 0.0).value_or(0.0);
    }
    if (const auto* flight = r.table(root, "flight", false)) {
        r.check_keys(*flight, {"loop"}, "[flight]");
        if (const auto* n = flight->get("loop")) {
            if (const auto* b = n->as_boolean()) {
                s.loop = b->get();
            } else {
                r.error(n->source(), "'loop' in [flight] must be true or false");
            }
        }
    }
    if (const auto* n = root.get("segment")) {
        const auto* list = n->as_array();
        if (list == nullptr || !list->is_array_of_tables() || list->empty()) {
            r.error(n->source(), "'segment' must be a list of tables: write each as [[segment]]");
        } else {
            for (std::size_t i = 0; i < list->size(); ++i) {
                Segment seg;
                read_segment(r, *list->get(i)->as_table(), "segment " + std::to_string(i + 1), seg);
                s.segments.push_back(seg);
            }
        }
    } else {
        r.error(root.source(), "missing [[segment]]: the flight needs at least one");
    }
    result.errors = r.take_errors();
    if (result.errors.empty()) {
        result.script = std::move(s);
    }
    return result;
}

ScriptResult load_script(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) {
        return {std::nullopt, {file.string() + ": cannot read the file"}};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse_script(text.str(), file.string());
}

}  // namespace fovea::test_host
