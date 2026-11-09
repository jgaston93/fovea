// The model library (milestone 4 step 2, D-023): .glb files written here,
// byte by byte, so each test controls exactly what a file holds.

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/image_file.hpp"
#include "fovea/models/model.hpp"
#include "support/glb.hpp"

namespace fovea {
namespace {

using Spec = test::GlbSpec;
using test::make_glb;

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
    ASSERT_EQ(m.images.size(), 1U);
    const ModelImage& im = m.images[t.image];
    EXPECT_EQ(t.wrap_u, TextureWrap::clamp);
    EXPECT_EQ(t.wrap_v, TextureWrap::repeat);
    ASSERT_EQ(im.levels.size(), 3U);  // 4, 2, 1
    EXPECT_EQ(im.levels[0].pixels, quadrants().pixels);
    EXPECT_EQ(im.levels[1].width, 2U);
    // The second level's top left texel is the red quarter's.
    EXPECT_EQ(im.levels[1].pixels[0], 255);
    EXPECT_EQ(im.levels[1].pixels[1], 0);
    EXPECT_EQ(im.levels[2].width, 1U);
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
    const TextureLevel& base = r.model->images[0].levels[0];
    EXPECT_EQ(base.width, 16U);
    EXPECT_EQ(base.height, 8U);
    EXPECT_NEAR(base.pixels[0], 128, 2);
    EXPECT_EQ(base.pixels[3], 255);                   // opaque
    EXPECT_EQ(r.model->images[0].levels.size(), 5U);  // 16x8 to 1x1
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

TEST(Models, AnImageSharedByTexturesIsDecodedOnce) {
    Spec s = textured();
    s.shared_image = true;
    const ModelResult r = read_glb(make_glb(s));
    ASSERT_TRUE(r.model.has_value()) << r.error;
    const Model& m = *r.model;
    ASSERT_EQ(m.materials.size(), 2U);
    EXPECT_EQ(m.materials[1].fovea_material, 9);
    ASSERT_EQ(m.textures.size(), 2U);
    EXPECT_EQ(m.images.size(), 1U);
    EXPECT_EQ(m.textures[0].image, m.textures[1].image);
    EXPECT_EQ(m.textures[0].wrap_u, TextureWrap::clamp);
    EXPECT_EQ(m.textures[1].wrap_u, TextureWrap::repeat);
    EXPECT_EQ(m.textures[1].wrap_v, TextureWrap::clamp);
    ASSERT_EQ(m.primitives.size(), 2U);
    EXPECT_EQ(m.primitives[1].material, 1U);
}

// Lit windows (milestone 6 step 8, D-033): emission in cd/m², its factor
// times KHR_materials_emissive_strength, and its own texture.
TEST(Models, ReadsEmission) {
    Spec s;
    const ModelResult none = read_glb(make_glb(s));
    ASSERT_TRUE(none.model.has_value()) << none.error;
    EXPECT_EQ(none.model->materials[0].emissive, (std::array<float, 3>{0, 0, 0}));
    EXPECT_FALSE(none.model->materials[0].emissive_texture.has_value());

    s.material = R"("emissiveFactor":[1,0.5,0.25],)"
                 R"("extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":40}},)";
    s.top = R"("extensionsUsed":["KHR_materials_emissive_strength"],)";
    const ModelResult strong = read_glb(make_glb(s));
    ASSERT_TRUE(strong.model.has_value()) << strong.error;
    EXPECT_EQ(strong.model->materials[0].emissive, (std::array<float, 3>{40, 20, 10}));

    // Its own texture, with the base colour's or alone (which still needs
    // the texture coordinates).
    s = textured();
    s.material = R"("emissiveFactor":[1,1,1],"emissiveTexture":{"index":0},)";
    const ModelResult lit = read_glb(make_glb(s));
    ASSERT_TRUE(lit.model.has_value()) << lit.error;
    EXPECT_EQ(lit.model->materials[0].emissive_texture, 0U);
    EXPECT_EQ(lit.model->images.size(), 1U);  // shared with the base colour
    s.image_is_base_colour = false;
    const ModelResult only = read_glb(make_glb(s));
    ASSERT_TRUE(only.model.has_value()) << only.error;
    EXPECT_FALSE(only.model->materials[0].base_colour_texture.has_value());
    EXPECT_EQ(only.model->materials[0].emissive_texture, 0U);
    s.uvs.clear();
    EXPECT_NE(refused(s).find("texture coordinates"), std::string::npos);

    s = {};
    s.material = R"("emissiveFactor":[1,1,1],)"
                 R"("extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":-1}},)";
    EXPECT_NE(refused(s).find("emission"), std::string::npos);
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
    // Only the top level counts: a group of custom properties may hold
    // its own.
    s.extras = R"({"lod": {"fovea_material": 3}})";
    EXPECT_NE(refused(s).find("fovea_material"), std::string::npos);
    s.extras = R"({"lod": {"fovea_material": 3, "x": [1, "}"]}, "fovea_material": 12})";
    const ModelResult nested = read_glb(make_glb(s));
    ASSERT_TRUE(nested.model.has_value()) << nested.error;
    EXPECT_EQ(nested.model->materials[0].fovea_material, 12);
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
    s = {};
    s.sparse_indices = true;
    EXPECT_NE(refused(s).find("sparse"), std::string::npos);
    s = {};
    s.buffer_bytes = std::size_t{1} << 40;  // a terabyte, declared
    EXPECT_NE(refused(s).find("more bytes"), std::string::npos);
    s = {};
    s.node = R"("skin":0,)";
    s.top = R"("skins":[{"joints":[0]}],)";
    EXPECT_NE(refused(s).find("skinned"), std::string::npos);
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
