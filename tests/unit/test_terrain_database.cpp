#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <string>

#include <gtest/gtest.h>

#include "fovea/core/geodesy.hpp"
#include "fovea/terrain/database.hpp"
#include "fovea/terrain/terrain_query.hpp"
#include "fovea/terrain/tile_file.hpp"
#include "fovea/terrain/tile_key.hpp"
#include "support/terrain_db.hpp"

namespace fovea {
namespace {

using test::make_db;
using test::open_db;
using test::test_root;
using test::TestDb;

// --- Tile keys (D-015) ---

TEST(TileKey, LevelZeroIsTwoHemispheres) {
    EXPECT_EQ(tile_columns(0), 2U);
    EXPECT_EQ(tile_rows(0), 1U);
    const GeoBounds w = tile_bounds({0, 0, 0});
    const GeoBounds e = tile_bounds({0, 1, 0});
    EXPECT_DOUBLE_EQ(w.west_deg, -180.0);
    EXPECT_DOUBLE_EQ(w.east_deg, 0.0);
    EXPECT_DOUBLE_EQ(e.east_deg, 180.0);
    EXPECT_DOUBLE_EQ(w.south_deg, -90.0);
    EXPECT_DOUBLE_EQ(w.north_deg, 90.0);
}

TEST(TileKey, APointIsInsideItsTileAtEveryLevel) {
    const double lat = 34.637;
    const double lon = -86.775;
    for (std::uint8_t level = 0; level <= kMaxTileLevel; ++level) {
        const TileKey k = tile_at(lat, lon, level);
        ASSERT_TRUE(is_valid(k)) << int{level};
        const GeoBounds b = tile_bounds(k);
        EXPECT_LE(b.south_deg, lat);
        EXPECT_GT(b.north_deg, lat);
        EXPECT_LE(b.west_deg, lon);
        EXPECT_GT(b.east_deg, lon);
        EXPECT_DOUBLE_EQ(b.north_deg - b.south_deg, tile_size_deg(level));
    }
    // Edges: the point goes north-east; the poles and 180 E clamp inside.
    EXPECT_EQ(tile_at(0.0, 0.0, 1).y, 1U);
    EXPECT_EQ(tile_at(0.0, 0.0, 1).x, 2U);
    EXPECT_EQ(tile_at(90.0, 180.0, 3).y, tile_rows(3) - 1);
    EXPECT_EQ(tile_at(90.0, 180.0, 3).x, tile_columns(3) - 1);
}

TEST(TileKey, ParentsChildrenAndNeighbours) {
    const TileKey k{12, 1200, 3000};
    for (const TileKey& c : children(k)) {
        ASSERT_TRUE(parent(c).has_value());
        EXPECT_EQ(*parent(c), k);
        const GeoBounds cb = tile_bounds(c);
        const GeoBounds kb = tile_bounds(k);
        EXPECT_GE(cb.west_deg, kb.west_deg);
        EXPECT_LE(cb.east_deg, kb.east_deg);
    }
    EXPECT_FALSE(parent({0, 1, 0}).has_value());
    // Columns wrap at the antimeridian; rows stop at the poles.
    EXPECT_EQ(neighbour({3, 0, 2}, -1, 0)->x, tile_columns(3) - 1);
    EXPECT_EQ(neighbour({3, tile_columns(3) - 1, 2}, 1, 0)->x, 0U);
    EXPECT_FALSE(neighbour({3, 5, 0}, 0, -1).has_value());
    EXPECT_FALSE(neighbour({3, 5, tile_rows(3) - 1}, 0, 1).has_value());
    EXPECT_EQ(to_string(k), "12/1200/3000");
    EXPECT_FALSE(is_valid({2, 8, 0}));
    EXPECT_FALSE(is_valid({2, 0, 4}));
}

// --- Tile files (D-018) ---

TileFile two_layer_tile() {
    TileFile t;
    t.key = {9, 300, 400};
    t.layers.push_back({"first", 7, {1, 2, 3}});  // odd sizes: alignment
    t.layers.push_back({"second", 9, {9, 8, 7, 6, 5}});
    return t;
}

TEST(TileFile, RoundTrips) {
    const TileFile t = two_layer_tile();
    const auto bytes = encode_tile(t);
    EXPECT_EQ(bytes.size() % 8, 0U);
    const TileResult r = decode_tile(bytes, t.key);
    ASSERT_TRUE(r.tile.has_value()) << r.detail;
    EXPECT_EQ(r.tile->key, t.key);
    ASSERT_EQ(r.tile->layers.size(), 2U);
    EXPECT_EQ(r.tile->layers[1].name, "second");
    EXPECT_EQ(r.tile->layers[1].format, 9U);
    EXPECT_EQ(r.tile->layers[1].data, t.layers[1].data);
    ASSERT_NE(r.tile->find("first"), nullptr);
    EXPECT_EQ(r.tile->find("nothing"), nullptr);
}

TEST(TileFile, DamageIsFound) {
    const auto good = encode_tile(two_layer_tile());
    auto check = [&](std::vector<std::uint8_t> bytes, TileError expected) {
        EXPECT_EQ(decode_tile(bytes, TileKey{9, 300, 400}).error, expected);
    };
    auto flip = [&](std::size_t at) {
        auto b = good;
        b[at] ^= 0x40;
        return b;
    };
    check(flip(good.size() - 8), TileError::checksum);  // in the second layer's data
    check(flip(0), TileError::bad_magic);
    check(flip(4), TileError::wrong_schema);
    check(std::vector<std::uint8_t>(good.begin(), good.begin() + 20), TileError::too_short);
    check(std::vector<std::uint8_t>(good.begin(), good.begin() + 60), TileError::bad_layer_table);
    auto many = good;
    many[23] = 0x7F;  // a layer count in the billions
    check(many, TileError::bad_layer_table);
    EXPECT_EQ(decode_tile(good, TileKey{9, 300, 401}).error, TileError::wrong_key);
    // Every truncation into the data is caught. Cutting only the padding
    // after the last layer (3 bytes here) leaves the tile whole.
    const std::size_t data_end = good.size() - 3;
    for (std::size_t n = 0; n < good.size(); ++n) {
        const std::vector<std::uint8_t> cut(good.begin(), good.begin() + static_cast<long>(n));
        const TileResult r = decode_tile(cut, std::nullopt);
        if (n < data_end) {
            EXPECT_FALSE(r.tile.has_value()) << n;
        } else {
            ASSERT_TRUE(r.tile.has_value()) << n;
            EXPECT_EQ(r.tile->layers[1].data, two_layer_tile().layers[1].data);
        }
    }
}

TEST(TileFile, Crc32MatchesTheStandard) {
    const std::string s = "123456789";  // the standard check value
    EXPECT_EQ(crc32({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}), 0xCBF43926U);
}

TEST(TileFile, ElevationLayerRoundTripsAndRefusesNonsense) {
    ElevationGrid g;
    g.size = 3;
    g.heights = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F, 9.5F};
    g.min_m = 1.0F;
    g.max_m = 9.5F;
    g.error_m = 0.25F;
    const auto back = decode_elevation(encode_elevation(g));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->heights, g.heights);
    EXPECT_EQ(back->error_m, 0.25F);
    TileLayer bad = encode_elevation(g);
    bad.data.pop_back();
    EXPECT_FALSE(decode_elevation(bad).has_value());
    ElevationGrid nan = g;
    nan.heights[4] = std::nanf("");
    EXPECT_FALSE(decode_elevation(encode_elevation(nan)).has_value());
}

TEST(TileFile, ImageryLayerRoundTripsAndRefusesNonsense) {
    EXPECT_EQ(imagery_mip_count(512), 10U);
    EXPECT_EQ(imagery_mip_count(4), 3U);
    EXPECT_EQ(imagery_mip_bytes(ImageryFormat::bc7_srgb, 512), 128U * 128 * 16);
    EXPECT_EQ(imagery_mip_bytes(ImageryFormat::bc1_srgb, 8), 4U * 8);
    EXPECT_EQ(imagery_mip_bytes(ImageryFormat::bc7_srgb, 2), 16U);  // one block below 4 x 4
    EXPECT_EQ(imagery_mip_bytes(ImageryFormat::rgba8_srgb, 2), 16U);
    EXPECT_EQ(imagery_format_from_name("bc1"), ImageryFormat::bc1_srgb);
    EXPECT_FALSE(imagery_format_from_name("bc3").has_value());

    ImageryImage image;
    image.format = ImageryFormat::bc1_srgb;
    image.size = 8;
    for (std::uint32_t s = 8; s >= 1; s /= 2) {
        auto& mip = image.mips.emplace_back(imagery_mip_bytes(image.format, s));
        for (std::size_t i = 0; i < mip.size(); ++i) {
            mip[i] = static_cast<std::uint8_t>(i * 7 + s);
        }
    }
    const TileLayer layer = encode_imagery(image);
    EXPECT_EQ(layer.name, kImageryLayer);
    EXPECT_EQ(layer.format, 2U);
    const auto back = decode_imagery(layer);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->format, ImageryFormat::bc1_srgb);
    EXPECT_EQ(back->size, 8U);
    EXPECT_EQ(back->mips, image.mips);

    // Through a tile file, beside the elevation.
    ElevationGrid g;
    g.size = 2;
    g.heights = {1.0F, 2.0F, 3.0F, 4.0F};
    const TileFile tile{{3, 1, 2}, {encode_elevation(g), layer}};
    const TileResult read = decode_tile(encode_tile(tile), TileKey{3, 1, 2});
    ASSERT_TRUE(read.tile.has_value()) << read.detail;
    ASSERT_NE(read.tile->find(kImageryLayer), nullptr);
    EXPECT_EQ(decode_imagery(*read.tile->find(kImageryLayer))->mips, image.mips);

    const auto broken = [&](const std::function<void(TileLayer&)>& f) {
        TileLayer l = layer;
        f(l);
        return !decode_imagery(l).has_value();
    };
    EXPECT_TRUE(broken([](TileLayer& l) { l.data.pop_back(); }));
    EXPECT_TRUE(broken([](TileLayer& l) { l.data.push_back(0); }));
    EXPECT_TRUE(broken([](TileLayer& l) { l.format = 9; }));
    EXPECT_TRUE(broken([](TileLayer& l) { l.format = 256 + 1; }));  // not bc7 when narrowed
    EXPECT_TRUE(broken([](TileLayer& l) { l.data[0] = 6; }));       // not a power of two
    EXPECT_TRUE(broken([](TileLayer& l) { l.data[0] = 2; }));       // below 4
    EXPECT_TRUE(broken([](TileLayer& l) { l.data[4] = 3; }));       // not the full chain
    EXPECT_TRUE(broken([](TileLayer& l) { l.data.resize(4); }));
}

// --- Databases ---

const GeoBounds kArea{34.60, 34.67, -86.80, -86.74};

TEST(TileFile, PlacementsLayerRoundTripsAndRefusesNonsense) {
    const std::vector<Placement> placed = {
        {"tower", 34.6372, -86.7751, 190.25, 12.5F},
        {"hangar_a-2", -12.0, 179.9999, -3.0, 359.75F},
    };
    const TileLayer layer = *encode_placements(placed);
    EXPECT_EQ(layer.name, "placements");
    const auto back = decode_placements(layer);
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->size(), 2U);
    EXPECT_EQ((*back)[1].model, "hangar_a-2");
    EXPECT_EQ((*back)[0].lat_deg, 34.6372);
    EXPECT_EQ((*back)[0].lon_deg, -86.7751);
    EXPECT_EQ((*back)[0].height_m, 190.25);
    EXPECT_EQ((*back)[1].heading_deg, 359.75F);
    EXPECT_TRUE(decode_placements(*encode_placements({}))->empty());
    EXPECT_EQ(model_path("tower"), std::filesystem::path("models") / "tower.glb");

    // Every truncation, extra bytes, and bad values are refused.
    for (std::size_t n = 0; n < layer.data.size(); ++n) {
        TileLayer cut = layer;
        cut.data.resize(n);
        EXPECT_FALSE(decode_placements(cut).has_value()) << n;
    }
    TileLayer extra = layer;
    extra.data.push_back(0);
    EXPECT_FALSE(decode_placements(extra).has_value());
    for (const Placement& bad :
         {Placement{"../etc", 0, 0, 0, 0}, Placement{"", 0, 0, 0, 0}, Placement{"a b", 0, 0, 0, 0},
          Placement{"ok", 91, 0, 0, 0}, Placement{"ok", 0, -181, 0, 0},
          Placement{"ok", 0, 0, 0, 360}, Placement{"ok", 0, 0, 0, -1},
          Placement{"ok", 0, 0, std::nan(""), 0}, Placement{std::string(65, 'a'), 0, 0, 0, 0}}) {
        EXPECT_FALSE(encode_placements({&bad, 1}).has_value()) << bad.model;  // never written
    }
    // Nor read: a good layer changed after it was written.
    const TileLayer one = *encode_placements(std::vector<Placement>{{"ok", 0, 0, 0, 0}});
    TileLayer slash = one;
    slash.data[6] = '/';  // the name's first letter
    EXPECT_FALSE(decode_placements(slash).has_value());
    TileLayer round = one;
    const float full_circle = 360.0F;
    std::memcpy(&round.data[4 + 2 + 2 + 24], &full_circle, 4);  // the heading (little-endian here)
    EXPECT_FALSE(decode_placements(round).has_value());
    TileLayer padded = one;
    padded.data[4 + 2 + 2 + 28] = 1;  // the zero bytes after it
    EXPECT_FALSE(decode_placements(padded).has_value());
    TileLayer v2 = layer;
    v2.format = 2;
    EXPECT_FALSE(decode_placements(v2).has_value());
}

TEST(TileFile, LightsLayerRoundTripsAndRefusesNonsense) {
    using enum LightColour;
    const std::vector<TileLight> lights = {
        {34.6372, -86.7751, 190.25, 179.5F, 0.0F, LightType::edge, {white, yellow}},
        {-12.0, 179.9999, -3.0, 359.75F, 3.25F, LightType::papi, {red, none}},
        {0.0, 0.0, 0.0, 0.0F, 0.0F, LightType::threshold, {none, green}},
    };
    const TileLayer layer = *encode_lights(lights);
    EXPECT_EQ(layer.name, "lights");
    EXPECT_EQ(layer.data.size(), 4U + 3U * 36U);
    const auto back = decode_lights(layer);
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->size(), 3U);
    EXPECT_EQ((*back)[0].lat_deg, 34.6372);
    EXPECT_EQ((*back)[0].lon_deg, -86.7751);
    EXPECT_EQ((*back)[0].height_m, 190.25);
    EXPECT_EQ((*back)[0].heading_deg, 179.5F);
    EXPECT_EQ((*back)[0].colour[1], yellow);
    EXPECT_EQ((*back)[1].type, LightType::papi);
    EXPECT_EQ((*back)[1].papi_angle_deg, 3.25F);
    EXPECT_EQ((*back)[1].colour[1], none);
    EXPECT_EQ((*back)[2].colour[1], green);
    EXPECT_TRUE(decode_lights(*encode_lights({}))->empty());

    // Every truncation, extra bytes, and bad values are refused.
    for (std::size_t n = 0; n < layer.data.size(); ++n) {
        TileLayer cut = layer;
        cut.data.resize(n);
        EXPECT_FALSE(decode_lights(cut).has_value()) << n;
    }
    TileLayer extra = layer;
    extra.data.push_back(0);
    EXPECT_FALSE(decode_lights(extra).has_value());
    TileLayer counted = layer;
    counted.data[0] = 200;  // more than are there
    EXPECT_FALSE(decode_lights(counted).has_value());
    const TileLight ok{0, 0, 0, 0.0F, 0.0F, LightType::edge, {white, white}};
    const auto changed = [&](auto change) {
        TileLight l = ok;
        change(l);
        return l;
    };
    for (const TileLight& bad : {
             changed([](TileLight& l) { l.lat_deg = 91; }),
             changed([](TileLight& l) { l.lon_deg = -181; }),
             changed([](TileLight& l) { l.height_m = std::nan(""); }),
             changed([](TileLight& l) { l.heading_deg = 360.0F; }),
             changed([](TileLight& l) { l.heading_deg = -1.0F; }),
             changed([](TileLight& l) { l.colour = {none, none}; }),  // dark both ways
             changed([](TileLight& l) { l.colour[0] = static_cast<LightColour>(5); }),
             changed([](TileLight& l) { l.type = static_cast<LightType>(5); }),
             changed([](TileLight& l) { l.papi_angle_deg = 3.0F; }),  // not a PAPI
             changed([](TileLight& l) {
                 l.type = LightType::papi;
                 l.papi_angle_deg = 90.0F;
             }),
         }) {
        EXPECT_FALSE(encode_lights({&bad, 1}).has_value());  // never written
    }
    // Nor read: a good layer changed after it was written.
    const TileLayer one = *encode_lights({&ok, 1});
    struct Byte {
        std::size_t at;
        std::uint8_t value;
    };
    // The type, the colours (each out of range) and the zero byte.
    for (const Byte& b : {Byte{36, 5}, Byte{37, 5}, Byte{38, 9}, Byte{39, 1}}) {
        TileLayer bad = one;
        bad.data[b.at] = b.value;
        EXPECT_FALSE(decode_lights(bad).has_value()) << b.at;
    }
    TileLayer v2 = layer;
    v2.format = 2;
    EXPECT_FALSE(decode_lights(v2).has_value());
}

TEST(TerrainDatabase, ManifestRoundTripsAndIsChecked) {
    DatabaseManifest m;
    m.name = "test";
    m.area = kArea;
    m.levels = {{10, 270, 271, 708, 709}, {12, 1080, 1086, 2833, 2838}};
    const ManifestResult back = parse_manifest(manifest_text(m), "m.toml");
    ASSERT_TRUE(back.manifest.has_value()) << back.errors[0];
    EXPECT_EQ(back.manifest->name, "test");
    EXPECT_DOUBLE_EQ(back.manifest->area.west_deg, -86.80);
    ASSERT_EQ(back.manifest->levels.size(), 2U);
    EXPECT_TRUE(back.manifest->has_tile({12, 1081, 2835}));
    EXPECT_FALSE(back.manifest->has_tile({12, 1087, 2835}));
    EXPECT_EQ(back.manifest->finest_level(), 12);
    EXPECT_FALSE(back.manifest->imagery.has_value());

    m.imagery = DatabaseManifest::Imagery{ImageryFormat::bc1_srgb, 256};
    const ManifestResult with = parse_manifest(manifest_text(m), "m.toml");
    ASSERT_TRUE(with.manifest.has_value()) << with.errors[0];
    ASSERT_TRUE(with.manifest->imagery.has_value());
    EXPECT_EQ(with.manifest->imagery->format, ImageryFormat::bc1_srgb);
    EXPECT_EQ(with.manifest->imagery->texels, 256U);
    for (const char* bad : {"format = \"bc3\"\ntexels = 256", "format = \"bc7\"\ntexels = 300",
                            "format = \"bc7\"\ntexels = 8192", "format = \"bc7\""}) {
        std::string text = manifest_text(m);
        const std::size_t at = text.find("format = \"bc1\"\ntexels = 256");
        text.replace(at, std::string_view("format = \"bc1\"\ntexels = 256").size(), bad);
        EXPECT_FALSE(parse_manifest(text, "m.toml").manifest.has_value()) << bad;
    }
    EXPECT_FALSE(with.manifest->placements.has_value());
    m.placements = DatabaseManifest::Placements{10};
    const ManifestResult placed = parse_manifest(manifest_text(m), "m.toml");
    ASSERT_TRUE(placed.manifest.has_value()) << placed.errors[0];
    ASSERT_TRUE(placed.manifest->placements.has_value());
    EXPECT_EQ(placed.manifest->placements->level, 10);
    std::string text = manifest_text(m);
    text.replace(text.find("level = 10\n\n[[level]]"), 10, "level = 31");
    EXPECT_FALSE(parse_manifest(text, "m.toml").manifest.has_value());
    EXPECT_FALSE(placed.manifest->lights.has_value());
    m.lights = DatabaseManifest::Lights{11};
    const ManifestResult lit = parse_manifest(manifest_text(m), "m.toml");
    ASSERT_TRUE(lit.manifest.has_value()) << lit.errors[0];
    ASSERT_TRUE(lit.manifest->lights.has_value());
    EXPECT_EQ(lit.manifest->lights->level, 11);
    std::string dark = manifest_text(m);
    dark.replace(dark.find("level = 11\n"), 10, "level = -1");
    EXPECT_FALSE(parse_manifest(dark, "m.toml").manifest.has_value());

    EXPECT_FALSE(parse_manifest("schema = 2\n[layers.elevation]\nformat = \"float32\"\nsamples = "
                                "17\n[[level]]\nlevel = 1\nx = [0, 0]\ny = [0, 0]\n",
                                "m")
                     .manifest.has_value());
    const ManifestResult bad = parse_manifest(
        "schema = 1\n[area]\nsouth = 0\nnorth = 1\nwest = 0\neast = 1\n[layers.elevation]\n"
        "format = \"float32\"\nsamples = 17\n[[level]]\nlevel = 2\nx = [3, 1]\ny = [0, 0]\n",
        "m.toml");
    EXPECT_FALSE(bad.manifest.has_value());
    ASSERT_FALSE(bad.errors.empty());
    EXPECT_NE(bad.errors[0].find("m.toml:10: level 2"), std::string::npos) << bad.errors[0];
}

TEST(DatabaseTerrain, HeightsComeBackExactlyFromTheFinestTile) {
    // A tilted plane (bilinear sampling is exact on it); the finer level is
    // 5 m higher, so the answers show which level they came from.
    const auto plane = [](double lat, double lon) {
        return 200.0 + 3000.0 * (lat - 34.6) - 1500.0 * (lon + 86.8);
    };
    const GeoBounds fine{34.62, 34.64, -86.78, -86.76};
    const TestDb coarse_db = make_db("plane_coarse", kArea, {10}, 17,
                                     [&](std::uint8_t, double a, double o) { return plane(a, o); });
    auto db = open_db(coarse_db);
    const DatabaseTerrain terrain(*db);
    for (const auto& [lat, lon] :
         {std::pair{34.61, -86.79}, {34.637, -86.775}, {34.665, -86.745}}) {
        ASSERT_TRUE(terrain.height_at(lat, lon).has_value());
        EXPECT_NEAR(*terrain.height_at(lat, lon), plane(lat, lon), 2e-4) << lat << "," << lon;
    }
    EXPECT_FALSE(terrain.height_at(40.0, -86.77).has_value());  // outside the tiles

    // Two levels: inside the fine tiles, the fine level; outside them, the
    // coarse one.
    TestDb two = make_db("plane_two_levels", kArea, {10}, 17,
                         [&](std::uint8_t, double a, double o) { return plane(a, o); });
    const TestDb fine_db =
        make_db("plane_fine_only", fine, {13}, 17,
                [&](std::uint8_t, double a, double o) { return plane(a, o) + 5.0; });
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fine_db.dir / "tiles")) {
        if (entry.is_regular_file()) {
            const auto rel = std::filesystem::relative(entry.path(), fine_db.dir);
            std::filesystem::create_directories((two.dir / rel).parent_path());
            std::filesystem::copy_file(entry.path(), two.dir / rel);
        }
    }
    two.manifest.levels.push_back(fine_db.manifest.levels[0]);
    ASSERT_TRUE(write_manifest(two.dir, two.manifest));
    auto db2 = open_db(two);
    const DatabaseTerrain both(*db2);
    EXPECT_NEAR(*both.height_at(34.63, -86.77), plane(34.63, -86.77) + 5.0, 2e-4);
    // Level 13's tiles run 34.607-34.651 N: 34.66 N is outside them.
    EXPECT_NEAR(*both.height_at(34.66, -86.79), plane(34.66, -86.79), 2e-4);

    // A damaged fine tile: skipped, reported, and the coarse level answers.
    const TileKey k = tile_at(34.63, -86.77, 13);
    const auto file = two.dir / tile_path(k);
    auto bytes = std::vector<char>(std::filesystem::file_size(file));
    {
        std::ifstream in(file, std::ios::binary);
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bytes[bytes.size() - 16] ^= 0x10;
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const DatabaseTerrain damaged(*db2);
    EXPECT_NEAR(*damaged.height_at(34.63, -86.77), plane(34.63, -86.77), 2e-4);
    ASSERT_EQ(damaged.problems().size(), 1U);
    EXPECT_NE(damaged.problems()[0].find("CRC-32"), std::string::npos) << damaged.problems()[0];
}

TEST(DatabaseTerrain, RaysHitFlatTerrainWhereTheEllipsoidModelSays) {
    // Flat terrain 150 m above the ellipsoid: hits agree with the exact
    // ellipsoid intersection (ConstantHeightTerrain) to a centimetre.
    const TestDb flat =
        make_db("flat150", kArea, {11}, 33, [](std::uint8_t, double, double) { return 150.0; });
    auto db = open_db(flat);
    const DatabaseTerrain terrain(*db);
    const ConstantHeightTerrain exact(150.0);
    const Geodetic eye{34.637, -86.775, 1150.0};
    const DVec3 origin = geodetic_to_ecef(eye);
    const DMat3 ned = ned_to_ecef(eye.lat_deg, eye.lon_deg);
    for (const DVec3 dir_ned :
         {DVec3{0.0, 0.0, 1.0}, DVec3{1.0, 0.3, 0.5}, DVec3{-0.4, 1.0, 0.25}}) {
        const DRay ray{origin, ned * dir_ned};
        const auto got = terrain.intersect(ray, 20000.0);
        const auto want = exact.intersect(ray, 20000.0);
        ASSERT_TRUE(got.has_value());
        ASSERT_TRUE(want.has_value());
        EXPECT_NEAR(got->distance_m, want->distance_m, 0.01);
        EXPECT_LT(glm::length(got->point - want->point), 0.01);
    }
    // Upward, or leaving the area before reaching the ground: no hit.
    EXPECT_FALSE(terrain.intersect({origin, ned * DVec3{0.0, 0.0, -1.0}}, 20000.0).has_value());
    EXPECT_FALSE(terrain.intersect({origin, ned * DVec3{1.0, 0.0, 0.01}}, 20000.0).has_value());
    // Starting underground: a hit where it starts.
    const DVec3 below = geodetic_to_ecef({34.637, -86.775, 100.0});
    const auto under = terrain.intersect({below, ned * DVec3{1.0, 0.0, 0.0}}, 100.0);
    ASSERT_TRUE(under.has_value());
    EXPECT_EQ(under->distance_m, 0.0);
    // Line of sight across the flat ground between two points 2 m up.
    const DVec3 a = geodetic_to_ecef({34.63, -86.78, 152.0});
    const DVec3 b = geodetic_to_ecef({34.63, -86.77, 152.0});
    EXPECT_TRUE(terrain.line_of_sight(a, b));
}

TEST(TerrainDatabase, ManifestNamesSourcesAndSizesAreSafe) {
    DatabaseManifest m;
    m.name = R"(KHSV "test" C:\terrain)";
    m.area = kArea;
    m.sources = {{"elevation", "USGS 3DEP 1 m DEM", "U.S. Geological Survey"}};
    m.levels = {{10, 270, 271, 708, 709}};
    const ManifestResult back = parse_manifest(manifest_text(m), "m.toml");
    ASSERT_TRUE(back.manifest.has_value()) << back.errors[0];
    EXPECT_EQ(back.manifest->name, m.name);
    ASSERT_EQ(back.manifest->sources.size(), 1U);
    EXPECT_EQ(back.manifest->sources[0].credit, "U.S. Geological Survey");
    // A size past 32 bits doesn't wrap into range (2^32 + 17).
    std::string text = manifest_text(m);
    const auto at = text.find("samples = 257");
    ASSERT_NE(at, std::string::npos);
    text.replace(at, 13, "samples = 4294967313");
    EXPECT_FALSE(parse_manifest(text, "m.toml").manifest.has_value());
}

TEST(TileFile, AFailedWriteLeavesNothingBehind) {
    // The tile's directory would have to be inside a regular file.
    const auto blocker = test_root() / "blocker";
    std::filesystem::create_directories(test_root());
    { std::ofstream(blocker) << "a file"; }
    const auto file = blocker / "tiles" / "1.fvt";
    EXPECT_FALSE(write_tile(file, two_layer_tile()));
    EXPECT_FALSE(std::filesystem::exists(file));
    auto part = file;
    part += ".part";
    EXPECT_FALSE(std::filesystem::exists(part));
}

TEST(DatabaseTerrain, OddPointsAndEdges) {
    const auto plane = [](double lat, double lon) {
        return 100.0 + 1000.0 * (lat - 34.6) + 500.0 * (lon + 86.8);
    };
    const TestDb t = make_db("edges", kArea, {11}, 17,
                             [&](std::uint8_t, double a, double o) { return plane(a, o); });
    auto db = open_db(t);
    const DatabaseTerrain terrain(*db);
    EXPECT_FALSE(terrain.height_at(std::nan(""), -86.77).has_value());
    EXPECT_FALSE(terrain.height_at(34.63, std::numeric_limits<double>::infinity()).has_value());
    // A longitude a turn away is the same place.
    ASSERT_TRUE(terrain.height_at(34.63, -86.77 + 360.0).has_value());
    EXPECT_NEAR(*terrain.height_at(34.63, -86.77 + 360.0), plane(34.63, -86.77), 1e-3);
    // The database's own north-east corner: held by its last tile.
    const LevelTiles& l = t.manifest.levels[0];
    const GeoBounds ne = tile_bounds({l.level, l.x1, l.y1});
    ASSERT_TRUE(terrain.height_at(ne.north_deg, ne.east_deg).has_value());
    EXPECT_NEAR(*terrain.height_at(ne.north_deg, ne.east_deg), plane(ne.north_deg, ne.east_deg),
                1e-3);
    // An unlimited ray that misses ends anyway: going up, or leaving the
    // tiles low and level.
    const Geodetic eye{34.637, -86.775, 400.0};
    const DMat3 ned = ned_to_ecef(eye.lat_deg, eye.lon_deg);
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(
        terrain.intersect({geodetic_to_ecef(eye), ned * DVec3{0, 0, -1}}, inf).has_value());
    EXPECT_FALSE(
        terrain.intersect({geodetic_to_ecef(eye), ned * DVec3{0, 1, -0.01}}, 1e12).has_value());
}

TEST(DatabaseTerrain, ATileWhoseGridDisagreesWithTheManifestIsSkipped) {
    TestDb t =
        make_db("wrong_grid", kArea, {11}, 17, [](std::uint8_t, double, double) { return 80.0; });
    t.manifest.elevation_samples = 33;  // the tiles hold 17
    ASSERT_TRUE(write_manifest(t.dir, t.manifest));
    auto db = open_db(t);
    const DatabaseTerrain terrain(*db);
    EXPECT_FALSE(terrain.height_at(34.63, -86.77).has_value());
    ASSERT_EQ(terrain.problems().size(), 1U);
    EXPECT_NE(terrain.problems()[0].find("17-sample grid"), std::string::npos)
        << terrain.problems()[0];
}

TEST(DatabaseTerrain, ARidgeBlocksTheView) {
    // A 60 m east-west ridge across the middle of the area.
    const double ridge_lat = 34.635;
    const TestDb ridge = make_db("ridge", kArea, {12}, 65, [&](std::uint8_t, double lat, double) {
        return 150.0 + 60.0 * std::max(0.0, 1.0 - std::abs(lat - ridge_lat) / 0.002);
    });
    auto db = open_db(ridge);
    const DatabaseTerrain terrain(*db);
    const DVec3 south = geodetic_to_ecef({34.625, -86.77, 160.0});
    const DVec3 north = geodetic_to_ecef({34.645, -86.77, 160.0});
    EXPECT_FALSE(terrain.line_of_sight(south, north));
    const DVec3 south_high = geodetic_to_ecef({34.625, -86.77, 260.0});
    const DVec3 north_high = geodetic_to_ecef({34.645, -86.77, 260.0});
    EXPECT_TRUE(terrain.line_of_sight(south_high, north_high));
    // The hit is on the ridge's south slope.
    const auto hit = terrain.intersect({south, north - south}, glm::length(north - south));
    ASSERT_TRUE(hit.has_value());
    const Geodetic g = ecef_to_geodetic(hit->point);
    EXPECT_LT(g.lat_deg, ridge_lat);
    EXPECT_NEAR(g.alt_m, *terrain.height_at(g.lat_deg, g.lon_deg), 0.01);
}

}  // namespace
}  // namespace fovea
