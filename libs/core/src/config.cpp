#include "fovea/core/config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <span>
#include <sstream>
#include <system_error>
#include <toml++/toml.hpp>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "fovea/core/ipv4.hpp"

namespace fovea {

namespace {

// Largest render extent accepted; Vulkan guarantees 4096 and desktop GPUs
// support 16384 (maxImageDimension2D).
constexpr std::int64_t kMaxExtent = 16384;
// Only the sign is checked here; whether the monitor exists is checked when
// the windows are created.
constexpr std::int64_t kMaxMonitor = std::numeric_limits<int>::max();

// Shortest text that reads back as the same double, independent of locale,
// so a message never shows two different values as equal.
std::string format_number(double v) {
    std::array<char, 32> buf{};
    const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    return ec == std::errc{} ? std::string(buf.data(), end) : "?";
}

// UTF-8 on every platform; path::string() uses the ANSI code page on Windows
// and throws for characters it cannot represent.
std::string path_text(const std::filesystem::path& path) {
    const std::u8string u = path.u8string();
    return {reinterpret_cast<const char*>(u.data()), u.size()};
}

// Collects every problem with its location; the getters report a problem and
// return nothing, so parsing carries on to find the rest.
class Parser {
public:
    explicit Parser(std::string_view source) : source_(source) {}

    // In file order, which is how people fix them.
    std::vector<ConfigError> take_errors() {
        std::stable_sort(errors_.begin(), errors_.end(), [](const auto& a, const auto& b) {
            return a.line != b.line ? a.line < b.line : a.column < b.column;
        });
        return std::move(errors_);
    }
    bool failed() const { return !errors_.empty(); }

    void error(const toml::source_region& where, std::string message) {
        errors_.push_back({source_, where.begin.line, where.begin.column, std::move(message)});
    }

    void error(std::string message) { errors_.push_back({source_, 0, 0, std::move(message)}); }

    void check_keys(const toml::table& table, std::initializer_list<std::string_view> allowed,
                    std::string_view path) {
        check_keys(table, std::span(allowed.begin(), allowed.size()), path);
    }

    void check_keys(const toml::table& table, std::span<const std::string_view> allowed,
                    std::string_view path) {
        for (const auto& [key, node] : table) {
            bool known = false;
            for (std::string_view a : allowed) {
                known = known || key.str() == a;
            }
            if (!known) {
                std::string list;
                for (std::string_view a : allowed) {
                    list += list.empty() ? "" : ", ";
                    list += a;
                }
                error(key.source(), "unknown key '" + join(path, key.str()) +
                                        "' (expected one of: " + list + ")");
            }
        }
    }

    // A key that must be present; reported at the table that lacks it.
    const toml::node* required(const toml::table& table, std::string_view key,
                               std::string_view path) {
        const toml::node* node = table.get(key);
        if (node == nullptr) {
            error(table.source(), "missing '" + join(path, key) + "'");
        }
        return node;
    }

    std::optional<double> number(const toml::node& node, std::string_view path) {
        std::optional<double> v;
        if (const auto* i = node.as_integer()) {
            v = static_cast<double>(i->get());
        } else if (const auto* f = node.as_floating_point()) {
            v = f->get();
        }
        if (!v || !std::isfinite(*v)) {
            error(node.source(), "'" + std::string(path) + "' must be a finite number");
            return std::nullopt;
        }
        return v;
    }

    std::optional<std::int64_t> integer(const toml::node& node, std::string_view path,
                                        std::int64_t min, std::int64_t max) {
        const auto* i = node.as_integer();
        if (i == nullptr) {
            error(node.source(), "'" + std::string(path) + "' must be an integer");
            return std::nullopt;
        }
        if (i->get() < min || i->get() > max) {
            error(node.source(), "'" + std::string(path) + "' must be between " +
                                     std::to_string(min) + " and " + std::to_string(max) +
                                     " (got " + std::to_string(i->get()) + ")");
            return std::nullopt;
        }
        return i->get();
    }

    std::optional<std::string> string(const toml::node& node, std::string_view path) {
        const auto* s = node.as_string();
        if (s == nullptr) {
            error(node.source(), "'" + std::string(path) + "' must be a string");
            return std::nullopt;
        }
        return s->get();
    }

    const toml::table* table(const toml::node& node, std::string_view path) {
        const auto* t = node.as_table();
        if (t == nullptr) {
            error(node.source(), "'" + std::string(path) + "' must be a table");
        }
        return t;
    }

    template <typename Enum>
    std::optional<Enum> choice(const toml::node& node, std::string_view path,
                               std::initializer_list<std::pair<std::string_view, Enum>> options) {
        const auto s = string(node, path);
        if (!s) {
            return std::nullopt;
        }
        std::string list;
        for (const auto& [text, value] : options) {
            if (*s == text) {
                return value;
            }
            list += list.empty() ? "" : ", ";
            list += text;
        }
        error(node.source(),
              "'" + std::string(path) + "' is \"" + *s + "\"; expected one of: " + list);
        return std::nullopt;
    }

    static std::string join(std::string_view path, std::string_view key) {
        return path.empty() ? std::string(key) : std::string(path) + "." + std::string(key);
    }

private:
    std::string source_;
    std::vector<ConfigError> errors_;
};

void parse_display(Parser& p, const toml::table& t, DisplayConfig& out) {
    p.check_keys(t, {"presenter", "target_hz", "near_m", "msaa"}, "display");
    if (const auto* n = t.get("presenter")) {
        if (auto v = p.choice<PresenterKind>(*n, "display.presenter",
                                             {{"multi_swapchain", PresenterKind::multi_swapchain},
                                              {"spanned", PresenterKind::spanned},
                                              {"offscreen", PresenterKind::offscreen}})) {
            out.presenter = *v;
        }
    }
    if (const auto* n = t.get("target_hz")) {
        if (auto v = p.number(*n, "display.target_hz")) {
            if (*v < 1.0 || *v > 1000.0) {
                p.error(n->source(), "'display.target_hz' must be between 1 and 1000 (got " +
                                         format_number(*v) + ")");
            }
            out.target_hz = *v;
        }
    }
    if (const auto* n = t.get("near_m")) {
        if (auto v = p.number(*n, "display.near_m")) {
            if (*v <= 0.0) {
                p.error(n->source(),
                        "'display.near_m' must be greater than 0 (got " + format_number(*v) + ")");
            }
            out.near_m = *v;
        }
    }
    if (const auto* n = t.get("msaa")) {
        if (auto v = p.integer(*n, "display.msaa", 1, 8)) {
            if (*v != 1 && *v != 2 && *v != 4 && *v != 8) {
                p.error(n->source(),
                        "'display.msaa' must be 1, 2, 4 or 8 (got " + std::to_string(*v) + ")");
            }
            out.msaa = static_cast<std::uint32_t>(*v);
        }
    }
}

// A number in [min, max] (or (min, max] with open_min), reported if not.
void check_range(Parser& p, const toml::node& n, const std::string& path, double v, double min,
                 double max, bool open_min) {
    const bool low = open_min ? v <= min : v < min;
    if (low || v > max) {
        p.error(n.source(), "'" + path + "' must be " + (open_min ? "greater than " : "at least ") +
                                format_number(min) + " and at most " + format_number(max) +
                                " (got " + format_number(v) + ")");
    }
}

void parse_host(Parser& p, const toml::table& t, HostConfig& out) {
    p.check_keys(
        t, {"protocol", "port", "listen_address", "host_address", "host_port", "geoid_height_m"},
        "host");
    if (const auto* n = t.get("listen_address")) {
        if (auto v = p.string(*n, "host.listen_address")) {
            out.listen_address = parse_ipv4(*v);
            if (!out.listen_address) {
                p.error(n->source(),
                        "'host.listen_address' must be an IPv4 address like 127.0.0.1 (got \"" +
                            *v + "\")");
            }
        }
    }
    if (const auto* n = p.required(t, "protocol", "host")) {
        if (auto v = p.choice<HostProtocol>(*n, "host.protocol", {{"cigi", HostProtocol::cigi}})) {
            out.protocol = *v;
        }
    }
    if (const auto* n = p.required(t, "port", "host")) {
        if (auto v = p.integer(*n, "host.port", 1, 65535)) {
            out.port = static_cast<std::uint16_t>(*v);
        }
    }
    const auto* address = t.get("host_address");
    const auto* port = t.get("host_port");
    if (address != nullptr) {
        if (auto v = p.string(*address, "host.host_address")) {
            out.host_address = parse_ipv4(*v);
            if (!out.host_address) {
                p.error(address->source(),
                        "'host.host_address' must be an IPv4 address like 192.168.1.20 (got \"" +
                            *v + "\")");
            }
        }
    }
    if (port != nullptr) {
        if (auto v = p.integer(*port, "host.host_port", 1, 65535)) {
            out.host_port = static_cast<std::uint16_t>(*v);
        }
    }
    // A socket bound to this computer's loopback can only reach this
    // computer: Start of Frame to another machine would never leave.
    const auto loopback = [](std::uint32_t ip) { return (ip >> 24) == 127U; };
    if (out.listen_address && loopback(*out.listen_address) && out.host_address &&
        !loopback(*out.host_address)) {
        p.error(address->source(),
                "'host.host_address' is another computer, but 'host.listen_address' is this "
                "computer only (127.x.x.x): Start of Frame couldn't reach the host");
    }
    if ((address == nullptr) != (port == nullptr)) {
        p.error((address != nullptr ? address : port)->source(),
                "'host.host_address' and 'host.host_port' go together: set both, or neither");
    }
    if (const auto* n = t.get("geoid_height_m")) {
        if (auto v = p.number(*n, "host.geoid_height_m")) {
            check_range(p, *n, "host.geoid_height_m", *v, -150.0, 150.0, false);
            out.geoid_height_m = *v;
        }
    }
}

// Reads the listed numeric fields of an inline table, all required.
template <std::size_t N>
std::optional<std::array<double, N>> number_fields(Parser& p, const toml::node& node,
                                                   const std::string& path,
                                                   const std::array<std::string_view, N>& keys) {
    const toml::table* t = p.table(node, path);
    if (t == nullptr) {
        return std::nullopt;
    }
    p.check_keys(*t, std::span<const std::string_view>(keys), path);
    std::array<double, N> values{};
    bool ok = true;
    for (std::size_t i = 0; i < N; ++i) {
        const toml::node* field = p.required(*t, keys[i], path);
        const auto v =
            field != nullptr ? p.number(*field, Parser::join(path, keys[i])) : std::nullopt;
        ok = ok && v.has_value();
        values[i] = v.value_or(0.0);
    }
    return ok ? std::optional(values) : std::nullopt;
}

void parse_reference_scene(Parser& p, const toml::table& t, ReferenceScene& out) {
    p.check_keys(t, {"eye_height_m", "grid_spacing_m", "exposure_ev", "origin"}, "reference_scene");
    if (const auto* n = t.get("origin")) {
        if (auto v = number_fields<3>(p, *n, "reference_scene.origin", {"lat", "lon", "alt"})) {
            check_range(p, *n, "reference_scene.origin.lat", (*v)[0], -90.0, 90.0, false);
            check_range(p, *n, "reference_scene.origin.lon", (*v)[1], -180.0, 180.0, false);
            check_range(p, *n, "reference_scene.origin.alt", (*v)[2], -1000.0, 10000.0, false);
            out.origin = {(*v)[0], (*v)[1], (*v)[2]};
        }
    }
    if (const auto* n = t.get("eye_height_m")) {
        if (auto v = p.number(*n, "reference_scene.eye_height_m")) {
            check_range(p, *n, "reference_scene.eye_height_m", *v, 0.0, 100000.0, true);
            out.eye_height_m = *v;
        }
    }
    if (const auto* n = t.get("grid_spacing_m")) {
        if (auto v = p.number(*n, "reference_scene.grid_spacing_m")) {
            check_range(p, *n, "reference_scene.grid_spacing_m", *v, 0.0, 100000.0, true);
            out.grid_spacing_m = *v;
        }
    }
    if (const auto* n = t.get("exposure_ev")) {
        if (auto v = p.number(*n, "reference_scene.exposure_ev")) {
            check_range(p, *n, "reference_scene.exposure_ev", *v, -20.0, 20.0, false);
            out.exposure_ev = *v;
        }
    }
}

void parse_environment(Parser& p, const toml::table& t, EnvironmentConfig& out) {
    p.check_keys(t, {"date", "time", "time_runs", "visibility_m"}, "environment");
    if (const auto* n = t.get("date")) {
        if (const auto* d = n->as_date()) {
            const toml::date v = d->get();
            if (v.year < 1950 || v.year > 2050) {
                p.error(n->source(),
                        "'environment.date' must be from 1950 to 2050, the years the "
                        "sun and moon are computed for (got " +
                            std::to_string(v.year) + ")");
            }
            out.year = v.year;
            out.month = v.month;
            out.day = v.day;
        } else {
            p.error(n->source(), "'environment.date' must be a date, like 2026-03-20");
        }
    }
    if (const auto* n = t.get("time")) {
        if (const auto* d = n->as_time()) {
            const toml::time v = d->get();
            out.utc_seconds = v.hour * 3600.0 + v.minute * 60.0 + v.second + v.nanosecond / 1e9;
        } else {
            p.error(n->source(), "'environment.time' must be a time of day, UTC, like 15:30:00");
        }
    }
    if (const auto* n = t.get("time_runs")) {
        if (const auto* b = n->as_boolean()) {
            out.time_runs = b->get();
        } else {
            p.error(n->source(), "'environment.time_runs' must be true or false");
        }
    }
    if (const auto* n = t.get("visibility_m")) {
        if (auto v = p.number(*n, "environment.visibility_m")) {
            check_range(p, *n, "environment.visibility_m", *v, 0.0, 1e7, true);
            out.visibility_m = *v;
        }
    }
}

void parse_terrain(Parser& p, const toml::table& t, TerrainConfig& out) {
    p.check_keys(t, {"database", "max_level", "pixel_error", "max_tiles", "budget_mb"}, "terrain");
    if (const auto* n = t.get("max_tiles")) {
        if (auto v = p.integer(*n, "terrain.max_tiles", 16, 1 << 20)) {
            out.max_tiles = static_cast<std::uint32_t>(*v);
        }
    }
    if (const auto* n = t.get("budget_mb")) {
        if (auto v = p.integer(*n, "terrain.budget_mb", 16, 1 << 20)) {
            out.budget_mb = static_cast<std::uint32_t>(*v);
        }
    }
    if (const auto* n = p.required(t, "database", "terrain")) {
        if (auto v = p.string(*n, "terrain.database")) {
            if (v->empty()) {
                p.error(n->source(), "'terrain.database' must name a directory");
            }
            // UTF-8, as TOML is: a char8_t path, so Windows reads it as UTF-8
            // and not the ANSI code page.
            std::u8string utf8;
            utf8.reserve(v->size());
            for (const char c : *v) {
                utf8.push_back(static_cast<char8_t>(c));
            }
            out.database = std::filesystem::path(utf8);
        }
    }
    if (const auto* n = t.get("max_level")) {
        if (auto v = p.integer(*n, "terrain.max_level", 0, 30)) {
            out.max_level = static_cast<int>(*v);
        }
    }
    if (const auto* n = t.get("pixel_error")) {
        if (auto v = p.number(*n, "terrain.pixel_error")) {
            check_range(p, *n, "terrain.pixel_error", *v, 0.0, 1000.0, true);
            out.pixel_error = *v;
        }
    }
}

void check_frustum(Parser& p, const toml::node& node, const std::string& path, const Frustum& f) {
    const auto in_range = [](double deg) { return deg > -90.0 && deg < 90.0; };
    for (const auto& [name, deg] :
         {std::pair{"left", f.left_deg}, std::pair{"right", f.right_deg},
          std::pair{"top", f.top_deg}, std::pair{"bottom", f.bottom_deg}}) {
        if (!in_range(deg)) {
            p.error(node.source(), "'" + path + "." + name +
                                       "' must be strictly between -90 and 90 degrees (got " +
                                       format_number(deg) + ")");
        }
    }
    if (f.left_deg >= f.right_deg) {
        p.error(node.source(), "'" + path + "': left (" + format_number(f.left_deg) +
                                   ") must be less than right (" + format_number(f.right_deg) +
                                   ")");
    }
    if (f.bottom_deg >= f.top_deg) {
        p.error(node.source(), "'" + path + "': bottom (" + format_number(f.bottom_deg) +
                                   ") must be less than top (" + format_number(f.top_deg) + ")");
    }
}

// The ranges in attitude.hpp, with yaw allowed either way round.
void check_view_rotation(Parser& p, const toml::node& node, const std::string& path,
                         const Euler& e) {
    for (const auto& [name, deg, limit] :
         {std::tuple{"yaw", e.yaw_deg, 360.0}, std::tuple{"pitch", e.pitch_deg, 90.0},
          std::tuple{"roll", e.roll_deg, 180.0}}) {
        if (deg < -limit || deg > limit) {
            p.error(node.source(), "'" + path + "." + name + "' must be between " +
                                       format_number(-limit) + " and " + format_number(limit) +
                                       " degrees (got " + format_number(deg) + ")");
        }
    }
}

void parse_channel(Parser& p, const toml::table& t, const std::string& path, ChannelConfig& out) {
    p.check_keys(t,
                 {"name", "monitor", "entity", "present_mode", "render_extent", "eye_offset",
                  "view_rotation", "frustum"},
                 path);

    if (const auto* n = p.required(t, "name", path)) {
        if (auto v = p.string(*n, path + ".name")) {
            // Also a file name (headless dumps write <name>.png), so plain
            // characters only, on every platform.
            const bool safe =
                std::all_of(v->begin(), v->end(),
                            [](char c) {
                                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
                            }) &&
                v->front() != '.';
            if (v->empty()) {
                p.error(n->source(), "'" + path + ".name' must not be empty");
            } else if (!safe || v->size() > 64) {
                p.error(n->source(), "'" + path + ".name' \"" + *v +
                                         "\" must be up to 64 letters, digits, '-', '_' or '.', "
                                         "not starting with '.'");
            }
            out.name = *v;
        }
    }
    if (const auto* n = p.required(t, "monitor", path)) {
        if (auto v = p.integer(*n, path + ".monitor", 0, kMaxMonitor)) {
            out.monitor = static_cast<int>(*v);
        }
    }
    if (const auto* n = t.get("entity")) {
        if (auto v =
                p.integer(*n, path + ".entity", 0, std::numeric_limits<std::uint32_t>::max())) {
            out.entity = static_cast<std::uint32_t>(*v);
        }
    }
    if (const auto* n = t.get("present_mode")) {
        if (auto v = p.choice<PresentMode>(*n, path + ".present_mode",
                                           {{"fifo", PresentMode::fifo},
                                            {"mailbox", PresentMode::mailbox},
                                            {"immediate", PresentMode::immediate}})) {
            out.present_mode = *v;
        }
    }
    if (const auto* n = p.required(t, "render_extent", path)) {
        const auto* a = n->as_array();
        if (a == nullptr || a->size() != 2) {
            p.error(n->source(), "'" + path + ".render_extent' must be [width, height]");
        } else {
            const auto w = p.integer(*a->get(0), path + ".render_extent[0]", 1, kMaxExtent);
            const auto h = p.integer(*a->get(1), path + ".render_extent[1]", 1, kMaxExtent);
            out.render_width = static_cast<std::uint32_t>(w.value_or(0));
            out.render_height = static_cast<std::uint32_t>(h.value_or(0));
        }
    }
    if (const auto* n = t.get("eye_offset")) {
        if (auto v = number_fields<3>(p, *n, path + ".eye_offset", {"x", "y", "z"})) {
            out.eye_offset_m = {(*v)[0], (*v)[1], (*v)[2]};
        }
    }
    if (const auto* n = t.get("view_rotation")) {
        if (auto v = number_fields<3>(p, *n, path + ".view_rotation", {"yaw", "pitch", "roll"})) {
            out.view_rotation = {(*v)[0], (*v)[1], (*v)[2]};
            check_view_rotation(p, *n, path + ".view_rotation", out.view_rotation);
        }
    }
    if (const auto* n = p.required(t, "frustum", path)) {
        if (auto v =
                number_fields<4>(p, *n, path + ".frustum", {"left", "right", "top", "bottom"})) {
            out.frustum = {(*v)[0], (*v)[1], (*v)[2], (*v)[3]};
            check_frustum(p, *n, path + ".frustum", out.frustum);
        }
    }
}

}  // namespace

std::string ConfigError::to_string() const {
    return source + ":" + std::to_string(line) + ":" + std::to_string(column) + ": " + message;
}

ConfigResult parse_display_config(std::string_view text, std::string_view source) {
    Parser p(source);
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        p.error(e.source(), std::string(e.description()));
        return {std::nullopt, p.take_errors()};
    }

    DisplayConfig config;
    p.check_keys(root, {"display", "channel", "reference_scene", "environment", "host", "terrain"},
                 "");
    if (const auto* n = root.get("environment")) {
        if (const auto* t = p.table(*n, "environment")) {
            parse_environment(p, *t, config.environment);
        }
    }
    if (const auto* n = root.get("terrain")) {
        if (const auto* t = p.table(*n, "terrain")) {
            parse_terrain(p, *t, config.terrain.emplace());
        }
    }
    if (const auto* n = root.get("host")) {
        if (const auto* t = p.table(*n, "host")) {
            parse_host(p, *t, config.host.emplace());
        }
    }
    if (const auto* n = root.get("reference_scene")) {
        if (const auto* t = p.table(*n, "reference_scene")) {
            parse_reference_scene(p, *t, config.reference_scene);
        }
    }
    if (const auto* n = root.get("display")) {
        if (const auto* t = p.table(*n, "display")) {
            parse_display(p, *t, config);
        }
    }

    const toml::node* channels = root.get("channel");
    const toml::array* list = channels != nullptr ? channels->as_array() : nullptr;
    if (channels == nullptr || (list != nullptr && list->empty())) {
        // toml++ reports an empty array as not an array of tables, so this
        // is checked first.
        p.error("no channels: add at least one [[channel]] table");
    } else if (list == nullptr || !list->is_array_of_tables()) {
        p.error(channels->source(),
                "'channel' must be a list of tables: write each as [[channel]]");
    } else {
        std::unordered_map<std::string, std::uint32_t> first_line;
        for (std::size_t i = 0; i < list->size(); ++i) {
            const auto& t = *list->get(i)->as_table();
            ChannelConfig channel;
            parse_channel(p, t, "channel[" + std::to_string(i) + "]", channel);
            if (!channel.name.empty()) {
                const auto line = t.get("name")->source().begin.line;
                const auto [it, inserted] = first_line.emplace(channel.name, line);
                if (!inserted) {
                    p.error(t.get("name")->source(), "duplicate channel name \"" + channel.name +
                                                         "\" (first used on line " +
                                                         std::to_string(it->second) + ")");
                }
            }
            config.channels.push_back(std::move(channel));
        }
    }

    if (p.failed()) {
        return {std::nullopt, p.take_errors()};
    }
    return {std::move(config), {}};
}

ConfigResult load_display_config(const std::filesystem::path& file) {
    const std::string name = path_text(file);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return {std::nullopt, {{name, 0, 0, "cannot open the file: not a regular file"}}};
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {std::nullopt, {{name, 0, 0, "cannot open the file"}}};
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) {
        return {std::nullopt, {{name, 0, 0, "cannot read the file"}}};
    }
    ConfigResult r = parse_display_config(text.str(), name);
    if (r.config && r.config->terrain && r.config->terrain->database.is_relative()) {
        r.config->terrain->database = file.parent_path() / r.config->terrain->database;
    }
    return r;
}

}  // namespace fovea
