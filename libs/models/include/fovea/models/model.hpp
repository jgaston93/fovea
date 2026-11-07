#pragma once

// Models (milestone 4 step 2, D-023): glTF 2.0 binary files (.glb), as
// Blender exports them, read into what the renderer uploads: one vertex and
// one index array, primitives drawing ranges of them with a material each,
// and textures decoded with their mip chains (D-024). CPU only and no
// Vulkan, like libs/terrain; it allocates freely, so it runs on the paging
// thread (or anywhere a host wants it), never on the render path.
//
// A model is flattened: every mesh of its scene, through its nodes'
// transforms, in the model's own frame, glTF's: metres, +Y up, +Z the
// model's front, its ground line at y = 0. Articulated parts (milestone 6)
// will keep their nodes.
//
// Each material names its Fovea material in its extras (in Blender, a custom
// property): {"fovea_material": 12}, an ID from 0 to 255, as terrain
// material maps will use (fovea-plan.md: material IDs from day one).
//
// Read: triangles with positions and normals (texture coordinates where a
// material has a texture), indexed or not, any accessor types (sparse too);
// materials' base colour factor and texture (PNG or JPEG in the file),
// alpha mode, double-sidedness. Refused, with a reason: anything else a file
// requires (extensions it lists as required, other primitive modes,
// external files), and limits past those below.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fovea {

struct ModelVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
};

enum class AlphaMode : std::uint8_t { opaque, mask, blend };
enum class TextureWrap : std::uint8_t { repeat, clamp, mirror };

struct ModelMaterial {
    std::string name;
    std::uint8_t fovea_material = 0;
    std::array<float, 4> base_colour{1.0F, 1.0F, 1.0F, 1.0F};  // linear
    std::optional<std::uint32_t> base_colour_texture;          // into Model::textures
    AlphaMode alpha_mode = AlphaMode::opaque;
    float alpha_cutoff = 0.5F;
    bool double_sided = false;
};

// One mip level: RGBA8, rows top to bottom (as the image file has them).
struct TextureLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

struct ModelTexture {
    std::vector<TextureLevel> levels;  // full chain, down to 1 x 1
    bool srgb = true;                  // base colour is
    TextureWrap wrap_u = TextureWrap::repeat;
    TextureWrap wrap_v = TextureWrap::repeat;
};

struct ModelPrimitive {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material = 0;  // into Model::materials
};

struct Model {
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;  // into vertices, three per triangle, counter-clockwise
    std::vector<ModelPrimitive> primitives;
    std::vector<ModelMaterial> materials;
    std::vector<ModelTexture> textures;
    std::array<float, 3> min{};  // bounds of the vertices
    std::array<float, 3> max{};
};

// Limits past which a model is refused rather than read.
inline constexpr std::size_t kMaxModelBytes = std::size_t{256} << 20;
inline constexpr std::uint32_t kMaxModelVertices = 1U << 24;
inline constexpr std::uint32_t kMaxModelIndices = 3U << 24;
inline constexpr std::uint32_t kMaxTextureSide = 8192;

struct ModelResult {
    std::optional<Model> model;
    std::string error;  // why not, when there's no model
};

// A .glb in memory.
ModelResult read_glb(std::span<const std::uint8_t> bytes);
// A .glb file ("<file>: why" in the error).
ModelResult read_model_file(const std::filesystem::path& file);

// A mip chain for an RGBA8 image (2 x 2 boxes, an odd edge folded into the
// last box; colour averaged in linear light when `srgb`, alpha as is).
std::vector<TextureLevel> make_mips(TextureLevel base, bool srgb);

}  // namespace fovea
