// The model library (milestone 4 step 2, D-023): .glb files written here,
// byte by byte, so each test controls exactly what a file holds.

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/image_file.hpp"
#include "fovea/models/model.hpp"

namespace fovea {
namespace {

// What goes in a test file: one mesh of one primitive under one node.
struct Spec {
    std::vector<std::array<float, 3>> positions{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    std::vector<std::array<float, 3>> normals{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::uint16_t> indices{0, 1, 2};
    bool indexed = true;
    bool with_normals = true;
    bool with_material = true;
    int mode = 4;  // triangles
    std::string extras = R"({"fovea_material": 7})";
    std::vector<std::uint8_t> image;  // a base colour texture, if any
    std::string mime = "image/png";
    std::string node;  // more of the node: "\"matrix\":[...],"
    std::string top;   // more of the top level: "\"extensionsRequired\":[...],"
    std::string buffer_uri;
};

std::vector<std::uint8_t> make_glb(const Spec& s) {
    std::vector<std::uint8_t> bin;
    std::string views;
    // A buffer view of `bytes`, 4-byte aligned; returns its index.
    int view_count = 0;
    const auto add = [&](const void* data, std::size_t bytes) {
        while (bin.size() % 4 != 0) {
            bin.push_back(0);
        }
        const std::size_t at = bin.size();
        bin.resize(at + bytes);
        std::memcpy(bin.data() + at, data, bytes);
        views += std::string(views.empty() ? "" : ",") +
                 "{\"buffer\":0,\"byteOffset\":" + std::to_string(at) +
                 ",\"byteLength\":" + std::to_string(bytes) + "}";
        return view_count++;
    };
    std::string accessors;
    int accessor_count = 0;
    const auto accessor = [&](int view, int component, std::size_t count, const char* type) {
        accessors += std::string(accessors.empty() ? "" : ",") +
                     "{\"bufferView\":" + std::to_string(view) +
                     ",\"componentType\":" + std::to_string(component) +
                     ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"}";
        return accessor_count++;
    };
    std::string attributes;
    const int pos = accessor(add(s.positions.data(), s.positions.size() * 12), 5126,
                             s.positions.size(), "VEC3");
    attributes += "\"POSITION\":" + std::to_string(pos);
    if (s.with_normals) {
        const int n =
            accessor(add(s.normals.data(), s.normals.size() * 12), 5126, s.normals.size(), "VEC3");
        attributes += ",\"NORMAL\":" + std::to_string(n);
    }
    if (!s.uvs.empty()) {
        const int t = accessor(add(s.uvs.data(), s.uvs.size() * 8), 5126, s.uvs.size(), "VEC2");
        attributes += ",\"TEXCOORD_0\":" + std::to_string(t);
    }
    std::string prim = "{\"attributes\":{" + attributes + "},\"mode\":" + std::to_string(s.mode);
    if (s.indexed) {
        const int i =
            accessor(add(s.indices.data(), s.indices.size() * 2), 5123, s.indices.size(), "SCALAR");
        prim += ",\"indices\":" + std::to_string(i);
    }
    if (s.with_material) {
        prim += ",\"material\":0";
    }
    prim += "}";
    std::string material =
        "{\"name\":\"paint\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.25,1,1]";
    std::string images;
    if (!s.image.empty()) {
        material += ",\"baseColorTexture\":{\"index\":0}";
        const int v = add(s.image.data(), s.image.size());
        images =
            ",\"textures\":[{\"source\":0,\"sampler\":0}],"
            "\"samplers\":[{\"wrapS\":33071,\"wrapT\":10497}],"
            "\"images\":[{\"bufferView\":" +
            std::to_string(v) + ",\"mimeType\":\"" + s.mime + "\"}]";
    }
    material += "}";
    if (!s.extras.empty()) {
        material += ",\"extras\":" + s.extras;
    }
    material += "}";
    while (bin.size() % 4 != 0) {
        bin.push_back(0);
    }
    std::string buffer = "{\"byteLength\":" + std::to_string(bin.size());
    if (!s.buffer_uri.empty()) {
        buffer += ",\"uri\":\"" + s.buffer_uri + "\"";
    }
    buffer += "}";
    std::string json = "{\"asset\":{\"version\":\"2.0\"}," + s.top +
                       "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{" + s.node +
                       "\"mesh\":0}],\"meshes\":[{\"name\":\"box\",\"primitives\":[" + prim +
                       "]}],\"materials\":[" + material + "]" + images + ",\"buffers\":[" + buffer +
                       "],\"bufferViews\":[" + views + "],\"accessors\":[" + accessors + "]}";
    while (json.size() % 4 != 0) {
        json += ' ';
    }
    std::vector<std::uint8_t> out;
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    const bool has_bin = s.buffer_uri.empty();
    out.insert(out.end(), {'g', 'l', 'T', 'F'});
    u32(2);
    u32(static_cast<std::uint32_t>(12 + 8 + json.size() + (has_bin ? 8 + bin.size() : 0)));
    u32(static_cast<std::uint32_t>(json.size()));
    u32(0x4E4F534A);  // JSON
    out.insert(out.end(), json.begin(), json.end());
    if (has_bin) {
        u32(static_cast<std::uint32_t>(bin.size()));
        u32(0x004E4942);  // BIN
        out.insert(out.end(), bin.begin(), bin.end());
    }
    return out;
}

RgbaImage quadrants() {
    // 4 x 4: red, green, blue and white quarters, top left first.
    RgbaImage im{4, 4, std::vector<std::uint8_t>(64)};
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const int q = (y < 2 ? 0 : 2) + (x < 2 ? 0 : 1);
            const std::array<std::uint8_t, 4> c =
                q == 0   ? std::array<std::uint8_t, 4>{255, 0, 0, 255}
                : q == 1 ? std::array<std::uint8_t, 4>{0, 255, 0, 255}
                : q == 2 ? std::array<std::uint8_t, 4>{0, 0, 255, 255}
                         : std::array<std::uint8_t, 4>{255, 255, 255, 255};
            std::copy(c.begin(), c.end(), im.pixels.begin() + (y * 4 + x) * 4);
        }
    }
    return im;
}

Spec textured() {
    Spec s;
    s.uvs = {{0, 0}, {1, 0}, {0, 1}};
    s.image = encode_png(quadrants());
    return s;
}

std::string refused(const Spec& s) {
    const ModelResult r = read_glb(make_glb(s));
    EXPECT_FALSE(r.model.has_value());
    EXPECT_FALSE(r.error.empty());
    return r.error;
}

TEST(Models, ReadsATexturedTriangle) {
    const ModelResult r = read_glb(make_glb(textured()));
    ASSERT_TRUE(r.model.has_value()) << r.error;
    const Model& m = *r.model;
    ASSERT_EQ(m.vertices.size(), 3U);
    EXPECT_EQ(m.vertices[1].position, (std::array<float, 3>{1, 0, 0}));
    EXPECT_EQ(m.vertices[2].normal, (std::array<float, 3>{0, 0, 1}));
    EXPECT_EQ(m.vertices[2].uv, (std::array<float, 2>{0, 1}));
    EXPECT_EQ(m.indices, (std::vector<std::uint32_t>{0, 1, 2}));
    ASSERT_EQ(m.primitives.size(), 1U);
    EXPECT_EQ(m.primitives[0].index_count, 3U);
    ASSERT_EQ(m.materials.size(), 1U);
    const ModelMaterial& mat = m.materials[0];
    EXPECT_EQ(mat.name, "paint");
    EXPECT_EQ(mat.fovea_material, 7);
    EXPECT_EQ(mat.base_colour, (std::array<float, 4>{0.5F, 0.25F, 1.0F, 1.0F}));
    EXPECT_EQ(mat.alpha_mode, AlphaMode::opaque);
    ASSERT_EQ(mat.base_colour_texture, 0U);
    ASSERT_EQ(m.textures.size(), 1U);
    const ModelTexture& t = m.textures[0];
    EXPECT_EQ(t.wrap_u, TextureWrap::clamp);
    EXPECT_EQ(t.wrap_v, TextureWrap::repeat);
    ASSERT_EQ(t.levels.size(), 3U);  // 4, 2, 1
    EXPECT_EQ(t.levels[0].pixels, quadrants().pixels);
    EXPECT_EQ(t.levels[1].width, 2U);
    // The second level's top left texel is the red quarter's.
    EXPECT_EQ(t.levels[1].pixels[0], 255);
    EXPECT_EQ(t.levels[1].pixels[1], 0);
    EXPECT_EQ(t.levels[2].width, 1U);
    EXPECT_EQ(m.min, (std::array<float, 3>{0, 0, 0}));
    EXPECT_EQ(m.max, (std::array<float, 3>{1, 1, 0}));
}

TEST(Models, ReadsJpegTextures) {
    Spec s = textured();
    RgbaImage grey{16, 8, std::vector<std::uint8_t>(16 * 8 * 4, 128)};
    s.image = encode_jpeg(grey, 95);
    s.mime = "image/jpeg";
    const ModelResult r = read_glb(make_glb(s));
    ASSERT_TRUE(r.model.has_value()) << r.error;
    const TextureLevel& base = r.model->textures[0].levels[0];
    EXPECT_EQ(base.width, 16U);
    EXPECT_EQ(base.height, 8U);
    EXPECT_NEAR(base.pixels[0], 128, 2);
    EXPECT_EQ(base.pixels[3], 255);                     // opaque
    EXPECT_EQ(r.model->textures[0].levels.size(), 5U);  // 16x8 to 1x1
}

// Front faces' winding agrees with their vertex normals.
bool faces_agree(const Model& m) {
    for (std::size_t i = 0; i < m.indices.size(); i += 3) {
        const auto& a = m.vertices[m.indices[i]].position;
        const auto& b = m.vertices[m.indices[i + 1]].position;
        const auto& c = m.vertices[m.indices[i + 2]].position;
        const std::array<float, 3> u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const std::array<float, 3> v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const std::array<float, 3> n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                                     u[0] * v[1] - u[1] * v[0]};
        const auto& vn = m.vertices[m.indices[i]].normal;
        if (n[0] * vn[0] + n[1] * vn[1] + n[2] * vn[2] <= 0.0F) {
            return false;
        }
    }
    return true;
}

TEST(Models, NodeTransformsApplyAndMirroringKeepsTheFrontFaces) {
    Spec s;
    s.positions = {{0, 0, 0}, {1, 0, 0}, {0, 0, -1}};  // facing up
    s.normals = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
    s.node = R"("translation":[10,2,-3],"scale":[2,2,2],)";
    ModelResult r = read_glb(make_glb(s));
    ASSERT_TRUE(r.model.has_value()) << r.error;
    EXPECT_EQ(r.model->vertices[1].position, (std::array<float, 3>{12, 2, -3}));
    EXPECT_EQ(r.model->vertices[0].normal, (std::array<float, 3>{0, 1, 0}));  // still unit
    EXPECT_TRUE(faces_agree(*r.model));
    // Mirrored in x, and sheared: the normal goes by the inverse transpose,
    // and the winding is turned back.
    s.node = R"("matrix":[-1,0,0,0, 0.5,1,0,0, 0,0,1,0, 0,0,0,1],)";
    r = read_glb(make_glb(s));
    ASSERT_TRUE(r.model.has_value()) << r.error;
    EXPECT_EQ(r.model->vertices[1].position, (std::array<float, 3>{-1, 0, 0}));
    EXPECT_TRUE(faces_agree(*r.model));
    const auto& n = r.model->vertices[0].normal;
    EXPECT_NEAR(n[0] * n[0] + n[1] * n[1] + n[2] * n[2], 1.0F, 1e-6F);
    EXPECT_NE(r.model->indices, (std::vector<std::uint32_t>{0, 1, 2}));
}

TEST(Models, UnindexedTrianglesAreIndexedInOrder) {
    Spec s;
    s.indexed = false;
    const ModelResult r = read_glb(make_glb(s));
    ASSERT_TRUE(r.model.has_value()) << r.error;
    EXPECT_EQ(r.model->indices, (std::vector<std::uint32_t>{0, 1, 2}));
}

TEST(Models, MaterialsNeedAFoveaMaterialId) {
    Spec s;
    s.extras = "";
    EXPECT_NE(refused(s).find("fovea_material"), std::string::npos);
    for (const char* bad :
         {R"({"fovea_material": 256})", R"({"fovea_material": "7"})", R"({"fovea_material": -1})",
          R"({"fovea_material": 7.5})", R"({"other": 7})"}) {
        s.extras = bad;
        EXPECT_NE(refused(s).find("material 'paint'"), std::string::npos) << bad;
    }
    s.extras = R"({"note": "x", "fovea_material" : 255 })";
    const ModelResult r = read_glb(make_glb(s));
    ASSERT_TRUE(r.model.has_value()) << r.error;
    EXPECT_EQ(r.model->materials[0].fovea_material, 255);
}

TEST(Models, RefusesWhatItDoesNotRead) {
    EXPECT_NE(read_glb(std::vector<std::uint8_t>{'n', 'o', 'p', 'e'}).error.find(".glb"),
              std::string::npos);
    auto bytes = make_glb(Spec{});
    bytes.resize(bytes.size() - 20);
    EXPECT_FALSE(read_glb(bytes).model.has_value());  // truncated

    Spec s;
    s.top = R"("extensionsUsed":["KHR_draco_mesh_compression"],)"
            R"("extensionsRequired":["KHR_draco_mesh_compression"],)";
    EXPECT_NE(refused(s).find("KHR_draco_mesh_compression"), std::string::npos);
    s = {};
    s.mode = 1;  // lines
    EXPECT_NE(refused(s).find("not triangles"), std::string::npos);
    s = {};
    s.with_normals = false;
    EXPECT_NE(refused(s).find("normals"), std::string::npos);
    s = {};
    s.with_material = false;
    EXPECT_NE(refused(s).find("no material"), std::string::npos);
    s = textured();
    s.uvs.clear();
    EXPECT_NE(refused(s).find("texture coordinates"), std::string::npos);
    s = textured();
    s.image = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_NE(refused(s).find("PNG or JPEG"), std::string::npos);
    s = {};
    s.indices = {0, 1, 3};
    EXPECT_FALSE(read_glb(make_glb(s)).model.has_value());  // past the vertices
    s = {};
    s.indices = {0, 1};
    EXPECT_FALSE(read_glb(make_glb(s)).model.has_value());  // not whole triangles
    s = {};
    s.buffer_uri = "box.bin";
    EXPECT_NE(refused(s).find("external file"), std::string::npos);
}

TEST(Models, DamagedFilesNeverCrash) {
    // Random bytes changed in a good file: read or refused, never worse
    // (the sanitizer builds check the "never worse").
    const std::vector<std::uint8_t> good = make_glb(textured());
    std::mt19937 rng(3);
    int read = 0;
    for (int i = 0; i < 500; ++i) {
        std::vector<std::uint8_t> b = good;
        for (int k = 0; k < 4; ++k) {
            b[std::uniform_int_distribution<std::size_t>(0, b.size() - 1)(rng)] =
                static_cast<std::uint8_t>(rng());
        }
        const ModelResult r = read_glb(b);
        read += r.model.has_value() ? 1 : 0;
        EXPECT_TRUE(r.model.has_value() || !r.error.empty());
    }
    EXPECT_GT(read, 0);
}

TEST(Models, MipsAverageColourInLinearLight) {
    // Black and white: 50% in linear light is 188 in sRGB, not 128.
    TextureLevel checker{2, 2, {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0}};
    auto srgb = make_mips(checker, true);
    ASSERT_EQ(srgb.size(), 2U);
    EXPECT_EQ(srgb[1].pixels[0], 188);
    EXPECT_EQ(srgb[1].pixels[3], 191);  // alpha as is: (255 * 3 + 0) / 4
    auto plain = make_mips(checker, false);
    EXPECT_EQ(plain[1].pixels[0], 128);
    // Odd sizes: the last row or column joins the last box; a solid colour
    // stays itself.
    TextureLevel odd{5, 3, {}};
    for (int i = 0; i < 15; ++i) {
        odd.pixels.insert(odd.pixels.end(), {40, 120, 200, 255});
    }
    const auto levels = make_mips(odd, true);
    ASSERT_EQ(levels.size(), 3U);
    EXPECT_EQ(levels[1].width, 2U);
    EXPECT_EQ(levels[1].height, 1U);
    EXPECT_EQ(levels[2].width, 1U);
    for (const TextureLevel& l : levels) {
        EXPECT_EQ(l.pixels[0], 40);
        EXPECT_EQ(l.pixels[2], 200);
    }
}

TEST(Models, ReadsFilesAndNamesThemInErrors) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("fovea-models-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    const auto bytes = make_glb(textured());
    std::ofstream(dir / "tri.glb", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    EXPECT_TRUE(read_model_file(dir / "tri.glb").model.has_value());
    const ModelResult missing = read_model_file(dir / "none.glb");
    EXPECT_NE(missing.error.find("none.glb"), std::string::npos);
    std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace fovea
