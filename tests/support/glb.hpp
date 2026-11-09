#pragma once

// .glb (binary glTF) files written byte by byte, so a test controls exactly
// what a file holds (the model library's tests, and models placed in the
// terrain render tests).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fovea::test {

// What goes in a test file: one mesh of one primitive under one node.
struct GlbSpec {
    std::vector<std::array<float, 3>> positions{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    std::vector<std::array<float, 3>> normals{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::uint16_t> indices{0, 1, 2};
    bool indexed = true;
    bool with_normals = true;
    bool with_material = true;
    int mode = 4;  // triangles
    std::string extras = R"({"fovea_material": 7})";
    std::array<float, 4> base_colour{0.5F, 0.25F, 1.0F, 1.0F};
    std::string material;             // more of the material: "\"alphaMode\":\"MASK\","
    std::vector<std::uint8_t> image;  // a base colour texture, if any
    bool image_is_base_colour = true;  // or only in the textures, for s.material to use
    std::string mime = "image/png";
    std::string node;  // more of the node: "\"matrix\":[...],"
    std::string top;   // more of the top level: "\"extensionsRequired\":[...],"
    std::string buffer_uri;
    std::size_t buffer_bytes = 0;  // declared, if not the real size
    bool sparse_indices = false;
    bool shared_image = false;  // a second material, its texture the same image, clamped
};

std::vector<std::uint8_t> make_glb(const GlbSpec& s);

// A box `2 half_x` wide (glTF's x), `height` tall from y = 0 up, `2 half_z`
// deep (z, its front), its faces' normals outward, each face's texture
// coordinates the whole image, upright.
GlbSpec glb_box(float half_x, float height, float half_z);

}  // namespace fovea::test
