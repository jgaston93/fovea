#include "fovea/models/model.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <unordered_map>

#include <cgltf.h>

#include "fovea/core/image_file.hpp"
#include "fovea/core/math.hpp"

namespace fovea {

namespace {

struct DataDeleter {
    void operator()(cgltf_data* d) const { cgltf_free(d); }
};
using Data = std::unique_ptr<cgltf_data, DataDeleter>;

std::string name_or(const char* name, const char* what, std::size_t index) {
    return name != nullptr && *name != '\0' ? std::string(what) + " '" + name + "'"
                                            : std::string(what) + " " + std::to_string(index);
}

std::string result_text(cgltf_result r) {
    switch (r) {
        case cgltf_result_data_too_short:
            return "truncated";
        case cgltf_result_unknown_format:
            return "not glTF";
        case cgltf_result_invalid_json:
            return "malformed JSON";
        case cgltf_result_invalid_gltf:
            return "not valid glTF";
        case cgltf_result_out_of_memory:
            return "out of memory";
        case cgltf_result_legacy_gltf:
            return "glTF 1.0, not 2.0";
        default:
            return "unreadable (cgltf error " + std::to_string(static_cast<int>(r)) + ")";
    }
}

// "fovea_material": an integer from 0 to 255, in a material's extras (a JSON
// object, as cgltf keeps it).
std::optional<int> fovea_material(const cgltf_extras& extras) {
    if (extras.data == nullptr) {
        return std::nullopt;
    }
    const std::string_view text = extras.data;
    constexpr std::string_view key = "\"fovea_material\"";
    const auto at = text.find(key);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t i = at + key.size();
    const auto space = [&] {
        while (i < text.size() &&
               (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) {
            ++i;
        }
    };
    space();
    if (i >= text.size() || text[i] != ':') {
        return std::nullopt;
    }
    ++i;
    space();
    int value = 0;
    std::size_t digits = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 4) {
        value = value * 10 + (text[i] - '0');
        ++i;
        ++digits;
    }
    // A whole number, nothing after it but the end of the value.
    space();
    if (digits == 0 || (i < text.size() && text[i] != ',' && text[i] != '}') || value > 255) {
        return std::nullopt;
    }
    return value;
}

TextureWrap wrap(cgltf_wrap_mode m) {
    switch (m) {
        case cgltf_wrap_mode_clamp_to_edge:
            return TextureWrap::clamp;
        case cgltf_wrap_mode_mirrored_repeat:
            return TextureWrap::mirror;
        default:
            return TextureWrap::repeat;
    }
}

float to_linear(std::uint8_t v) {
    const float c = static_cast<float>(v) / 255.0F;
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

std::uint8_t to_srgb(float c) {
    c = std::clamp(c, 0.0F, 1.0F);
    const float s = c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
    return static_cast<std::uint8_t>(std::lround(s * 255.0F));
}

class Reader {
public:
    explicit Reader(cgltf_data* d) : d_(d) {}

    bool read(Model& m) {
        if (d_->extensions_required_count > 0) {
            error_ = "requires extension";
            for (cgltf_size i = 0; i < d_->extensions_required_count; ++i) {
                error_ += std::string(i == 0 ? " " : ", ") + d_->extensions_required[i];
            }
            return false;
        }
        for (cgltf_size i = 0; i < d_->materials_count; ++i) {
            if (!material(i, m)) {
                return false;
            }
        }
        // The scene's nodes, or with no scene, every node without a parent.
        const cgltf_scene* scene = d_->scene;
        if (scene == nullptr && d_->scenes_count > 0) {
            scene = d_->scenes;
        }
        if (scene != nullptr) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
                if (!node(scene->nodes[i], m)) {
                    return false;
                }
            }
        } else {
            for (cgltf_size i = 0; i < d_->nodes_count; ++i) {
                if (d_->nodes[i].parent == nullptr && !node(&d_->nodes[i], m)) {
                    return false;
                }
            }
        }
        if (m.primitives.empty()) {
            error_ = "has no triangles";
            return false;
        }
        m.min = m.max = m.vertices.front().position;
        for (const ModelVertex& v : m.vertices) {
            for (std::size_t a = 0; a < 3; ++a) {
                m.min[a] = std::min(m.min[a], v.position[a]);
                m.max[a] = std::max(m.max[a], v.position[a]);
            }
        }
        return true;
    }

    const std::string& error() const { return error_; }

private:
    bool material(cgltf_size i, Model& m) {
        const cgltf_material& src = d_->materials[i];
        const std::string what = name_or(src.name, "material", i);
        ModelMaterial mat;
        mat.name = src.name != nullptr ? src.name : "";
        const auto id = fovea_material(src.extras);
        if (!id) {
            error_ = what + ": needs a Fovea material ID, extras {\"fovea_material\": 0 to 255}";
            return false;
        }
        mat.fovea_material = static_cast<std::uint8_t>(*id);
        if (src.alpha_mode == cgltf_alpha_mode_mask) {
            mat.alpha_mode = AlphaMode::mask;
        } else if (src.alpha_mode == cgltf_alpha_mode_blend) {
            mat.alpha_mode = AlphaMode::blend;
        }
        mat.alpha_cutoff = src.alpha_cutoff;
        mat.double_sided = src.double_sided != 0;
        if (src.has_pbr_metallic_roughness != 0) {
            const cgltf_pbr_metallic_roughness& pbr = src.pbr_metallic_roughness;
            std::copy(std::begin(pbr.base_color_factor), std::end(pbr.base_color_factor),
                      mat.base_colour.begin());
            const cgltf_texture_view& view = pbr.base_color_texture;
            if (view.texture != nullptr) {
                if (view.texcoord != 0 || view.has_transform != 0) {
                    error_ = what +
                             ": its base colour texture needs the first texture coordinates, "
                             "untransformed";
                    return false;
                }
                const auto t = texture(view.texture, m, what);
                if (!t) {
                    return false;
                }
                mat.base_colour_texture = t;
            }
        }
        m.materials.push_back(std::move(mat));
        return true;
    }

    std::optional<std::uint32_t> texture(const cgltf_texture* t, Model& m,
                                         const std::string& what) {
        if (const auto found = textures_.find(t); found != textures_.end()) {
            return found->second;
        }
        const cgltf_image* image = t->image;
        if (image == nullptr || image->buffer_view == nullptr) {
            error_ = what + ": its texture's image isn't in the file";
            return std::nullopt;
        }
        const std::uint8_t* bytes = cgltf_buffer_view_data(image->buffer_view);
        if (bytes == nullptr) {
            error_ = what + ": its texture's image has no data";
            return std::nullopt;
        }
        auto decoded = decode_image({bytes, image->buffer_view->size}, kMaxTextureSide);
        if (!decoded) {
            error_ = what + ": its texture isn't a PNG or JPEG image of at most " +
                     std::to_string(kMaxTextureSide) + " pixels a side";
            return std::nullopt;
        }
        ModelTexture tex;
        tex.srgb = true;
        if (t->sampler != nullptr) {
            tex.wrap_u = wrap(t->sampler->wrap_s);
            tex.wrap_v = wrap(t->sampler->wrap_t);
        }
        tex.levels =
            make_mips({decoded->width, decoded->height, std::move(decoded->pixels)}, tex.srgb);
        const auto index = static_cast<std::uint32_t>(m.textures.size());
        m.textures.push_back(std::move(tex));
        textures_.emplace(t, index);
        return index;
    }

    bool node(const cgltf_node* n, Model& m) {
        if (++depth_ > 64) {
            error_ = "nodes nested more than 64 deep";
            return false;
        }
        if (n->mesh != nullptr) {
            std::array<float, 16> world{};
            cgltf_node_transform_world(n, world.data());
            Mat4 to_model(1.0F);
            for (std::size_t c = 0; c < 4; ++c) {
                for (std::size_t r = 0; r < 4; ++r) {
                    to_model[static_cast<glm::length_t>(c)][static_cast<glm::length_t>(r)] =
                        world[c * 4 + r];
                }
            }
            const cgltf_mesh& mesh = *n->mesh;
            for (cgltf_size p = 0; p < mesh.primitives_count; ++p) {
                const std::string what =
                    name_or(mesh.name, "mesh", static_cast<std::size_t>(&mesh - d_->meshes)) +
                    " primitive " + std::to_string(p);
                if (!primitive(mesh.primitives[p], to_model, m, what)) {
                    return false;
                }
            }
        }
        for (cgltf_size i = 0; i < n->children_count; ++i) {
            if (!node(n->children[i], m)) {
                return false;
            }
        }
        --depth_;
        return true;
    }

    bool primitive(const cgltf_primitive& prim, const Mat4& to_model, Model& m,
                   const std::string& what) {
        if (prim.type != cgltf_primitive_type_triangles) {
            error_ = what + ": not triangles (strips, fans, lines and points aren't read)";
            return false;
        }
        if (prim.material == nullptr) {
            error_ = what + ": has no material (each needs one, with a Fovea material ID)";
            return false;
        }
        const auto material = static_cast<std::uint32_t>(prim.material - d_->materials);
        const cgltf_accessor* pos = cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
        const cgltf_accessor* nrm = cgltf_find_accessor(&prim, cgltf_attribute_type_normal, 0);
        const cgltf_accessor* uv = cgltf_find_accessor(&prim, cgltf_attribute_type_texcoord, 0);
        if (pos == nullptr || pos->type != cgltf_type_vec3) {
            error_ = what + ": has no positions";
            return false;
        }
        if (nrm == nullptr || nrm->type != cgltf_type_vec3 || nrm->count != pos->count) {
            error_ = what + ": has no normals (export them from Blender)";
            return false;
        }
        const bool textured = m.materials[material].base_colour_texture.has_value();
        if (textured && (uv == nullptr || uv->type != cgltf_type_vec2 || uv->count != pos->count)) {
            error_ = what + ": its material has a texture but it has no texture coordinates";
            return false;
        }
        const std::size_t count = pos->count;
        if (count == 0 || m.vertices.size() + count > kMaxModelVertices) {
            error_ = what + (count == 0 ? ": has no vertices"
                                        : ": more than " + std::to_string(kMaxModelVertices) +
                                              " vertices in the model");
            return false;
        }
        std::vector<float> p(count * 3);
        std::vector<float> nv(count * 3);
        std::vector<float> t(uv != nullptr && uv->count == count ? count * 2 : 0);
        if (cgltf_accessor_unpack_floats(pos, p.data(), p.size()) != p.size() ||
            cgltf_accessor_unpack_floats(nrm, nv.data(), nv.size()) != nv.size() ||
            (!t.empty() && cgltf_accessor_unpack_floats(uv, t.data(), t.size()) != t.size())) {
            error_ = what + ": its vertex data can't be read";
            return false;
        }
        // Normals by the inverse transpose; a mirroring transform turns the
        // triangles over, so their winding is swapped back.
        const glm::mat3 linear(to_model);
        const glm::mat3 normals = glm::transpose(glm::inverse(linear));
        const bool mirrored = glm::determinant(linear) < 0.0F;
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (std::size_t i = 0; i < count; ++i) {
            const Vec4 at = to_model * Vec4(p[i * 3], p[i * 3 + 1], p[i * 3 + 2], 1.0F);
            Vec3 n = normals * Vec3(nv[i * 3], nv[i * 3 + 1], nv[i * 3 + 2]);
            const float len = glm::length(n);
            n = len > 0.0F ? n / len : Vec3(0.0F, 1.0F, 0.0F);
            ModelVertex v;
            v.position = {at.x, at.y, at.z};
            v.normal = {n.x, n.y, n.z};
            if (!t.empty()) {
                v.uv = {t[i * 2], t[i * 2 + 1]};
            }
            if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) {
                error_ = what + ": a position isn't finite";
                return false;
            }
            m.vertices.push_back(v);
        }
        const std::size_t indices = prim.indices != nullptr ? prim.indices->count : count;
        if (indices % 3 != 0 || m.indices.size() + indices > kMaxModelIndices) {
            error_ = what + (indices % 3 != 0 ? ": its indices aren't whole triangles"
                                              : ": more than " + std::to_string(kMaxModelIndices) +
                                                    " indices in the model");
            return false;
        }
        const auto first = static_cast<std::uint32_t>(m.indices.size());
        for (std::size_t i = 0; i < indices; i += 3) {
            std::array<std::size_t, 3> tri{};
            for (std::size_t k = 0; k < 3; ++k) {
                tri[k] = prim.indices != nullptr ? cgltf_accessor_read_index(prim.indices, i + k)
                                                 : i + k;
                if (tri[k] >= count) {
                    error_ = what + ": an index past its vertices";
                    return false;
                }
            }
            if (mirrored) {
                std::swap(tri[1], tri[2]);
            }
            for (const std::size_t k : tri) {
                m.indices.push_back(base + static_cast<std::uint32_t>(k));
            }
        }
        m.primitives.push_back({first, static_cast<std::uint32_t>(indices), material});
        return true;
    }

    cgltf_data* d_;
    std::string error_;
    std::unordered_map<const cgltf_texture*, std::uint32_t> textures_;
    int depth_ = 0;
};

}  // namespace

std::vector<TextureLevel> make_mips(TextureLevel base, bool srgb) {
    std::vector<TextureLevel> levels;
    levels.push_back(std::move(base));
    while (levels.back().width > 1 || levels.back().height > 1) {
        const TextureLevel& a = levels.back();
        TextureLevel b;
        b.width = std::max(1U, a.width / 2);
        b.height = std::max(1U, a.height / 2);
        b.pixels.resize(std::size_t{b.width} * b.height * 4);
        for (std::uint32_t y = 0; y < b.height; ++y) {
            // An odd last row or column joins the last box.
            const std::uint32_t y0 = y * 2;
            const std::uint32_t y1 = y + 1 == b.height ? a.height : std::min(a.height, y0 + 2);
            for (std::uint32_t x = 0; x < b.width; ++x) {
                const std::uint32_t x0 = x * 2;
                const std::uint32_t x1 = x + 1 == b.width ? a.width : std::min(a.width, x0 + 2);
                std::array<float, 4> sum{};
                for (std::uint32_t yy = y0; yy < y1; ++yy) {
                    for (std::uint32_t xx = x0; xx < x1; ++xx) {
                        const std::uint8_t* px = &a.pixels[(std::size_t{yy} * a.width + xx) * 4];
                        for (std::size_t c = 0; c < 3; ++c) {
                            sum[c] += srgb ? to_linear(px[c]) : static_cast<float>(px[c]) / 255.0F;
                        }
                        sum[3] += static_cast<float>(px[3]) / 255.0F;
                    }
                }
                const auto n = static_cast<float>((y1 - y0) * (x1 - x0));
                std::uint8_t* out = &b.pixels[(std::size_t{y} * b.width + x) * 4];
                for (std::size_t c = 0; c < 3; ++c) {
                    out[c] = srgb ? to_srgb(sum[c] / n)
                                  : static_cast<std::uint8_t>(
                                        std::lround(std::clamp(sum[c] / n, 0.0F, 1.0F) * 255.0F));
                }
                out[3] = static_cast<std::uint8_t>(
                    std::lround(std::clamp(sum[3] / n, 0.0F, 1.0F) * 255.0F));
            }
        }
        levels.push_back(std::move(b));
    }
    return levels;
}

ModelResult read_glb(std::span<const std::uint8_t> bytes) {
    ModelResult r;
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "glTF", 4) != 0) {
        r.error = "not a .glb (binary glTF) file";
        return r;
    }
    if (bytes.size() > kMaxModelBytes) {
        r.error = "larger than " + std::to_string(kMaxModelBytes >> 20) + " MB";
        return r;
    }
    cgltf_options options{};
    options.type = cgltf_file_type_glb;
    cgltf_data* parsed = nullptr;
    cgltf_result res = cgltf_parse(&options, bytes.data(), bytes.size(), &parsed);
    const Data data(parsed);
    if (res == cgltf_result_success) {
        // External files are refused before cgltf would open them (data:
        // URIs it decodes itself).
        for (cgltf_size i = 0; i < data->buffers_count; ++i) {
            const char* uri = data->buffers[i].uri;
            if (uri != nullptr && std::strncmp(uri, "data:", 5) != 0) {
                r.error = name_or(data->buffers[i].name, "buffer", i) + ": an external file (" +
                          uri + "); a .glb holds its own";
                return r;
            }
        }
        res = cgltf_load_buffers(&options, data.get(), nullptr);
    }
    if (res == cgltf_result_success) {
        res = cgltf_validate(data.get());
    }
    if (res != cgltf_result_success) {
        r.error = result_text(res);
        return r;
    }
    Model m;
    Reader reader(data.get());
    if (!reader.read(m)) {
        r.error = reader.error();
        return r;
    }
    r.model = std::move(m);
    return r;
}

ModelResult read_model_file(const std::filesystem::path& file) {
    ModelResult r;
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        r.error = file.string() + ": cannot read";
        return r;
    }
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > kMaxModelBytes) {
        r.error = file.string() + ": larger than " + std::to_string(kMaxModelBytes >> 20) + " MB";
        return r;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        r.error = file.string() + ": cannot read";
        return r;
    }
    r = read_glb(bytes);
    if (!r.model) {
        r.error = file.string() + ": " + r.error;
    }
    return r;
}

}  // namespace fovea
