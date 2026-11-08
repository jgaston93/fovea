#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "fovea/core/config.hpp"
#include "fovea/core/scene.hpp"

namespace fovea {
namespace {

// The smallest valid config; line 1 is the empty line after R"(.
const std::string kMinimal = R"(
[[channel]]
name = "center"
monitor = 1
render_extent = [1920, 1080]
frustum = { left = -30.0, right = 30.0, top = 17.0, bottom = -17.0 }
)";

ConfigResult parse(const std::string& text) {
    return parse_display_config(text, "test.toml");
}

// Text after replacing the first `from` in kMinimal with `to`.
std::string minimal_with(const std::string& from, const std::string& to) {
    std::string text = kMinimal;
    const auto at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    return text.replace(at, from.size(), to);
}

// Expects exactly one error, on the given line, containing `text`.
void expect_one_error(const ConfigResult& r, std::uint32_t line, const std::string& text) {
    EXPECT_FALSE(r.ok());
    ASSERT_EQ(r.errors.size(), 1U) << (r.errors.empty() ? "" : r.errors[0].to_string());
    EXPECT_EQ(r.errors[0].line, line) << r.errors[0].to_string();
    EXPECT_NE(r.errors[0].message.find(text), std::string::npos) << r.errors[0].to_string();
}

TEST(Config, ExampleFileLoads) {
    const auto r = load_display_config(FOVEA_SOURCE_DIR "/config/channels.example.toml");
    ASSERT_TRUE(r.ok()) << (r.errors.empty() ? "" : r.errors[0].to_string());
    const DisplayConfig& c = *r.config;
    EXPECT_EQ(c.presenter, PresenterKind::multi_swapchain);
    EXPECT_EQ(c.target_hz, 60.0);
    EXPECT_EQ(c.near_m, 0.1);
    ASSERT_EQ(c.channels.size(), 3U);
    const char* names[] = {"left", "center", "right"};  // NOLINT(modernize-avoid-c-arrays)
    const double yaws[] = {-60.0, 0.0, 60.0};           // NOLINT(modernize-avoid-c-arrays)
    for (std::size_t i = 0; i < 3; ++i) {
        const ChannelConfig& ch = c.channels[i];
        EXPECT_EQ(ch.name, names[i]);
        EXPECT_EQ(ch.monitor, static_cast<int>(i));
        EXPECT_EQ(ch.present_mode, PresentMode::fifo);
        EXPECT_EQ(ch.render_width, 1920U);
        EXPECT_EQ(ch.render_height, 1080U);
        EXPECT_EQ(ch.view_rotation.yaw_deg, yaws[i]);
        EXPECT_EQ(ch.frustum.left_deg, -30.0);
        EXPECT_EQ(ch.frustum.right_deg, 30.0);
        EXPECT_EQ(ch.frustum.top_deg, 17.0);
        EXPECT_EQ(ch.frustum.bottom_deg, -17.0);
    }
}

// Every config shipped in config/ must load, not only the example.
TEST(Config, EveryShippedConfigLoads) {
    int loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(FOVEA_SOURCE_DIR "/config")) {
        if (entry.path().extension() != ".toml") {
            continue;
        }
        const auto r = load_display_config(entry.path());
        EXPECT_TRUE(r.ok()) << (r.errors.empty() ? entry.path().string() : r.errors[0].to_string());
        ++loaded;
    }
    EXPECT_GE(loaded, 2);  // channels.example.toml and the desk config, at least
}

TEST(Config, OptionalFieldsTakeDefaults) {
    const auto r = parse(kMinimal);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.config->presenter, PresenterKind::multi_swapchain);
    EXPECT_EQ(r.config->target_hz, 60.0);
    EXPECT_EQ(r.config->near_m, 0.1);
    EXPECT_EQ(r.config->msaa, 4U);
    const ChannelConfig& ch = r.config->channels.at(0);
    EXPECT_EQ(ch.present_mode, PresentMode::fifo);
    EXPECT_EQ(ch.entity, 0U);
    EXPECT_EQ(ch.eye_offset_m, DVec3(0.0));
    EXPECT_EQ(ch.view_rotation.yaw_deg, 0.0);
}

TEST(Config, MsaaIsOneTwoFourOrEight) {
    for (const int n : {1, 2, 4, 8}) {
        const auto r = parse("[display]\nmsaa = " + std::to_string(n) + "\n" + kMinimal);
        ASSERT_TRUE(r.ok()) << n << ": " << r.errors.at(0).to_string();
        EXPECT_EQ(r.config->msaa, static_cast<std::uint32_t>(n));
    }
    for (const char* bad : {"0", "3", "16", "4.0", "\"4\""}) {
        const auto r = parse(std::string("[display]\nmsaa = ") + bad + "\n" + kMinimal);
        EXPECT_FALSE(r.ok()) << bad;
    }
}

// How clouds are drawn (milestone 6 step 6): [display] clouds.
TEST(Config, DisplayChoosesTheCloudModel) {
    EXPECT_EQ(parse(kMinimal).config->clouds, CloudModel::layer);
    for (const auto& [name, model] :
         {std::pair{"layer", CloudModel::layer}, std::pair{"volumetric", CloudModel::volumetric},
          std::pair{"skybox", CloudModel::skybox}}) {
        const auto r = parse(std::string("[display]\nclouds = \"") + name + "\"\n" + kMinimal);
        ASSERT_TRUE(r.ok()) << name;
        EXPECT_EQ(r.config->clouds, model);
    }
    EXPECT_FALSE(parse("[display]\nclouds = \"puffy\"\n" + kMinimal).ok());
    EXPECT_FALSE(parse("[display]\nclouds = 1\n" + kMinimal).ok());
}

TEST(Config, EnvironmentSetsTheDateTimeAndVisibility) {
    const auto d = parse(kMinimal);
    ASSERT_TRUE(d.ok());
    EXPECT_EQ(d.config->environment.year, 2026);  // the default date, as EnvironmentState's
    EXPECT_EQ(d.config->environment.month, 3U);
    EXPECT_EQ(d.config->environment.day, 20U);
    EXPECT_EQ(d.config->environment.utc_seconds, 15.5 * 3600.0);
    EXPECT_TRUE(d.config->environment.time_runs);
    EXPECT_EQ(fovea::julian_date(2026, 3, 20, 15.5 * 3600.0), EnvironmentState{}.julian_date);

    const auto r = parse(
        "[environment]\ndate = 2027-12-31\ntime = 23:59:30.5\ntime_runs = false\n"
        "visibility_m = 8000\n" +
        kMinimal);
    ASSERT_TRUE(r.ok()) << r.errors.at(0).to_string();
    EXPECT_EQ(r.config->environment.year, 2027);
    EXPECT_EQ(r.config->environment.month, 12U);
    EXPECT_EQ(r.config->environment.day, 31U);
    EXPECT_EQ(r.config->environment.utc_seconds, 23 * 3600.0 + 59 * 60.0 + 30.5);
    EXPECT_FALSE(r.config->environment.time_runs);
    EXPECT_EQ(r.config->environment.visibility_m, 8000.0);

    for (const char* bad :
         {"date = \"2026-03-20\"", "time = \"12:00\"", "time_runs = 1", "visibility_m = 0",
          "date = 2026-03-20T12:00:00Z", "season = 2", "date = 1949-12-31", "date = 2051-01-01"}) {
        const auto e = parse("[environment]\n" + std::string(bad) + "\n" + kMinimal);
        EXPECT_FALSE(e.ok()) << bad;
    }
}

// The weather layers (milestone 6 step 6): [[environment.layer]] tables,
// each checked; fog, rain and snow take a visibility of their own kind's
// unless given one.
TEST(Config, EnvironmentLayersAreReadAndChecked) {
    const auto r = parse(
        "[[environment.layer]]\nkind = \"cloud\"\nbase_m = 1500\nthickness_m = 400\n"
        "coverage = 0.4\n"
        "[[environment.layer]]\nkind = \"fog\"\nbase_m = 180\nthickness_m = 60\n"
        "[[environment.layer]]\nkind = \"rain\"\nbase_m = 0\nthickness_m = 1400\n"
        "visibility_m = 3000\n" +
        kMinimal);
    ASSERT_TRUE(r.ok()) << r.errors.at(0).to_string();
    const auto& layers = r.config->environment.layers;
    ASSERT_EQ(layers.size(), 3U);
    EXPECT_EQ(layers[0].kind, WeatherKind::cloud);
    EXPECT_EQ(layers[0].base_m, 1500.0);
    EXPECT_EQ(layers[0].thickness_m, 400.0);
    EXPECT_EQ(layers[0].coverage, 0.4);
    EXPECT_EQ(layers[1].kind, WeatherKind::fog);
    EXPECT_EQ(layers[1].visibility_m, 200.0);
    EXPECT_EQ(layers[2].kind, WeatherKind::rain);
    EXPECT_EQ(layers[2].visibility_m, 3000.0);
    EXPECT_EQ(parse(kMinimal).config->environment.layers.size(), 0U);

    const std::string base = "base_m = 100\nthickness_m = 50\n";
    for (const std::string& bad : std::vector<std::string>{
             "kind = \"hail\"\n" + base, "kind = 3\n" + base, base,
             "kind = \"cloud\"\nthickness_m = 50\n", "kind = \"cloud\"\nbase_m = 100\n",
             "kind = \"fog\"\n" + base + "coverage = 0.5\n",
             "kind = \"cloud\"\n" + base + "visibility_m = 500\n",
             "kind = \"cloud\"\n" + base + "coverage = 1.5\n",
             "kind = \"fog\"\nbase_m = 100\nthickness_m = 0\n",
             "kind = \"fog\"\n" + base + "visibility_m = 0\n",
             "kind = \"fog\"\n" + base + "colour = 1\n"}) {
        const auto e = parse("[[environment.layer]]\n" + bad + kMinimal);
        EXPECT_FALSE(e.ok()) << bad;
    }
    std::string seven;
    for (int i = 0; i < 7; ++i) {
        seven += "[[environment.layer]]\nkind = \"cloud\"\n" + base;
    }
    EXPECT_FALSE(parse(seven + kMinimal).ok());
    EXPECT_FALSE(parse("[environment]\nlayer = 1\n" + kMinimal).ok());
}

TEST(Config, IntegersAreAcceptedForNumbers) {
    const auto r = parse(minimal_with("left = -30.0, right = 30.0", "left = -30, right = 30") +
                         "eye_offset = { x = 1, y = -0.5, z = 0 }\n");
    ASSERT_TRUE(r.ok()) << r.errors.at(0).to_string();
    EXPECT_EQ(r.config->channels[0].frustum.left_deg, -30.0);
    EXPECT_EQ(r.config->channels[0].eye_offset_m, DVec3(1.0, -0.5, 0.0));
}

TEST(Config, AllFieldsAreRead) {
    const auto r =
        parse("[display]\npresenter = \"spanned\"\ntarget_hz = 59.94\nnear_m = 0.25\nmsaa = 8\n" +
              minimal_with("monitor = 1", "monitor = 2\npresent_mode = \"mailbox\"") +
              "view_rotation = { yaw = 45, pitch = -5, roll = 1 }\n");
    ASSERT_TRUE(r.ok()) << r.errors.at(0).to_string();
    EXPECT_EQ(r.config->presenter, PresenterKind::spanned);
    EXPECT_EQ(r.config->target_hz, 59.94);
    EXPECT_EQ(r.config->near_m, 0.25);
    EXPECT_EQ(r.config->msaa, 8U);
    const ChannelConfig& ch = r.config->channels[0];
    EXPECT_EQ(ch.monitor, 2);
    EXPECT_EQ(ch.present_mode, PresentMode::mailbox);
    EXPECT_EQ(ch.view_rotation.yaw_deg, 45.0);
    EXPECT_EQ(ch.view_rotation.pitch_deg, -5.0);
    EXPECT_EQ(ch.view_rotation.roll_deg, 1.0);
}

TEST(Config, ViewpointEntityIsReadAndDefaultsToZero) {
    EXPECT_EQ(parse(kMinimal).config->channels[0].entity, 0U);
    const auto r = parse(minimal_with("monitor = 1", "monitor = 1\nentity = 42"));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.config->channels[0].entity, 42U);
    expect_one_error(parse(minimal_with("monitor = 1", "monitor = 1\nentity = -3")), 5,
                     "'channel[0].entity' must be between 0 and");
}

TEST(Config, ReferenceSceneIsReadWithDefaults) {
    const auto d = parse(kMinimal);
    ASSERT_TRUE(d.ok());
    EXPECT_EQ(d.config->reference_scene.eye_height_m, 2.0);
    EXPECT_EQ(d.config->reference_scene.grid_spacing_m, 10.0);
    EXPECT_EQ(d.config->reference_scene.exposure_ev, 0.0);
    const auto r =
        parse("[reference_scene]\neye_height_m = 150\ngrid_spacing_m = 25\nexposure_ev = -1.5\n" +
              kMinimal);
    ASSERT_TRUE(r.ok()) << r.errors.at(0).to_string();
    EXPECT_EQ(r.config->reference_scene.eye_height_m, 150.0);
    EXPECT_EQ(r.config->reference_scene.grid_spacing_m, 25.0);
    EXPECT_EQ(r.config->reference_scene.exposure_ev, -1.5);
}

TEST(Config, ReferenceSceneValuesAreChecked) {
    expect_one_error(parse("[reference_scene]\neye_height_m = 0\n" + kMinimal), 2,
                     "'reference_scene.eye_height_m' must be greater than 0");
    expect_one_error(parse("[reference_scene]\ngrid_spacing_m = -5\n" + kMinimal), 2,
                     "'reference_scene.grid_spacing_m' must be greater than 0");
    expect_one_error(parse("[reference_scene]\nexposure_ev = 30\n" + kMinimal), 2,
                     "'reference_scene.exposure_ev' must be at least -20 and at most 20");
    expect_one_error(parse("[reference_scene]\neye_hieght_m = 2\n" + kMinimal), 2,
                     "unknown key 'reference_scene.eye_hieght_m'");
}

TEST(Config, SyntaxErrorReportsItsLine) {
    expect_one_error(parse(minimal_with("monitor = 1", "monitor = ")), 4, "");
}

TEST(Config, UnknownKeysAreReportedWhereTheyAre) {
    expect_one_error(parse(minimal_with("monitor = 1", "monitor = 1\nmonitr = 2")), 5,
                     "unknown key 'channel[0].monitr'");
    expect_one_error(parse(minimal_with("top = 17.0", "top = 17.0, botom = 1")), 6,
                     "unknown key 'channel[0].frustum.botom'");
    expect_one_error(parse("chanel = 1\n" + kMinimal), 1, "unknown key 'chanel'");
}

TEST(Config, MissingRequiredFieldIsReportedAtItsTable) {
    expect_one_error(parse(minimal_with("monitor = 1\n", "")), 2, "missing 'channel[0].monitor'");
    expect_one_error(parse(minimal_with("top = 17.0, ", "")), 6,
                     "missing 'channel[0].frustum.top'");
}

TEST(Config, NoChannelsIsAnError) {
    const auto r = parse("[display]\ntarget_hz = 60\n");
    ASSERT_EQ(r.errors.size(), 1U);
    EXPECT_NE(r.errors[0].message.find("no channels"), std::string::npos);
}

TEST(Config, EmptyChannelListIsNoChannels) {
    const auto r = parse("channel = []\n");
    ASSERT_EQ(r.errors.size(), 1U);
    EXPECT_NE(r.errors[0].message.find("no channels"), std::string::npos);
}

TEST(Config, ChannelMustBeAListOfTables) {
    expect_one_error(parse(minimal_with("[[channel]]", "[channel]")), 2, "[[channel]]");
}

TEST(Config, DuplicateChannelNamesAreRejected) {
    expect_one_error(parse(kMinimal + minimal_with("monitor = 1", "monitor = 2")), 9,
                     "duplicate channel name \"center\" (first used on line 3)");
}

TEST(Config, ChannelNamesAreFileNames) {
    // Headless dumps write <name>.png: no separators, '..' or odd characters.
    for (const char* bad : {"../left", "cam/1", "a\\\\b", "a:b", ".hidden", "two words"}) {
        expect_one_error(parse(minimal_with("\"center\"", std::string("\"") + bad + "\"")), 3,
                         "must be up to 64 letters");
    }
    EXPECT_TRUE(parse(minimal_with("\"center\"", "\"Left-2_wide.v1\"")).ok());
}

TEST(Config, HostSectionIsOptional) {
    const auto r = parse(kMinimal);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.config->host.has_value());
}

TEST(Config, HostSectionReadsItsFields) {
    const auto r = parse(kMinimal + R"(
[host]
protocol = "cigi"
port = 8004
listen_address = "192.168.1.10"
host_address = "192.168.1.20"
host_port = 8005
geoid_height_m = -29.5
)");
    ASSERT_TRUE(r.ok()) << r.errors[0].to_string();
    ASSERT_TRUE(r.config->host.has_value());
    const HostConfig& h = *r.config->host;
    EXPECT_EQ(h.protocol, HostProtocol::cigi);
    EXPECT_EQ(h.port, 8004);
    EXPECT_EQ(h.listen_address, 0xC0A8010AU);
    EXPECT_EQ(h.host_address, 0xC0A80114U);
    EXPECT_EQ(h.host_port, 8005);
    EXPECT_DOUBLE_EQ(h.geoid_height_m, -29.5);
}

TEST(Config, HostSectionIsChecked) {
    const std::string head = kMinimal + "[host]\n";  // [host] is line 7
    expect_one_error(parse(head + "port = 8004\n"), 7, "missing 'host.protocol'");
    expect_one_error(parse(head + "protocol = \"dis\"\nport = 8004\n"), 8, "cigi");
    expect_one_error(parse(head + "protocol = \"cigi\"\nport = 0\n"), 9, "host.port");
    expect_one_error(
        parse(head + "protocol = \"cigi\"\nport = 8004\nhost_address = \"1.2.3\"\nhost_port = 1\n"),
        10, "IPv4 address");
    expect_one_error(parse(head + "protocol = \"cigi\"\nport = 8004\nhost_port = 8005\n"), 10,
                     "go together");
    expect_one_error(parse(head + "protocol = \"cigi\"\nport = 8004\ngeoid_height_m = 400\n"), 10,
                     "host.geoid_height_m");
    expect_one_error(
        parse(head + "protocol = \"cigi\"\nport = 8004\nlisten_address = "
                     "\"127.0.0.1\"\nhost_address = \"192.168.1.20\"\nhost_port = 1\n"),
        11, "this computer only");
}

TEST(Config, ReferenceSceneOrigin) {
    const auto r =
        parse(kMinimal + "[reference_scene]\norigin = { lat = 34.6, lon = -86.8, alt = 190.0 }\n");
    ASSERT_TRUE(r.ok()) << r.errors[0].to_string();
    EXPECT_DOUBLE_EQ(r.config->reference_scene.origin.lat_deg, 34.6);
    EXPECT_DOUBLE_EQ(r.config->reference_scene.origin.lon_deg, -86.8);
    EXPECT_DOUBLE_EQ(r.config->reference_scene.origin.alt_m, 190.0);
    expect_one_error(
        parse(kMinimal + "[reference_scene]\norigin = { lat = 95.0, lon = 0.0, alt = 0.0 }\n"), 8,
        "reference_scene.origin.lat");
}

TEST(Config, FrustumMustBeValid) {
    expect_one_error(parse(minimal_with("left = -30.0", "left = 31.0")), 6,
                     "left (31) must be less than right (30)");
    expect_one_error(parse(minimal_with("bottom = -17.0", "bottom = 20")), 6,
                     "bottom (20) must be less than top (17)");
    expect_one_error(parse(minimal_with("right = 30.0", "right = 90")), 6,
                     "'channel[0].frustum.right' must be strictly between -90 and 90");
    expect_one_error(parse(minimal_with("left = -30.0", "left = nan")), 6, "finite number");
}

TEST(Config, MessagesShowValuesExactly) {
    expect_one_error(parse(minimal_with("left = -30.0", "left = 30.0000001")), 6,
                     "left (30.0000001) must be less than right (30)");
}

TEST(Config, ViewRotationMustBeInTheCigiRanges) {
    const std::string base = kMinimal + "view_rotation = { yaw = 0, pitch = 0, roll = 0 }\n";
    const auto with = [&](const std::string& from, const std::string& to) {
        std::string text = base;
        return parse(text.replace(text.rfind(from), from.size(), to));
    };
    expect_one_error(
        with("pitch = 0", "pitch = 100"), 7,
        "'channel[0].view_rotation.pitch' must be between -90 and 90 degrees (got 100)");
    expect_one_error(with("roll = 0", "roll = -181"), 7, "roll' must be between -180 and 180");
    expect_one_error(with("yaw = 0", "yaw = 361"), 7, "yaw' must be between -360 and 360");
    EXPECT_TRUE(with("yaw = 0", "yaw = -300").ok());
}

TEST(Config, MonitorOnlyNeedsToBeNonNegative) {
    EXPECT_TRUE(parse(minimal_with("monitor = 1", "monitor = 64")).ok());
    expect_one_error(parse(minimal_with("monitor = 1", "monitor = -1")), 4,
                     "'channel[0].monitor' must be between 0 and");
}

TEST(Config, RenderExtentMustBeTwoSizesInRange) {
    expect_one_error(parse(minimal_with("[1920, 1080]", "[0, 1080]")), 5,
                     "'channel[0].render_extent[0]' must be between 1 and 16384 (got 0)");
    expect_one_error(parse(minimal_with("[1920, 1080]", "[1920, 20000]")), 5,
                     "must be between 1 and 16384 (got 20000)");
    expect_one_error(parse(minimal_with("[1920, 1080]", "[1920]")), 5, "must be [width, height]");
    expect_one_error(parse(minimal_with("[1920, 1080]", "\"1920x1080\"")), 5,
                     "must be [width, height]");
}

TEST(Config, WrongTypesAreReported) {
    expect_one_error(parse(minimal_with("monitor = 1", "monitor = \"one\"")), 4,
                     "'channel[0].monitor' must be an integer");
    expect_one_error(parse(minimal_with("name = \"center\"", "name = 3")), 3,
                     "'channel[0].name' must be a string");
    expect_one_error(parse(minimal_with("monitor = 1", "monitor = 1\neye_offset = 0")), 5,
                     "'channel[0].eye_offset' must be a table");
}

TEST(Config, EnumValuesListTheChoices) {
    expect_one_error(parse(minimal_with("monitor = 1", "monitor = 1\npresent_mode = \"vsync\"")), 5,
                     "is \"vsync\"; expected one of: fifo, mailbox, immediate");
}

TEST(Config, DisplayValuesAreChecked) {
    expect_one_error(parse("[display]\ntarget_hz = 0\n" + kMinimal), 2,
                     "'display.target_hz' must be between 1 and 1000");
    expect_one_error(parse("[display]\nnear_m = -1\n" + kMinimal), 2,
                     "'display.near_m' must be greater than 0");
}

TEST(Config, EveryProblemIsReported) {
    std::string text = minimal_with("monitor = 1", "monitor = -1");
    text = text.replace(text.find("[1920, 1080]"), 12, "[0, 0]");
    const auto r = parse(text + "colour = \"red\"\n");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errors.size(), 4U);  // monitor, both extents, the unknown key
}

TEST(Config, ErrorsAreInFileOrder) {
    // The unknown key (line 4) is found before the missing field (line 2).
    const auto r = parse(minimal_with("monitor = 1", "monitr = 1"));
    ASSERT_EQ(r.errors.size(), 2U);
    EXPECT_EQ(r.errors[0].line, 2U);
    EXPECT_EQ(r.errors[1].line, 4U);
}

TEST(Config, MissingFileIsReported) {
    const auto r = load_display_config("no/such/file.toml");
    ASSERT_EQ(r.errors.size(), 1U);
    EXPECT_EQ(r.errors[0].line, 0U);
    EXPECT_NE(r.errors[0].message.find("cannot open"), std::string::npos);
}

TEST(Config, DirectoryIsReportedAsNotAFile) {
    const auto r = load_display_config(FOVEA_SOURCE_DIR "/config");
    ASSERT_EQ(r.errors.size(), 1U);
    EXPECT_NE(r.errors[0].message.find("not a regular file"), std::string::npos);
}

TEST(Config, ErrorFormatsAsSourceLineColumn) {
    const ConfigError e{"a.toml", 3, 7, "bad"};
    EXPECT_EQ(e.to_string(), "a.toml:3:7: bad");
}

TEST(Config, TerrainIsOptionalAndChecked) {
    const auto none = parse(kMinimal);
    ASSERT_TRUE(none.ok());
    EXPECT_FALSE(none.config->terrain.has_value());

    const auto r = parse(kMinimal +
                         "[terrain]\ndatabase = \"db/khsv\"\nmax_level = 15\n"
                         "pixel_error = 2.5\n");
    ASSERT_TRUE(r.ok()) << r.errors[0].to_string();
    ASSERT_TRUE(r.config->terrain.has_value());
    EXPECT_EQ(r.config->terrain->database, std::filesystem::path("db/khsv"));  // as written
    EXPECT_EQ(r.config->terrain->max_level, 15);
    EXPECT_DOUBLE_EQ(r.config->terrain->pixel_error, 2.5);
    const auto defaults = parse(kMinimal + "[terrain]\ndatabase = \"db\"\n");
    ASSERT_TRUE(defaults.ok());
    EXPECT_EQ(defaults.config->terrain->max_level, 30);
    EXPECT_DOUBLE_EQ(defaults.config->terrain->pixel_error, 1.0);

    // Line 8 is [terrain]'s first key.
    expect_one_error(parse(kMinimal + "[terrain]\nmax_level = 3\n"), 7, "database");
    expect_one_error(parse(kMinimal + "[terrain]\ndatabase = \"\"\n"), 8, "must name a directory");
    expect_one_error(parse(kMinimal + "[terrain]\ndatabase = \"db\"\nmax_level = 31\n"), 9,
                     "max_level");
    expect_one_error(parse(kMinimal + "[terrain]\ndatabase = \"db\"\npixel_error = 0\n"), 9,
                     "pixel_error");
    expect_one_error(parse(kMinimal + "[terrain]\ndatabase = \"db\"\nlevel = 3\n"), 9, "level");
}

TEST(Config, TerrainDatabaseIsRelativeToTheConfigFile) {
    // config/khsv.toml names "../data/db/khsv": from the repository root.
    const auto r = load_display_config(FOVEA_SOURCE_DIR "/config/khsv.toml");
    ASSERT_TRUE(r.ok()) << r.errors[0].to_string();
    ASSERT_TRUE(r.config->terrain.has_value());
    EXPECT_EQ(r.config->terrain->database.lexically_normal(),
              std::filesystem::path(FOVEA_SOURCE_DIR "/data/db/khsv").lexically_normal());
}

}  // namespace
}  // namespace fovea
