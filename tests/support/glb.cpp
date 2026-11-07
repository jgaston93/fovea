#include "support/glb.hpp"

#include <cstring>
#include <string_view>

namespace fovea::test {

std::vector<std::uint8_t> make_glb(const GlbSpec& s) {
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
        if (s.sparse_indices) {
            // The last index replaced by itself, sparsely.
            const std::uint16_t at = 2;
            const std::uint16_t value = s.indices[2];
            const int iv = add(&at, 2);
            const int vv = add(&value, 2);
            accessors.pop_back();
            accessors +=
                ",\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":" + std::to_string(iv) +
                ",\"componentType\":5123},\"values\":{\"bufferView\":" + std::to_string(vv) + "}}}";
        }
        prim += ",\"indices\":" + std::to_string(i);
    }
    const std::string shape = prim;
    if (s.with_material) {
        prim += ",\"material\":0";
    }
    prim += "}";
    if (s.shared_image) {
        prim += "," + shape + ",\"material\":1}";
    }
    std::string material =
        "{\"name\":\"paint\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[" +
        std::to_string(s.base_colour[0]) + "," + std::to_string(s.base_colour[1]) + "," +
        std::to_string(s.base_colour[2]) + "," + std::to_string(s.base_colour[3]) + "]";
    std::string images;
    if (!s.image.empty()) {
        material += ",\"baseColorTexture\":{\"index\":0}";
        const int v = add(s.image.data(), s.image.size());
        images =
            ",\"textures\":[{\"source\":0,\"sampler\":0},{\"source\":0,\"sampler\":1}],"
            "\"samplers\":[{\"wrapS\":33071,\"wrapT\":10497},{\"wrapS\":10497,\"wrapT\":33071}],"
            "\"images\":[{\"bufferView\":" +
            std::to_string(v) + ",\"mimeType\":\"" + s.mime + "\"}]";
    }
    material += "}";
    if (!s.extras.empty()) {
        material += ",\"extras\":" + s.extras;
    }
    material += "}";
    if (s.shared_image) {
        material += R"(,{"name":"trim","pbrMetallicRoughness":{"baseColorTexture":{"index":1}},)"
                    R"("extras":{"fovea_material":9}})";
    }
    while (bin.size() % 4 != 0) {
        bin.push_back(0);
    }
    std::string buffer =
        "{\"byteLength\":" + std::to_string(s.buffer_bytes != 0 ? s.buffer_bytes : bin.size());
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
    for (const char c : std::string_view("glTF")) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    u32(2);
    u32(static_cast<std::uint32_t>(12 + 8 + json.size() + (has_bin ? 8 + bin.size() : 0)));
    u32(static_cast<std::uint32_t>(json.size()));
    u32(0x4E4F534A);  // JSON
    for (const char c : json) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    if (has_bin) {
        u32(static_cast<std::uint32_t>(bin.size()));
        u32(0x004E4942);  // BIN
        out.insert(out.end(), bin.begin(), bin.end());
    }
    return out;
}

GlbSpec glb_box(float half_x, float height, float half_z) {
    GlbSpec s;
    s.positions.clear();
    s.normals.clear();
    s.indices.clear();
    s.uvs.clear();
    // Each face: its normal, and two axes along it (u x v = normal).
    struct Face {
        std::array<float, 3> n, u, v;
    };
    const std::array<Face, 6> faces = {{
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    }};
    const std::array<float, 3> half = {half_x, height / 2, half_z};
    for (const Face& f : faces) {
        const auto base = static_cast<std::uint16_t>(s.positions.size());
        for (const auto& [a, b] : {std::pair{-1.0F, -1.0F}, std::pair{1.0F, -1.0F},
                                   std::pair{1.0F, 1.0F}, std::pair{-1.0F, 1.0F}}) {
            std::array<float, 3> p{};
            for (std::size_t k = 0; k < 3; ++k) {
                p[k] = (f.n[k] + a * f.u[k] + b * f.v[k]) * half[k];
            }
            p[1] += height / 2;
            s.positions.push_back(p);
            s.normals.push_back(f.n);
            s.uvs.push_back({(a + 1.0F) / 2.0F, (1.0F - b) / 2.0F});
        }
        for (const int k : {0, 1, 2, 0, 2, 3}) {
            s.indices.push_back(static_cast<std::uint16_t>(base + k));
        }
    }
    return s;
}

}  // namespace fovea::test
