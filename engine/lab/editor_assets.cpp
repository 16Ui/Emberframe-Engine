#include "editor_assets.h"
#include "editor_lights.h"
#include "shading.h"
#include "environment.h"
#include <fastgltf/parser.hpp>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>

// 私有 stb 实现与 systems.cpp 的 PNG/JPEG 实现互不冲突；从内存解码避免窄字符文件名。
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4505)
#endif
#include <stb_image.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace emberframe::lab {
namespace {
using Limits = EditorAssetLimits;
[[noreturn]] void fail(const std::string& message) { throw std::invalid_argument("Editor asset: " + message); }
void require(bool condition, const std::string& message) { if (!condition) fail(message); }
void add_budget(std::size_t& total, std::size_t amount, std::size_t limit, const char* name) {
    require(total <= limit && amount <= limit - total, std::string(name) + " budget exceeded");
    total += amount;
}
std::string path_text(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::filesystem::path utf8_path(const std::string& text) {
    require(text.find('\0') == std::string::npos, "NUL in asset path");
    return std::filesystem::path(std::u8string(text.begin(),text.end()));
}
std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return value;
}
std::filesystem::path normalized(const std::filesystem::path& path) {
    require(!path.empty(), "Empty asset path");
    return std::filesystem::absolute(path).lexically_normal();
}
std::size_t file_size_checked(const std::filesystem::path& path, std::size_t limit) {
    require(std::filesystem::is_regular_file(path), "File does not exist: " + path_text(path));
    const auto size = std::filesystem::file_size(path);
    require(size <= limit, "File byte budget exceeded: " + path_text(path));
    return std::size_t(size);
}
std::string read_file(const std::filesystem::path& path, std::size_t limit) {
    const auto size = file_size_checked(path, limit);
    std::string data(size, '\0');
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "Cannot open: " + path_text(path));
    if (size) stream.read(data.data(), std::streamsize(size));
    require(bool(stream) && stream.peek() == std::char_traits<char>::eof(), "File changed or read failed: " + path_text(path));
    return data;
}
bool is_color(TextureRole role) {
    switch (role) {
    case TextureRole::base_color: case TextureRole::emissive: return true;
    case TextureRole::metallic_roughness: case TextureRole::normal: case TextureRole::occlusion: return false;
    }
    fail("Invalid texture role");
}
int& texture_slot(Material& material, TextureRole role) {
    switch (role) {
    case TextureRole::base_color: return material.base_texture;
    case TextureRole::metallic_roughness: return material.mr_texture;
    case TextureRole::normal: return material.normal_texture;
    case TextureRole::occlusion: return material.ao_texture;
    case TextureRole::emissive: return material.emissive_texture;
    }
    fail("Invalid texture role");
}
template<class V> bool finite(const V& value) {
    for (glm::length_t i = 0; i < value.length(); ++i) if (!std::isfinite(value[i])) return false;
    return true;
}
std::size_t mip_size(int width, int height) {
    require(width > 0 && height > 0 && width <= Limits::image_dimension && height <= Limits::image_dimension,
            "Invalid image dimensions / dimension budget exceeded");
    require(std::size_t(width) * height <= Limits::base_pixels, "Base texture pixel budget exceeded");
    std::size_t pixels = 0;
    for (;;) {
        add_budget(pixels, std::size_t(width) * height, Limits::mip_pixels, "Texture mip pixels");
        if (width == 1 && height == 1) return pixels;
        width = std::max(1, width / 2); height = std::max(1, height / 2);
    }
}
std::size_t validate_texture(const Texture& texture) {
    require(!texture.levels.empty() && texture.levels.size() <= 15, "Texture has no levels or too many levels");
    int width = texture.levels.front().width, height = texture.levels.front().height;
    (void)mip_size(width, height);
    std::size_t pixels = 0;
    for (std::size_t i = 0; i < texture.levels.size(); ++i) {
        const auto& level = texture.levels[i];
        require(level.width == width && level.height == height && level.pixels.size() == std::size_t(width) * height,
                "Invalid mip dimensions or pixel count");
        add_budget(pixels, level.pixels.size(), Limits::mip_pixels, "Texture mip pixels");
        for (const auto& pixel : level.pixels) require(finite(pixel), "Nonfinite texture pixel");
        if (i + 1 < texture.levels.size()) require(width > 1 || height > 1, "Duplicate 1x1 mip");
        width = std::max(1, width / 2); height = std::max(1, height / 2);
    }
    const auto wrap = [](Texture::Wrap w) {
        return w == Texture::Wrap::repeat || w == Texture::Wrap::clamp_to_edge || w == Texture::Wrap::mirrored_repeat;
    };
    const int filter = int(texture.min_filter);
    require(wrap(texture.wrap_s) && wrap(texture.wrap_t) && (filter == 9728 || filter == 9729 || (filter >= 9984 && filter <= 9987)) &&
            (texture.mag_filter == Texture::Filter::nearest || texture.mag_filter == Texture::Filter::linear), "Invalid texture sampler");
    return pixels;
}
void role_mips(Texture& texture, TextureRole role) {
    texture.srgb = is_color(role);
    if (role == TextureRole::normal) build_normal_mips(texture); else build_mips(texture);
}

struct SceneBudget { std::size_t vertices = 0, indices = 0, pixels = 0, primitives = 0; };
SceneBudget validate_scene(const Scene& scene) {
    for (auto size : {scene.meshes.size(), scene.materials.size(), scene.textures.size(), scene.nodes.size(), scene.lights.size()})
        require(size <= Limits::records, "Scene record budget exceeded");
    SceneBudget budget;
    for (const auto& texture : scene.textures) add_budget(budget.pixels, validate_texture(texture), Limits::mip_pixels, "Scene texture pixels");
    for (const auto& material : scene.materials) {
        require(finite(material.base_color) && finite(material.emissive), "Nonfinite material color");
        for (float value : {material.metallic, material.roughness, material.normal_scale, material.ao_strength, material.clearcoat,
                            material.clearcoat_roughness, material.anisotropy, material.sheen, material.alpha_cutoff})
            require(std::isfinite(value), "Nonfinite material factor");
        require(material.alpha_mode >= 0 && material.alpha_mode <= 2, "Invalid material alpha mode");
        for (int index : {material.base_texture, material.mr_texture, material.normal_texture, material.ao_texture, material.emissive_texture})
            require(index >= -1 && (index < 0 || std::size_t(index) < scene.textures.size()), "Material texture reference out of range");
    }
    auto geometry = [&](const auto& mesh) {
        add_budget(budget.vertices, mesh.vertices.size(), Limits::vertices, "Scene vertices including LODs");
        add_budget(budget.indices, mesh.indices.size(), Limits::indices, "Scene indices including LODs");
        add_budget(budget.primitives, mesh.primitives.size(), Limits::indices / 3, "Scene primitives");
        for (const auto& v : mesh.vertices)
            require(finite(v.position) && finite(v.normal) && finite(v.uv) && finite(v.tangent) && finite(v.color) && finite(v.baked_irradiance), "Nonfinite vertex attribute");
        for (auto index : mesh.indices) require(index < mesh.vertices.size(), "Mesh index out of range");
        const auto count = mesh.indices.empty() ? mesh.vertices.size() : mesh.indices.size();
        require(count % 3 == 0 || !mesh.primitives.empty(), "Incomplete implicit triangle list");
        for (const auto& primitive : mesh.primitives)
            require(primitive.material < scene.materials.size() && primitive.index_count % 3 == 0 &&
                    primitive.first_index <= count && primitive.index_count <= count - primitive.first_index, "Invalid primitive range or material reference");
    };
    for (const auto& mesh : scene.meshes) {
        geometry(mesh); require(mesh.lods.size() <= 8, "LOD level budget exceeded");
        double previous = 0;
        for (const auto& lod : mesh.lods) {
            require(std::isfinite(lod.geometric_error) && lod.geometric_error >= previous, "Invalid LOD error");
            geometry(lod); previous = lod.geometric_error;
        }
    }
    for (const auto& node : scene.nodes) {
        require(node.mesh >= -1 && (node.mesh < 0 || std::size_t(node.mesh) < scene.meshes.size()), "Node mesh reference out of range");
        for (int column = 0; column < 4; ++column)
            require(finite(node.local[column]) && finite(node.previous_world[column]), "Nonfinite node transform");
    }
    // 使用现有迭代层级校验，避免深链递归和导入父节点环。
    const auto worlds = scene_world_transforms(scene);
    for (const auto& world : worlds) for (int column = 0; column < 4; ++column)
        require(finite(world[column]), "World transform overflow");
    for (std::size_t i = 0; i < scene.lights.size(); ++i) {
        const auto& light = scene.lights[i];
        require(light.linked_node >= -1 && (light.linked_node < 0 || std::size_t(light.linked_node) < scene.nodes.size()), "Light linked_node reference out of range");
        require(finite(light.position) && finite(light.direction) && finite(light.color) && finite(light.size) &&
                std::isfinite(light.intensity) && std::isfinite(light.range), "Nonfinite light");
        // 与工程持久化共用关联语义，拒绝非矩形、重复所有权或非法发光面几何。
        validate_light_binding(scene,i);
    }
    return budget;
}
void explicit_instances(Scene& scene) {
    if (!scene.nodes.empty()) return;
    require(scene.meshes.size() <= Limits::records, "Implicit node budget exceeded");
    for (std::size_t i = 0; i < scene.meshes.size(); ++i) {
        Node node; node.name = scene.meshes[i].name; node.mesh = int(i); scene.nodes.push_back(std::move(node));
    }
}
void explicit_materials(Scene& scene) {
    bool geometry = false;
    for (const auto& mesh : scene.meshes) {
        geometry |= !mesh.vertices.empty();
        for (const auto& lod : mesh.lods) geometry |= !lod.vertices.empty();
    }
    if (geometry && scene.materials.empty()) scene.materials.emplace_back();
}
void explicit_primitives(Mesh& mesh) {
    auto add = [](auto& geometry) {
        const auto count = geometry.indices.empty() ? geometry.vertices.size() : geometry.indices.size();
        if (geometry.primitives.empty() && count) geometry.primitives.push_back({0, std::uint32_t(count), 0});
    };
    add(mesh); for (auto& lod : mesh.lods) add(lod);
}

// 分词保留 Windows 路径分隔符；支持引号、转义空格/井号和行尾注释。
std::vector<std::string> tokens(std::string_view line) {
    std::vector<std::string> result;
    std::string word; bool active = false; char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (!quote && c == '#') break;
        if (c == '\\' && i + 1 < line.size()) {
            const char next = line[i + 1];
            if (next == ' ' || next == '\t' || next == '#' || next == '"' || next == '\'') {
                word += next; ++i; active = true; continue;
            }
        }
        if (quote) { if (c == quote) quote = 0; else word += c; active = true; continue; }
        if ((c == '"' || c == '\'') && !active) { quote = c; active = true; continue; }
        if (c == ' ' || c == '\t' || c == '\r') {
            if (active) { result.push_back(std::move(word)); word.clear(); active = false; }
        } else { word += c; active = true; }
    }
    require(!quote, "Unterminated quoted token");
    if (active) result.push_back(std::move(word));
    require(result.size() <= 4096, "Line token budget exceeded");
    return result;
}
std::string joined(const std::vector<std::string>& words, std::size_t start = 1) {
    require(start < words.size(), "Missing name or path");
    std::string result;
    for (std::size_t i = start; i < words.size(); ++i) { if (i > start) result += ' '; result += words[i]; }
    require(!result.empty() && result.size() <= 4096, "Empty or oversized name/path");
    return result;
}
double number(const std::string& text) {
    std::string_view value(text); if (!value.empty() && value.front() == '+') value.remove_prefix(1);
    double result = 0; const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(!value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && std::isfinite(result) &&
            std::abs(result) <= 1e12, "Invalid or unbounded number: " + text);
    return result;
}
bool numeric(const std::string& text) {
    if (text.empty()) return false;
    std::string_view value(text); if (value.front() == '+') value.remove_prefix(1);
    double result = 0; const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return !value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
std::int64_t integer(const std::string& text) {
    std::string_view value(text); if (!value.empty() && value.front() == '+') value.remove_prefix(1);
    std::int64_t result = 0; const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(!value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(), "Invalid integer: " + text);
    return result;
}
template<class F> void lines(const std::filesystem::path& path, const std::string& data, F&& consume) {
    require(data.find('\0') == std::string::npos, "NUL in text file: " + path_text(path));
    std::istringstream input(data); std::string physical, logical; std::size_t line_number = 0, first = 1;
    while (std::getline(input, physical)) {
        ++line_number;
        if (line_number == 1 && physical.starts_with("\xef\xbb\xbf")) physical.erase(0, 3);
        if (logical.empty()) first = line_number;
        if (!physical.empty() && physical.back() == '\r') physical.pop_back();
        const bool continued = !physical.empty() && physical.back() == '\\';
        if (continued) physical.pop_back();
        require(physical.size() <= 65536 && logical.size() + physical.size() + 1 <= 65536, "Text line byte budget exceeded");
        logical += physical;
        if (continued) { logical += ' '; continue; }
        try { auto words = tokens(logical); if (!words.empty()) consume(words); }
        catch (const std::exception& e) { fail(path_text(path) + ":" + std::to_string(first) + ": " + e.what()); }
        logical.clear();
    }
    require(logical.empty(), "Unfinished line continuation: " + path_text(path));
}

struct MapSpec {
    std::filesystem::path path;
    bool clamp = false;
    float bump_scale = 1;
    char channel = 'r';
};
MapSpec map_spec(const std::vector<std::string>& words, const std::filesystem::path& directory, bool bump, bool scalar) {
    MapSpec map; std::size_t i = 1;
    while (i < words.size() && words[i].starts_with('-')) {
        const auto option = words[i++];
        auto argument = [&]() -> const std::string& { require(i < words.size(), "Missing map option argument: " + option); return words[i++]; };
        if (option == "-clamp") {
            const auto& value = argument(); require(value == "on" || value == "off", "Invalid -clamp"); map.clamp = value == "on";
        } else if (option == "-bm") {
            require(bump, "-bm only applies to bump/normal maps"); map.bump_scale = float(number(argument()));
            require(std::abs(map.bump_scale) <= 100, "Bump scale budget exceeded");
        } else if (option == "-imfchan") {
            require(scalar, "Channel selection is only representable for scalar maps"); const auto& value = argument();
            require(value.size() == 1 && std::string_view("rgbalm").find(value[0]) != std::string_view::npos, "Unsupported -imfchan"); map.channel = value[0];
        } else if (option == "-o" || option == "-s" || option == "-t") {
            const double identity = option == "-s" ? 1 : 0; std::size_t count = 0;
            while (i < words.size() && numeric(words[i]) && count < 3) {
                require(number(words[i++]) == identity, "Non-identity MTL UV transform is not representable"); ++count;
            }
            require(count != 0, "Map transform needs numeric arguments");
        } else if (option == "-mm") {
            const auto base = number(argument()), gain = number(argument());
            require(base == 0 && gain == 1, "Non-default MTL map value remapping is not representable");
        } else if (option == "-blendu" || option == "-blendv") {
            require(argument() == "on", "Disabled MTL map blending is not representable");
        } else if (option == "-cc") {
            require(argument() == "off", "MTL color correction is not representable");
        } else if (option == "-boost") {
            require(number(argument()) == 1, "MTL map boost is not representable");
        } else fail("Unsupported MTL map option: " + option);
    }
    map.path = normalized(directory / utf8_path(joined(words, i)));
    return map;
}
float channel(const glm::vec4& pixel, char selector) {
    switch (selector) {
    case 'r': return pixel.r; case 'g': return pixel.g; case 'b': return pixel.b;
    case 'a': case 'm': return pixel.a;
    case 'l': return glm::dot(glm::vec3(pixel), glm::vec3(.2126f, .7152f, .0722f));
    }
    fail("Invalid map channel");
}
struct MtlMaterial {
    Material value;
    bool defined = false, emissive_factor = false, metallic_factor = false, roughness_factor = false;
    std::optional<MapSpec> base, emissive, alpha, normal, bump, roughness, metallic, ao, packed;
    bool orm = false;
};
struct Corner { std::size_t position = 0; int uv = -1, normal = -1; };
struct SmoothKey {
    std::size_t position; std::int64_t group;
    bool operator==(const SmoothKey&) const = default;
};
struct SmoothHash {
    std::size_t operator()(const SmoothKey& key) const noexcept {
        return std::hash<std::size_t>{}(key.position) ^ (std::hash<std::int64_t>{}(key.group) << 1);
    }
};
struct MissingNormal { std::size_t mesh, vertex; SmoothKey key; };
struct ObjTangentCorner {
    std::size_t position;
    // 显式法线为 0，平滑组为正，未平滑的原多边形编号为负。
    std::int64_t domain;
    bool has_uv;
};
struct ObjTangentKey {
    std::size_t position;
    std::int64_t domain;
    std::array<float,5> normal_uv;
    int handedness;
    bool has_uv;
    bool operator==(const ObjTangentKey&) const = default;
};
struct ObjTangentHash {
    std::size_t operator()(const ObjTangentKey& key) const noexcept {
        auto hash = std::hash<std::size_t>{}(key.position);
        const auto combine = [&](std::size_t value) { hash ^= value + std::size_t(0x9e3779b9u) + (hash << 6) + (hash >> 2); };
        combine(std::hash<std::int64_t>{}(key.domain));
        for (float value : key.normal_uv) combine(std::hash<float>{}(value));
        combine(std::hash<int>{}(key.handedness)); combine(std::hash<bool>{}(key.has_uv));
        return hash;
    }
};
struct ObjLoader {
    LoadedSceneAsset result;
    std::vector<glm::vec3> positions, normals;
    std::vector<glm::vec2> uvs;
    std::vector<glm::vec4> colors;
    std::vector<MtlMaterial> materials;
    std::map<std::string, std::size_t> material_names;
    std::set<std::filesystem::path> libraries, dependencies;
    std::set<std::string> warning_set;
    std::unordered_map<SmoothKey, glm::dvec3, SmoothHash> smooth;
    std::vector<MissingNormal> missing;
    std::vector<std::vector<bool>> primitive_uvs;
    std::vector<std::vector<ObjTangentCorner>> tangent_corners;
    std::size_t text_bytes = 0, dependency_bytes = 0, vertices = 0, indices = 0, pixels = 0, work = 0;
    std::size_t face_serial = 0;
    std::size_t active_material = 0;
    std::int64_t smoothing = 0;
    std::string object_name;
    bool new_mesh = true;
    void warning(const std::string& text) {
        if (warning_set.insert(text).second) {
            require(result.warnings.size() < 256, "Import warning budget exceeded"); result.warnings.push_back(text);
        }
    }
    std::string text_file(const std::filesystem::path& path) {
        const auto size = file_size_checked(path, Limits::obj_text_bytes);
        add_budget(text_bytes, size, Limits::obj_text_bytes, "OBJ + MTL text bytes");
        return read_file(path, size);
    }
    std::size_t material_id(const std::string& name) {
        if (const auto found = material_names.find(name); found != material_names.end()) return found->second;
        require(materials.size() < Limits::records, "MTL material budget exceeded");
        MtlMaterial material; material.value.name = name; material.value.base_color = glm::vec4(1);
        const auto id = materials.size(); materials.push_back(std::move(material)); material_names.emplace(name, id); return id;
    }
    void read_mtl(const std::filesystem::path& path) {
        const auto canonical = std::filesystem::weakly_canonical(path);
        if (!libraries.insert(canonical).second) return;
        require(libraries.size() <= 64, "MTL library budget exceeded");
        std::optional<std::size_t> current;
        lines(path, text_file(path), [&](const auto& words) {
            const auto& command = words[0];
            if (command == "newmtl") {
                const auto id = material_id(joined(words)); require(!materials[id].defined, "Duplicate MTL material: " + joined(words));
                materials[id].defined = true; current = id; return;
            }
            require(current.has_value(), "MTL property before newmtl"); auto& m = materials[*current];
            auto scalar = [&]() { require(words.size() == 2, "MTL scalar requires one value"); return float(number(words[1])); };
            auto color = [&]() {
                require(words.size() == 4, "Only RGB MTL colors are representable");
                glm::vec3 value; for (int k = 0; k < 3; ++k) { value[k] = float(number(words[std::size_t(k) + 1])); require(value[k] >= 0, "Negative MTL color"); }
                return value;
            };
            if (command == "Kd") { const auto c = color(); require(glm::all(glm::lessThanEqual(c, glm::vec3(1))), "Kd exceeds normalized color range"); m.value.base_color = glm::vec4(c, m.value.base_color.a); }
            else if (command == "Ke") { m.value.emissive = color(); m.emissive_factor = true; }
            else if (command == "d" || command == "Tr") { float opacity = scalar(); require(opacity >= 0 && opacity <= 1, "Invalid MTL opacity"); if (command == "Tr") opacity = 1 - opacity; m.value.base_color.a = opacity; }
            else if (command == "Ns") { const float ns = scalar(); require(ns >= 0, "Negative specular exponent"); if (!m.roughness_factor) m.value.roughness = std::sqrt(2.f / (ns + 2.f)); warning("MTL Ns is approximated as perceptual PBR roughness; the original specular BRDF is not reproduced."); }
            else if (command == "Pr" || command == "Pm") { const float value = scalar(); require(value >= 0 && value <= 1, "PBR MTL factor must be in [0,1]"); if (command == "Pr") { m.value.roughness = value; m.roughness_factor = true; } else { m.value.metallic = value; m.metallic_factor = true; } }
            else if (command == "map_Kd") m.base = map_spec(words, path.parent_path(), false, false);
            else if (command == "map_Ke") m.emissive = map_spec(words, path.parent_path(), false, false);
            else if (command == "map_d") m.alpha = map_spec(words, path.parent_path(), false, true);
            else if (command == "norm") m.normal = map_spec(words, path.parent_path(), true, false);
            else if (command == "bump" || command == "map_bump" || command == "map_Bump") m.bump = map_spec(words, path.parent_path(), true, true);
            else if (command == "map_Pr") m.roughness = map_spec(words, path.parent_path(), false, true);
            else if (command == "map_Pm") m.metallic = map_spec(words, path.parent_path(), false, true);
            else if (command == "map_ao" || command == "map_AO") m.ao = map_spec(words, path.parent_path(), false, true);
            else if (command == "map_ORM" || command == "map_metallic_roughness") { m.packed = map_spec(words, path.parent_path(), false, false); m.orm = command == "map_ORM"; }
            else if (command == "Ka" || command == "Ks" || command == "Tf") { (void)color(); warning("MTL " + command + " is not represented by the editor PBR material."); }
            else if (command == "Ni" || command == "illum") { (void)scalar(); warning("MTL " + command + " is not represented by the static opaque/PBR importer."); }
            else warning("Unsupported MTL directive ignored: " + command);
        });
    }
    std::size_t index(const std::string& text, std::size_t count) {
        const auto value = integer(text);
        require(value != 0 && value >= -std::int64_t(count) && value <= std::int64_t(count), "OBJ index out of range: " + text);
        return std::size_t(value > 0 ? value - 1 : std::int64_t(count) + value);
    }
    Corner corner(const std::string& text) {
        const auto slash = text.find('/'); Corner c;
        c.position = index(text.substr(0, slash), positions.size());
        if (slash == std::string::npos) return c;
        const auto second = text.find('/', slash + 1);
        if (second == std::string::npos) { require(slash + 1 < text.size(), "Missing OBJ UV index"); c.uv = int(index(text.substr(slash + 1), uvs.size())); return c; }
        require(text.find('/', second + 1) == std::string::npos && second + 1 < text.size(), "Invalid OBJ face corner");
        if (second > slash + 1) c.uv = int(index(text.substr(slash + 1, second - slash - 1), uvs.size()));
        c.normal = int(index(text.substr(second + 1), normals.size())); return c;
    }
    void step() { add_budget(work, 1, 64 * 1024 * 1024, "Polygon triangulation work"); }
    std::vector<std::array<std::size_t, 3>> triangulate(const std::vector<Corner>& face, glm::vec3& face_normal) {
        const auto origin = glm::dvec3(positions[face[0].position]); glm::dvec3 area(0), min(0), max(0);
        for (std::size_t i = 0; i < face.size(); ++i) {
            const auto p = glm::dvec3(positions[face[i].position]) - origin;
            const auto q = glm::dvec3(positions[face[(i + 1) % face.size()].position]) - origin;
            area += glm::cross(p, q); min = glm::min(min, p); max = glm::max(max, p);
        }
        const double scale = glm::length(max - min), epsilon = scale * scale * 1e-12;
        // 实际导出包常夹带顶点重合或共线的零面积三角形；它们不贡献画面。
        // 只跳过这种三角形并告知用户，自相交/退化的多边形仍拒绝，不能任意扇形填补。
        if (face.size() == 3 && !(scale > 0 && glm::length(area) > epsilon)) {
            warning("Zero-area OBJ triangles were skipped; usable surfaces were retained.");
            return {};
        }
        require(scale > 0 && glm::length(area) > epsilon, "Degenerate or self-intersecting OBJ polygon");
        const auto normal = glm::normalize(area); face_normal = glm::vec3(normal);
        int axis = 0; if (std::abs(normal.y) > std::abs(normal[axis])) axis = 1; if (std::abs(normal.z) > std::abs(normal[axis])) axis = 2;
        std::vector<glm::dvec2> points;
        for (const auto& c : face) {
            const auto p = glm::dvec3(positions[c.position]) - origin;
            require(std::abs(glm::dot(normal, p)) <= scale * 1e-5, "Nonplanar OBJ polygon is unsupported");
            points.emplace_back(p[(axis + 1) % 3], p[(axis + 2) % 3]);
        }
        auto cross = [&](std::size_t a, std::size_t b, std::size_t c) { const auto p = points[b] - points[a], q = points[c] - points[a]; return p.x * q.y - p.y * q.x; };
        auto on = [&](std::size_t a, std::size_t b, std::size_t c) {
            return std::abs(cross(a,b,c)) <= epsilon && glm::dot(points[c] - points[a], points[c] - points[b]) <= epsilon;
        };
        for (std::size_t a = 0; a < face.size(); ++a) {
            const auto b = (a + 1) % face.size(); require(glm::length(points[a] - points[b]) > scale * 1e-12, "Repeated OBJ polygon corner");
            for (std::size_t c = a + 1; c < face.size(); ++c) {
                const auto d = (c + 1) % face.size(); if (b == c || d == a) continue; step();
                const double ab_c = cross(a,b,c), ab_d = cross(a,b,d), cd_a = cross(c,d,a), cd_b = cross(c,d,b);
                const bool crossing = ((ab_c > epsilon && ab_d < -epsilon) || (ab_c < -epsilon && ab_d > epsilon)) &&
                                      ((cd_a > epsilon && cd_b < -epsilon) || (cd_a < -epsilon && cd_b > epsilon));
                require(!crossing && !on(a,b,c) && !on(a,b,d) && !on(c,d,a) && !on(c,d,b), "Self-intersecting OBJ polygon");
            }
        }
        double signed_area = 0;
        for (std::size_t i = 0; i < points.size(); ++i) { const auto& p = points[i]; const auto& q = points[(i + 1) % points.size()]; signed_area += p.x * q.y - p.y * q.x; }
        const double sign = signed_area > 0 ? 1 : -1;
        std::vector<std::size_t> ring(face.size()); std::iota(ring.begin(), ring.end(), 0);
        std::vector<std::array<std::size_t,3>> triangles;
        // 耳切而非扇形：凹面不能跨越凹口；所有检查共享有限工作预算。
        while (ring.size() > 3) {
            bool found = false;
            for (std::size_t i = 0; i < ring.size(); ++i) {
                const auto a = ring[(i + ring.size() - 1) % ring.size()], b = ring[i], c = ring[(i + 1) % ring.size()]; step();
                if (sign * cross(a,b,c) <= epsilon) continue;
                bool inside = false;
                for (auto p : ring) {
                    if (p == a || p == b || p == c) continue; step();
                    if (sign * cross(a,b,p) >= -epsilon && sign * cross(b,c,p) >= -epsilon && sign * cross(c,a,p) >= -epsilon) { inside = true; break; }
                }
                if (inside) continue;
                triangles.push_back({a,b,c}); ring.erase(ring.begin() + std::ptrdiff_t(i)); found = true; break;
            }
            require(found, "Cannot triangulate degenerate OBJ polygon");
        }
        require(sign * cross(ring[0],ring[1],ring[2]) > epsilon, "Degenerate final OBJ triangle");
        triangles.push_back({ring[0],ring[1],ring[2]}); return triangles;
    }
    void face(const std::vector<std::string>& words) {
        require(words.size() >= 4 && words.size() - 1 <= Limits::face_corners, "OBJ face corner budget / minimum exceeded");
        std::vector<Corner> corners; for (std::size_t i = 1; i < words.size(); ++i) corners.push_back(corner(words[i]));
        glm::vec3 face_normal; const auto triangles = triangulate(corners, face_normal);
        if (triangles.empty()) return; // 不为已跳过的面创建空网格、材质 Primitive 或平滑法线记录。
        add_budget(vertices, triangles.size() * 3, Limits::vertices, "Expanded OBJ vertices");
        add_budget(indices, triangles.size() * 3, Limits::indices, "OBJ indices");
        const auto flat_domain = -std::int64_t(++face_serial);
        if (new_mesh) {
            require(result.scene.meshes.size() < Limits::records, "OBJ mesh budget exceeded");
            Mesh mesh; mesh.name = object_name; result.scene.meshes.push_back(std::move(mesh));
            primitive_uvs.emplace_back(); tangent_corners.emplace_back(); new_mesh = false;
        }
        auto& mesh = result.scene.meshes.back(); auto& has_uv = primitive_uvs.back();
        if (mesh.primitives.empty() || mesh.primitives.back().material != active_material) {
            mesh.primitives.push_back({std::uint32_t(mesh.indices.size()), 0, std::uint32_t(active_material)}); has_uv.push_back(true);
        }
        for (const auto& triangle : triangles) {
            const auto pa = glm::dvec3(positions[corners[triangle[0]].position]), pb = glm::dvec3(positions[corners[triangle[1]].position]), pc = glm::dvec3(positions[corners[triangle[2]].position]);
            const auto weighted = glm::cross(pb - pa, pc - pa);
            for (auto id : triangle) {
                const auto& c = corners[id]; Vertex vertex; vertex.position = positions[c.position]; vertex.color = colors[c.position];
                vertex.normal = c.normal >= 0 ? normals[std::size_t(c.normal)] : face_normal;
                // OBJ 的 V=0 位于图像底部；stb 保持首行在顶部，转换 UV 后无需全局 flip 状态。
                if (c.uv >= 0) vertex.uv = {uvs[std::size_t(c.uv)].x, 1 - uvs[std::size_t(c.uv)].y}; else has_uv.back() = false;
                if (smoothing > 0) {
                    const SmoothKey key{c.position, smoothing};
                    const auto [entry, inserted] = smooth.try_emplace(key, glm::dvec3(0)); (void)inserted; entry->second += weighted;
                    if (c.normal < 0) missing.push_back({result.scene.meshes.size() - 1, mesh.vertices.size(), key});
                }
                tangent_corners.back().push_back({c.position, c.normal >= 0 ? 0 : smoothing > 0 ? smoothing : flat_domain, c.uv >= 0});
                mesh.indices.push_back(std::uint32_t(mesh.vertices.size())); mesh.vertices.push_back(vertex); ++mesh.primitives.back().index_count;
            }
        }
    }
    void obj_line(const std::vector<std::string>& words, const std::filesystem::path& path) {
        const auto& command = words[0];
        if (command == "v") {
            require(words.size() == 4 || words.size() == 5 || words.size() == 7 || words.size() == 8, "OBJ v requires xyz, xyzw or xyz RGB[A]");
            require(positions.size() < Limits::vertices, "OBJ position budget exceeded");
            glm::dvec3 p; for (int k = 0; k < 3; ++k) p[k] = number(words[std::size_t(k) + 1]);
            if (words.size() == 5) { const auto w = number(words[4]); require(w != 0, "Zero homogeneous OBJ weight"); p /= w; }
            require(finite(p) && glm::all(glm::lessThanEqual(glm::abs(p), glm::dvec3(1e8))), "OBJ position magnitude budget exceeded");
            glm::vec4 color(1);
            if (words.size() >= 7) for (std::size_t k = 4; k < words.size(); ++k) { const auto value = number(words[k]); require(value >= 0 && value <= 1, "OBJ color must be in [0,1]"); color[glm::length_t(k - 4)] = float(value); }
            positions.push_back(glm::vec3(p)); colors.push_back(color);
        } else if (command == "vt") {
            require(words.size() >= 2 && words.size() <= 4, "OBJ vt requires 1..3 values"); require(uvs.size() < Limits::vertices, "OBJ UV budget exceeded");
            const float u = float(number(words[1])), v = words.size() > 2 ? float(number(words[2])) : 0;
            require(std::abs(u) <= 1e6f && std::abs(v) <= 1e6f, "OBJ UV magnitude budget exceeded");
            if (words.size() == 4) require(number(words[3]) == 0, "3D OBJ texture coordinates are unsupported"); uvs.emplace_back(u,v);
        } else if (command == "vn") {
            require(words.size() == 4, "OBJ vn requires xyz"); require(normals.size() < Limits::vertices, "OBJ normal budget exceeded");
            glm::dvec3 normal; for (int k = 0; k < 3; ++k) normal[k] = number(words[std::size_t(k) + 1]);
            require(glm::length(normal) > 1e-20, "Zero OBJ normal"); normals.push_back(glm::vec3(glm::normalize(normal)));
        } else if (command == "f") face(words);
        else if (command == "o" || command == "g") { object_name = words.size() > 1 ? joined(words) : result.scene.name; new_mesh = true; }
        else if (command == "s") { require(words.size() == 2, "Invalid OBJ smoothing directive"); smoothing = words[1] == "off" ? 0 : words[1] == "on" ? 1 : integer(words[1]); require(smoothing >= 0, "Invalid smoothing group"); }
        else if (command == "usemtl") active_material = material_id(joined(words));
        else if (command == "mtllib") {
            const auto combined = normalized(path.parent_path() / utf8_path(joined(words)));
            if (words.size() > 2 && std::filesystem::is_regular_file(combined)) read_mtl(combined);
            else for (std::size_t i = 1; i < words.size(); ++i) read_mtl(normalized(path.parent_path() / utf8_path(words[i])));
        } else if (command == "vp" || command == "curv" || command == "curv2" || command == "surf" || command == "cstype" || command == "deg" || command == "parm" || command == "trim" || command == "hole" || command == "end")
            fail("OBJ freeform geometry is unsupported: " + command);
        else if (command == "l" || command == "p") warning("OBJ point/line elements are ignored; only polygon surfaces are imported.");
        else warning("Unsupported OBJ directive ignored: " + command);
    }
    Texture load_map(const MapSpec& map, TextureRole role) {
        const auto canonical = std::filesystem::weakly_canonical(map.path);
        if (dependencies.insert(canonical).second)
            add_budget(dependency_bytes, file_size_checked(map.path, Limits::file_bytes), Limits::dependency_bytes, "OBJ encoded texture dependencies");
        auto texture = load_editor_texture(map.path, role);
        if (map.clamp) texture.wrap_s = texture.wrap_t = Texture::Wrap::clamp_to_edge;
        return texture;
    }
    int store(Texture texture) {
        require(result.scene.textures.size() < Limits::records, "OBJ texture record budget exceeded");
        add_budget(pixels, validate_texture(texture), Limits::mip_pixels, "OBJ texture mip pixels");
        const int index = int(result.scene.textures.size()); TextureSampler sampler;
        sampler.wrap_s = int(texture.wrap_s); sampler.wrap_t = int(texture.wrap_t);
        sampler.min_filter = int(texture.min_filter); sampler.mag_filter = int(texture.mag_filter);
        result.scene.textures.push_back(std::move(texture)); result.samplers.push_back(sampler); return index;
    }
    void same_layout(const Texture& a, const Texture& b) {
        require(a.levels[0].width == b.levels[0].width && a.levels[0].height == b.levels[0].height && a.wrap_s == b.wrap_s && a.wrap_t == b.wrap_t,
                "Combined MTL maps need matching dimensions and wrap modes");
    }
    void realize_materials() {
        for (auto& source : materials) {
            require(source.defined, "Undefined OBJ material: " + source.value.name); auto& m = source.value;
            require(!(source.normal && source.bump), "MTL has both normal and height bump maps; layering is unsupported");
            require(!(source.packed && (source.roughness || source.metallic)), "MTL packed and separate MR maps conflict");
            if (source.base || source.alpha) {
                Texture color;
                if (source.base) color = load_map(*source.base, TextureRole::base_color);
                if (source.alpha) {
                    auto alpha = load_map(*source.alpha, TextureRole::occlusion);
                    if (source.base) same_layout(color, alpha);
                    else { color = alpha; color.name += " (opacity)"; for (auto& pixel : color.levels[0].pixels) pixel = glm::vec4(1); }
                    for (std::size_t i = 0; i < color.levels[0].pixels.size(); ++i) color.levels[0].pixels[i].a *= channel(alpha.levels[0].pixels[i], source.alpha->channel);
                    role_mips(color, TextureRole::base_color); m.alpha_mode = 2;
                }
                m.base_texture = store(std::move(color));
            }
            if (source.emissive) { m.emissive_texture = store(load_map(*source.emissive, TextureRole::emissive)); if (!source.emissive_factor) m.emissive = glm::vec3(1); }
            if (source.normal) { m.normal_texture = store(load_map(*source.normal, TextureRole::normal)); m.normal_scale = source.normal->bump_scale; }
            if (source.bump) {
                auto texture = load_map(*source.bump, TextureRole::occlusion); auto& image = texture.levels[0];
                Image<glm::vec4> normal(image.width, image.height);
                auto height = [&](int x, int y) {
                    if (source.bump->clamp) { x = std::clamp(x,0,image.width-1); y = std::clamp(y,0,image.height-1); }
                    else { x = (x + image.width) % image.width; y = (y + image.height) % image.height; }
                    return channel(image.at(x,y), source.bump->channel);
                };
                for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) {
                    const float dx = (height(x+1,y) - height(x-1,y)) * .5f * image.width;
                    const float dy = (height(x,y+1) - height(x,y-1)) * .5f * image.height;
                    const auto n = safe_normalize(glm::vec3(-dx * source.bump->bump_scale, -dy * source.bump->bump_scale, 1), {0,0,1});
                    normal.at(x,y) = glm::vec4(n * .5f + .5f, 1);
                }
                image = std::move(normal); texture.name += " (height to normal)"; role_mips(texture, TextureRole::normal); m.normal_texture = store(std::move(texture));
                warning("MTL bump is a scalar height map converted to tangent normals using central UV differences; it is not treated as an RGB normal map.");
            }
            if (source.packed) {
                m.mr_texture = store(load_map(*source.packed, TextureRole::metallic_roughness)); if (source.orm) m.ao_texture = m.mr_texture;
                if (!source.metallic_factor) m.metallic = 1; if (!source.roughness_factor) m.roughness = 1;
            } else if (source.roughness || source.metallic) {
                std::optional<Texture> rough, metal;
                if (source.roughness) rough = load_map(*source.roughness, TextureRole::metallic_roughness);
                if (source.metallic) metal = load_map(*source.metallic, TextureRole::metallic_roughness);
                if (rough && metal) same_layout(*rough,*metal);
                Texture packed = rough ? *rough : *metal; packed.name += " (packed MR)";
                for (std::size_t i = 0; i < packed.levels[0].pixels.size(); ++i)
                    packed.levels[0].pixels[i] = {1, rough ? channel(rough->levels[0].pixels[i], source.roughness->channel) : 1,
                                                 metal ? channel(metal->levels[0].pixels[i], source.metallic->channel) : 1, 1};
                role_mips(packed, TextureRole::metallic_roughness); m.mr_texture = store(std::move(packed));
                if (metal && !source.metallic_factor) m.metallic = 1; if (rough && !source.roughness_factor) m.roughness = 1;
            }
            if (source.ao) {
                auto texture = load_map(*source.ao, TextureRole::occlusion);
                for (auto& pixel : texture.levels[0].pixels) pixel.r = channel(pixel, source.ao->channel);
                role_mips(texture, TextureRole::occlusion); m.ao_texture = store(std::move(texture));
            }
            if (m.base_color.a < 1) m.alpha_mode = 2;
            result.scene.materials.push_back(std::move(m));
        }
    }
    void finish_geometry() {
        require(!result.scene.meshes.empty(), "OBJ contains no polygon surfaces");
        for (const auto& entry : missing) {
            const auto n = smooth.at(entry.key); require(glm::length(n) > 1e-20, "Cancelling OBJ smoothing normals");
            result.scene.meshes[entry.mesh].vertices[entry.vertex].normal = glm::vec3(glm::normalize(n));
        }
        // OBJ 顶点按面角展开；按来源/最终法线/UV 聚合，而不改写索引、材质范围或网格数量。
        // 分组跨 usemtl/o/g，数量不超过已检查预算的展开顶点数；不合并不同源位置的重合点。
        std::unordered_map<ObjTangentKey, glm::dvec3, ObjTangentHash> sums;
        sums.reserve(vertices);
        const auto key = [&](std::size_t mesh_id, std::size_t vertex_id, int handedness) {
            const auto& corner = tangent_corners[mesh_id][vertex_id];
            const auto& v = result.scene.meshes[mesh_id].vertices[vertex_id];
            return ObjTangentKey{corner.position, corner.domain, {v.normal.x,v.normal.y,v.normal.z,v.uv.x,v.uv.y}, handedness, corner.has_uv};
        };
        for (std::size_t id = 0; id < result.scene.meshes.size(); ++id) {
            auto& mesh = result.scene.meshes[id];
            for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
                const auto& material = result.scene.materials.at(mesh.primitives[p].material);
                const bool textured = material.base_texture >= 0 || material.mr_texture >= 0 || material.normal_texture >= 0 || material.ao_texture >= 0 || material.emissive_texture >= 0;
                require(!textured || primitive_uvs[id][p], "Textured OBJ face has missing UVs: " + mesh.name);
            }
            for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
                const std::array<std::uint32_t,3> ids{mesh.indices[i],mesh.indices[i+1],mesh.indices[i+2]};
                const auto& a = mesh.vertices[ids[0]]; const auto& b = mesh.vertices[ids[1]]; const auto& c = mesh.vertices[ids[2]];
                const std::array<glm::dvec3,3> p{glm::dvec3(a.position),glm::dvec3(b.position),glm::dvec3(c.position)};
                const auto e1 = p[1]-p[0], e2 = p[2]-p[0];
                const auto d1 = glm::dvec2(b.uv)-glm::dvec2(a.uv), d2 = glm::dvec2(c.uv)-glm::dvec2(a.uv);
                const double determinant = d1.x*d2.y - d1.y*d2.x;
                const double uv_scale = std::sqrt(glm::dot(d1,d1)*glm::dot(d2,d2));
                const bool usable = tangent_corners[id][ids[0]].has_uv && tangent_corners[id][ids[1]].has_uv && tangent_corners[id][ids[2]].has_uv &&
                                    std::abs(determinant) > 8*std::numeric_limits<double>::epsilon()*uv_scale;
                glm::dvec3 t(0), bitangent(0);
                if (usable) { t = glm::normalize((e1*d2.y-e2*d1.y)/determinant); bitangent = (e2*d1.x-e1*d2.x)/determinant; }
                for (int k = 0; k < 3; ++k) {
                    auto& v = mesh.vertices[ids[k]];
                    const int handedness = usable ? (glm::dot(glm::cross(glm::dvec3(v.normal),t),bitangent) < 0 ? -1 : 1) : 0;
                    v.tangent.w = float(handedness); // 第二遍恢复正负号；0 隔离缺失/退化 UV 的回退角。
                    if (!usable) continue;
                    const auto first = p[(k+1)%3]-p[k], second = p[(k+2)%3]-p[k];
                    const double angle = std::atan2(glm::length(glm::cross(first,second)),glm::dot(first,second));
                    sums.try_emplace(key(id,ids[k],handedness),glm::dvec3(0)).first->second += t*angle;
                }
            }
        }
        for (std::size_t id = 0; id < result.scene.meshes.size(); ++id) {
            auto& mesh = result.scene.meshes[id];
            for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
                auto& v = mesh.vertices[i]; const glm::dvec3 n(v.normal);
                const int handedness = int(v.tangent.w);
                const auto t = handedness ? sums.at(key(id,i,handedness)) : glm::dvec3(0);
                auto projected = glm::cross(n,glm::cross(t,n));
                if (!(glm::dot(projected,projected) > glm::dot(t,t)*1e-24))
                    projected = glm::cross(std::abs(n.y) < .9 ? glm::dvec3(0,1,0) : glm::dvec3(1,0,0),n);
                v.tangent = glm::vec4(glm::vec3(glm::normalize(projected)),handedness < 0 ? -1.f : 1.f);
            }
            Node node; node.name = mesh.name; node.mesh = int(id); result.scene.nodes.push_back(std::move(node));
        }
        warning("OBJ tangents average angle-weighted UV derivatives across matching smooth corners; UV seams, hard normals and handedness remain separate (not MikkTSpace).");
    }
    LoadedSceneAsset load(const std::filesystem::path& path) {
        result.scene.name = path_text(path.stem()); object_name = result.scene.name;
        MtlMaterial fallback; fallback.defined = true; fallback.value.name = "OBJ default material"; materials.push_back(std::move(fallback));
        lines(path, text_file(path), [&](const auto& words) { obj_line(words,path); });
        realize_materials(); finish_geometry(); (void)validate_scene(result.scene); return std::move(result);
    }
};

void preflight_gltf(const std::filesystem::path& path) {
    // 先只解析元数据，检查外部依赖与解码预算，再委托原加载器。
    // 仅检查主文件大小不足以阻止一个很小的 glTF 引用巨大的外部 buffer/image。
    fastgltf::GltfDataBuffer data;
    require(data.loadFromFile(path), "Cannot inspect glTF: " + path_text(path));
    const auto extensions = fastgltf::Extensions::KHR_mesh_quantization | fastgltf::Extensions::KHR_materials_emissive_strength |
                            fastgltf::Extensions::KHR_lights_punctual | fastgltf::Extensions::KHR_materials_clearcoat |
                            fastgltf::Extensions::KHR_texture_transform;
    fastgltf::Parser parser(extensions);
    const auto type = fastgltf::determineGltfFileType(&data);
    require(type == fastgltf::GltfType::glTF || type == fastgltf::GltfType::GLB, "Not a glTF/GLB asset");
    auto parsed = type == fastgltf::GltfType::glTF ? parser.loadGLTF(&data,path.parent_path(),fastgltf::Options::LoadGLBBuffers)
                                                : parser.loadBinaryGLTF(&data,path.parent_path(),fastgltf::Options::LoadGLBBuffers);
    require(parsed.error() == fastgltf::Error::None, "glTF preflight parse failed: " + std::string(fastgltf::getErrorMessage(parsed.error())));
    auto asset = std::move(parsed.get());
    for (auto count : {asset.buffers.size(),asset.bufferViews.size(),asset.accessors.size(),asset.images.size(),asset.textures.size(),
                       asset.materials.size(),asset.meshes.size(),asset.nodes.size(),asset.lights.size()})
        require(count <= Limits::records, "glTF metadata record budget exceeded");
    std::size_t encoded_bytes = 0;
    std::map<std::filesystem::path,std::string> external;
    auto bytes = [&](const fastgltf::DataSource& source) -> std::span<const unsigned char> {
        if (const auto* value = std::get_if<fastgltf::sources::Vector>(&source)) {
            add_budget(encoded_bytes,value->bytes.size(),Limits::dependency_bytes,"glTF encoded dependency bytes");
            return {value->bytes.data(),value->bytes.size()};
        }
        if (const auto* value = std::get_if<fastgltf::sources::ByteView>(&source)) {
            add_budget(encoded_bytes,value->bytes.size(),Limits::dependency_bytes,"glTF encoded dependency bytes");
            return {reinterpret_cast<const unsigned char*>(value->bytes.data()),value->bytes.size()};
        }
        if (const auto* value = std::get_if<fastgltf::sources::URI>(&source)) {
            require(value->uri.valid() && value->uri.isLocalPath() && value->fileByteOffset == 0, "Only local glTF dependency files are supported");
            const auto dependency = normalized(path.parent_path() / utf8_path(std::string(value->uri.path())));
            auto found = external.find(dependency);
            const auto size = found == external.end() ? file_size_checked(dependency,Limits::file_bytes) : found->second.size();
            // 原加载器按 buffer/image 记录持有字节；同一路径的重复引用也计入分配预算。
            add_budget(encoded_bytes,size,Limits::dependency_bytes,"glTF encoded dependency bytes");
            if (found == external.end()) {
                found = external.emplace(dependency,read_file(dependency,size)).first;
            }
            return {reinterpret_cast<const unsigned char*>(found->second.data()),found->second.size()};
        }
        fail("Unsupported glTF buffer/image source during budget inspection");
    };
    std::vector<std::span<const unsigned char>> buffers;
    for (const auto& buffer : asset.buffers) {
        require(buffer.byteLength <= Limits::file_bytes, "glTF declared buffer byte budget exceeded");
        const auto view = bytes(buffer.data); require(view.size() >= buffer.byteLength,"Truncated glTF buffer"); buffers.push_back(view);
    }
    std::vector<std::pair<int,int>> dimensions;
    for (const auto& image : asset.images) {
        std::span<const unsigned char> encoded;
        if (const auto* source = std::get_if<fastgltf::sources::BufferView>(&image.data)) {
            require(source->bufferViewIndex < asset.bufferViews.size(),"Invalid image bufferView");
            const auto& view = asset.bufferViews[source->bufferViewIndex]; require(view.bufferIndex < buffers.size(),"Invalid image buffer");
            const auto buffer = buffers[view.bufferIndex];
            require(view.byteOffset <= buffer.size() && view.byteLength <= buffer.size()-view.byteOffset,"Invalid encoded image range");
            encoded = buffer.subspan(view.byteOffset,view.byteLength);
        } else encoded = bytes(image.data);
        require(!encoded.empty() && encoded.size() <= Limits::file_bytes,"glTF encoded image byte budget exceeded");
        int width = 0, height = 0, components = 0;
        require(stbi_info_from_memory(encoded.data(),int(encoded.size()),&width,&height,&components) != 0,"Cannot inspect glTF image dimensions");
        (void)mip_size(width,height); dimensions.emplace_back(width,height);
    }
    std::set<std::pair<std::size_t,bool>> texture_views;
    std::size_t pixels = 0;
    auto texture = [&](const auto& optional, bool srgb) {
        if (!optional) return;
        const auto id = optional->textureIndex; require(id < asset.textures.size(),"Invalid glTF texture reference");
        if (!texture_views.emplace(id,srgb).second) return;
        const auto image = asset.textures[id].imageIndex;
        require(image.has_value() && *image < dimensions.size(),"glTF texture has no supported image");
        const auto [width,height] = dimensions[*image]; add_budget(pixels,mip_size(width,height),Limits::mip_pixels,"glTF decoded texture views including mips");
    };
    for (const auto& material : asset.materials) {
        texture(material.pbrData.baseColorTexture,true); texture(material.emissiveTexture,true);
        texture(material.pbrData.metallicRoughnessTexture,false); texture(material.normalTexture,false); texture(material.occlusionTexture,false);
    }
    std::size_t vertices = 0, source_vertices = 0, indices = 0, primitives = 0;
    for (const auto& mesh : asset.meshes) for (const auto& primitive : mesh.primitives) {
        add_budget(primitives,1,Limits::indices/3,"glTF primitives");
        const auto position = primitive.findAttribute("POSITION");
        require(position != primitive.attributes.end() && position->second < asset.accessors.size(),"Missing/invalid glTF POSITION");
        const auto vertex_count = asset.accessors[position->second].count;
        add_budget(source_vertices,vertex_count,Limits::vertices,"glTF source vertex budget");
        std::size_t raw = vertex_count;
        if (primitive.indicesAccessor) { require(*primitive.indicesAccessor < asset.accessors.size(),"Invalid glTF index accessor"); raw = asset.accessors[*primitive.indicesAccessor].count; }
        require(raw <= Limits::indices,"glTF index accessor budget exceeded");
        std::size_t emitted = raw;
        if (primitive.type == fastgltf::PrimitiveType::TriangleStrip || primitive.type == fastgltf::PrimitiveType::TriangleFan) {
            require(raw >= 3,"Incomplete glTF strip/fan"); emitted = (raw-2)*3;
        } else require(primitive.type == fastgltf::PrimitiveType::Triangles,"Only glTF triangle geometry is supported");
        add_budget(indices,emitted,Limits::indices,"glTF expanded indices");
        require(vertex_count <= Limits::vertices,"glTF source vertex budget exceeded");
        const bool normals = primitive.findAttribute("NORMAL") != primitive.attributes.end();
        add_budget(vertices,normals ? vertex_count : emitted,Limits::vertices,"glTF expanded vertices");
    }
}
} // namespace

Texture load_editor_texture(const std::filesystem::path& input, TextureRole role) {
    const bool srgb = is_color(role); const auto path = normalized(input); const auto extension = lower(path_text(path.extension()));
    require(extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" || extension == ".bmp",
            "Supported textures: PNG, JPEG, TGA, BMP; got " + extension);
    const auto encoded = read_file(path, Limits::file_bytes);
    require(!encoded.empty(), "Empty encoded texture: " + path_text(path));
    int width = 0, height = 0, components = 0;
    const auto* bytes = reinterpret_cast<const stbi_uc*>(encoded.data());
    require(stbi_info_from_memory(bytes, int(encoded.size()), &width, &height, &components) != 0, "Cannot inspect texture: " + path_text(path));
    (void)mip_size(width,height); const int inspected_width = width, inspected_height = height;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(stbi_load_from_memory(bytes, int(encoded.size()), &width, &height, &components, 4), stbi_image_free);
    require(bool(decoded), "Cannot decode texture: " + path_text(path) + ": " + (stbi_failure_reason() ? stbi_failure_reason() : "unknown codec error"));
    require(width == inspected_width && height == inspected_height, "Image dimensions changed during decode");
    Texture texture; texture.name = path_text(path.filename()); texture.srgb = srgb; texture.levels.emplace_back(width,height);
    for (std::size_t i = 0; i < texture.levels[0].pixels.size(); ++i) {
        const auto* p = decoded.get() + 4*i; texture.levels[0].pixels[i] = glm::vec4(p[0],p[1],p[2],p[3])/255.f;
    }
    role_mips(texture,role); return texture;
}

LoadedSceneAsset load_editor_model(const std::filesystem::path& input) {
    const auto path = normalized(input); const auto extension = lower(path_text(path.extension()));
    if (extension == ".fbx") fail("FBX is not supported. Export a static OBJ/MTL or glTF 2.0/GLB asset.");
    require(extension == ".obj" || extension == ".gltf" || extension == ".glb", "Supported models: OBJ/MTL, glTF 2.0, GLB; got " + extension);
    (void)file_size_checked(path, Limits::file_bytes);
    if (extension == ".obj") return ObjLoader{}.load(path);
    preflight_gltf(path);
    auto result = load_scene_asset_detailed(path); (void)validate_scene(result.scene); return result;
}

void append_scene_asset(Scene& destination, Scene source) {
    const auto old = validate_scene(destination), incoming = validate_scene(source);
    SceneBudget total = old;
    add_budget(total.vertices, incoming.vertices, Limits::vertices, "Combined scene vertices");
    add_budget(total.indices, incoming.indices, Limits::indices, "Combined scene indices");
    add_budget(total.pixels, incoming.pixels, Limits::mip_pixels, "Combined scene texture pixels");
    add_budget(total.primitives, incoming.primitives, Limits::indices/3, "Combined scene primitives");
    require(destination.revision != std::numeric_limits<std::uint64_t>::max(), "Scene revision overflow");
    explicit_instances(source); explicit_materials(source); for (auto& mesh : source.meshes) explicit_primitives(mesh);
    Scene candidate = destination; explicit_materials(candidate);
    const bool old_implicit = destination.nodes.empty();
    const auto old_mesh_count = destination.meshes.size();
    for (auto sizes : {std::pair{candidate.meshes.size(),source.meshes.size()}, std::pair{candidate.materials.size(),source.materials.size()},
                       std::pair{candidate.textures.size(),source.textures.size()}, std::pair{candidate.lights.size(),source.lights.size()}}) {
        auto count = sizes.first; add_budget(count,sizes.second,Limits::records,"Combined scene records");
    }
    auto node_count = candidate.nodes.size(); add_budget(node_count,source.nodes.size()+1,Limits::records,"Combined scene nodes");
    if (old_implicit) add_budget(node_count,old_mesh_count,Limits::records,"Preserved implicit scene nodes");
    const int texture_offset = int(candidate.textures.size()), material_offset = int(candidate.materials.size()), mesh_offset = int(candidate.meshes.size());
    const int root_id = int(candidate.nodes.size()), node_offset = root_id + 1;
    for (auto& material : source.materials) for (auto role : {TextureRole::base_color,TextureRole::metallic_roughness,TextureRole::normal,TextureRole::occlusion,TextureRole::emissive}) {
        auto& slot = texture_slot(material,role); if (slot >= 0) slot += texture_offset;
    }
    for (auto& mesh : source.meshes) {
        for (auto& primitive : mesh.primitives) primitive.material += std::uint32_t(material_offset);
        for (auto& lod : mesh.lods) for (auto& primitive : lod.primitives) primitive.material += std::uint32_t(material_offset);
    }
    for (auto& node : source.nodes) { node.parent = node.parent < 0 ? root_id : node.parent + node_offset; if (node.mesh >= 0) node.mesh += mesh_offset; }
    for (auto& light : source.lights) if (light.linked_node >= 0) light.linked_node += node_offset;
    auto append = [](auto& to, auto& from) { to.insert(to.end(),std::make_move_iterator(from.begin()),std::make_move_iterator(from.end())); };
    append(candidate.textures,source.textures); append(candidate.materials,source.materials); append(candidate.meshes,source.meshes);
    Node root; root.name = source.name.empty() ? "Imported asset" : source.name; candidate.nodes.push_back(std::move(root));
    append(candidate.nodes,source.nodes); append(candidate.lights,source.lights);
    // 根必须是第一个新节点。旧场景的隐式实例在导入节点之后显式化，仍处于原世界空间。
    if (old_implicit) for (std::size_t i = 0; i < old_mesh_count; ++i) {
        Node node; node.name = candidate.meshes[i].name; node.mesh = int(i); candidate.nodes.push_back(std::move(node));
    }
    candidate.baked_resources.reset(); ++candidate.revision;candidate.asset_revision=0;
    (void)validate_scene(candidate);
    // 所有可抛异常的工作都在临时快照中；提交只交换容器/标量。
    static_assert(std::is_nothrow_swappable_v<Scene>); using std::swap; swap(destination,candidate);
}

int bind_material_texture(Scene& scene, std::size_t material, Texture texture, TextureRole role) {
    (void)is_color(role); require(material < scene.materials.size(), "Material index out of range");
    require(scene.textures.size() < Limits::records, "Texture record budget exceeded");
    require(scene.revision != std::numeric_limits<std::uint64_t>::max(), "Scene revision overflow");
    (void)validate_texture(texture);
    std::size_t pixels = mip_size(texture.levels[0].width,texture.levels[0].height);
    for (const auto& existing : scene.textures) add_budget(pixels,validate_texture(existing),Limits::mip_pixels,"Bound scene texture pixels");
    role_mips(texture,role);
    const int id = int(scene.textures.size());
    // Texture 的 noexcept move 使 vector::push_back 具备强保证；随后只修改整数引用与 revision。
    static_assert(std::is_nothrow_move_constructible_v<Texture>);
    int& slot = texture_slot(scene.materials[material],role);
    scene.textures.push_back(std::move(texture)); slot = id; ++scene.revision;scene.asset_revision=0; return id;
}
SceneEnvironmentSnapshot capture_environment_snapshot(const Scene& scene) {
    validate_environment_settings(scene);
    return {scene.environment_map,scene.sky_top,scene.sky_bottom,scene.environment_intensity,scene.environment_rotation};
}
void restore_environment_snapshot(Scene& scene,const SceneEnvironmentSnapshot& snapshot) {
    Scene candidate;candidate.sky_top=snapshot.sky_top;candidate.sky_bottom=snapshot.sky_bottom;
    candidate.environment_map=snapshot.map;candidate.environment_intensity=snapshot.intensity;
    candidate.environment_rotation=snapshot.rotation;validate_environment_settings(candidate);
    scene.sky_top=snapshot.sky_top;scene.sky_bottom=snapshot.sky_bottom;scene.environment_map=snapshot.map;
    scene.environment_intensity=snapshot.intensity;scene.environment_rotation=snapshot.rotation;
}
void set_scene_environment(Scene& scene,std::shared_ptr<const EnvironmentMap> map,float intensity,float rotation) {
    SceneEnvironmentSnapshot snapshot{std::move(map),scene.sky_top,scene.sky_bottom,intensity,rotation};
    if(scene.revision==UINT64_MAX)throw std::overflow_error("Scene revision exhausted");
    restore_environment_snapshot(scene,snapshot);++scene.revision;
    // PRT transfer 仍可复用；SceneBakeResources 按环境指纹重算 SH，不清除静态几何烘焙。
}
EnvironmentProjectSnapshot capture_environment_project_snapshot(const Scene& scene,const Camera& camera,const Settings& settings) {
    EnvironmentProjectSnapshot result;result.environment=capture_environment_snapshot(scene);
    ProjectDocument document;document.scene=scene;document.scene.environment_map.reset();
    document.camera=camera;document.settings=settings;result.project=serialize_project(document);return result;
}
void restore_environment_project_snapshot(const EnvironmentProjectSnapshot& snapshot,Scene& scene,Camera& camera,Settings& settings) {
    auto document=deserialize_project(snapshot.project);
    restore_environment_snapshot(document.scene,snapshot.environment);
    const auto revision=std::max(scene.revision,document.scene.revision);
    if(revision==UINT64_MAX)throw std::overflow_error("Scene revision exhausted");
    document.scene.revision=revision+1;scene=std::move(document.scene);camera=document.camera;settings=document.settings;
}
} // namespace emberframe::lab
