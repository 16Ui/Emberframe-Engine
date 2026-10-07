#include "editor_assets.h"
#include "editor_lights.h"
#include "scene_resources.h"
#include "shading.h"
#include <atomic>
#include <bit>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

namespace emberframe::lab {
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool close(float a, float b, float epsilon = 1e-5f) { return std::abs(a-b) <= epsilon; }
template<class F> std::string error(F&& function) {
    try { function(); } catch (const std::exception& e) { return e.what(); }
    throw std::runtime_error("Expected asset error was not thrown");
}
struct Fixtures {
    std::filesystem::path root;
    Fixtures() {
        static std::atomic<unsigned> serial{0};
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto name = "emberframe-editor-assets-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial.fetch_add(1));
            auto candidate = std::filesystem::temp_directory_path() / name;
            if (std::filesystem::create_directory(candidate)) { root = std::filesystem::canonical(candidate); return; }
        }
        throw std::runtime_error("Cannot create isolated asset fixture directory");
    }
    ~Fixtures() {
        // 只清理本测试独占且规范化的临时目录，绝不删除资产目录或工作区。
        if (root.empty() || !root.filename().string().starts_with("emberframe-editor-assets-test-")) return;
        std::error_code ignored; std::filesystem::remove_all(root,ignored);
    }
    std::filesystem::path path(const std::filesystem::path& name) const { return root / name; }
    void bytes(const std::filesystem::path& name, const std::vector<unsigned char>& data) const {
        const auto file_path = path(name); std::filesystem::create_directories(file_path.parent_path());
        std::ofstream file(file_path,std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size())); check(bool(file),"Fixture byte write failed");
    }
    void text(const std::filesystem::path& name, const std::string& data) const { bytes(name,{data.begin(),data.end()}); }
};
void le16(std::vector<unsigned char>& bytes, unsigned value) {
    bytes.push_back(static_cast<unsigned char>(value & 255)); bytes.push_back(static_cast<unsigned char>((value >> 8) & 255));
}
void le32(std::vector<unsigned char>& bytes, std::uint32_t value) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>((value >> (8*i)) & 255)); }
void be32(std::vector<unsigned char>& bytes, std::uint32_t value) { for (int i = 3; i >= 0; --i) bytes.push_back(static_cast<unsigned char>((value >> (8*i)) & 255)); }
void f32(std::vector<unsigned char>& bytes, float value) { le32(bytes,std::bit_cast<std::uint32_t>(value)); }
std::uint32_t crc32(const std::vector<unsigned char>& bytes, std::size_t start) {
    std::uint32_t crc = ~0u;
    for (std::size_t i = start; i < bytes.size(); ++i) {
        crc ^= bytes[i]; for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & std::uint32_t(-int(crc & 1)));
    }
    return ~crc;
}
std::vector<unsigned char> png3(unsigned width = 3, unsigned height = 1) {
    // 原创 3x1 RGBA PNG：黑/白/红，alpha 0/128/255，专门检测 NPOT 边缘与线性平均。
    std::vector<unsigned char> bytes{137,80,78,71,13,10,26,10};
    auto chunk = [&](const char* type, const std::vector<unsigned char>& payload) {
        be32(bytes,std::uint32_t(payload.size())); const auto start = bytes.size();
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>(type[i]));
        bytes.insert(bytes.end(),payload.begin(),payload.end()); be32(bytes,crc32(bytes,start));
    };
    std::vector<unsigned char> header; be32(header,width); be32(header,height); header.insert(header.end(),{8,6,0,0,0}); chunk("IHDR",header);
    const std::vector<unsigned char> raw{0,0,0,0,0,255,255,255,128,255,0,0,255};
    std::vector<unsigned char> zlib{0x78,0x01,0x01}; le16(zlib,unsigned(raw.size())); le16(zlib,65535u-unsigned(raw.size()));
    zlib.insert(zlib.end(),raw.begin(),raw.end()); std::uint32_t a = 1, b = 0;
    for (auto value : raw) { a = (a+value)%65521; b = (b+a)%65521; }
    be32(zlib,(b<<16)|a); chunk("IDAT",zlib); chunk("IEND",{}); return bytes;
}
std::vector<unsigned char> tga(int width = 3, int height = 1, bool payload = true) {
    std::vector<unsigned char> bytes{0,0,2,0,0,0,0,0,0,0,0,0}; le16(bytes,unsigned(width)); le16(bytes,unsigned(height));
    bytes.insert(bytes.end(),{32,0x28});
    if (payload) for (int i = 0; i < width*height; ++i) bytes.insert(bytes.end(),{255,128,64,192});
    return bytes;
}
std::vector<unsigned char> bmp3() {
    std::vector<unsigned char> bytes{'B','M'}; le32(bytes,66); le32(bytes,0); le32(bytes,54);
    le32(bytes,40); le32(bytes,3); le32(bytes,1); le16(bytes,1); le16(bytes,24); le32(bytes,0); le32(bytes,12);
    for (int i = 0; i < 4; ++i) le32(bytes,0);
    for (int i = 0; i < 3; ++i) bytes.insert(bytes.end(),{0,128,255}); bytes.insert(bytes.end(),{0,0,0}); return bytes;
}
std::vector<unsigned char> jpeg3() {
    // 原创最小 baseline 灰度 JPEG：DC=0、AC=EOB，解码为 128，无外部编码器/下载。
    std::vector<unsigned char> bytes{255,216};
    auto segment = [&](unsigned marker, const std::vector<unsigned char>& data) {
        bytes.push_back(255); bytes.push_back(static_cast<unsigned char>(marker));
        const auto length = unsigned(data.size()+2); bytes.push_back(static_cast<unsigned char>(length>>8)); bytes.push_back(static_cast<unsigned char>(length&255));
        bytes.insert(bytes.end(),data.begin(),data.end());
    };
    std::vector<unsigned char> quant(65,1); quant[0] = 0; segment(0xdb,quant);
    segment(0xc0,{8,0,1,0,3,1,1,0x11,0});
    std::vector<unsigned char> huffman;
    for (unsigned table : {0u,16u}) {
        huffman.push_back(static_cast<unsigned char>(table)); huffman.push_back(1);
        huffman.insert(huffman.end(),15,0); huffman.push_back(0);
    }
    segment(0xc4,huffman); segment(0xda,{1,1,0,0,63,0}); bytes.insert(bytes.end(),{0x3f,255,217}); return bytes;
}
std::filesystem::path model_fixture(const Fixtures& files, bool binary) {
    std::vector<unsigned char> buffer;
    for (float value : {0.f,0.f,0.f,1.f,0.f,0.f,0.f,1.f,0.f}) f32(buffer,value);
    for (int i = 0; i < 3; ++i) for (float value : {0.f,0.f,1.f}) f32(buffer,value);
    for (float value : {0.f,0.f,1.f,0.f,0.f,1.f}) f32(buffer,value);
    for (unsigned index : {0u,1u,2u}) le16(buffer,index); while (buffer.size()%4) buffer.push_back(0);
    const auto image_offset = buffer.size(); const auto image = png3();
    if (binary) buffer.insert(buffer.end(),image.begin(),image.end());
    std::ostringstream json;
    json << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"name":"Imported fixture","nodes":[0]}],"buffers":[{"byteLength":)" << buffer.size();
    if (!binary) json << R"(,"uri":"model.bin")";
    json << R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":6})";
    if (binary) json << R"(,{"buffer":0,"byteOffset":)" << image_offset << R"(,"byteLength":)" << image.size() << '}';
    json << R"(],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],"images":[)";
    json << (binary ? R"({"bufferView":4,"mimeType":"image/png"})" : R"({"uri":"image.png"})");
    json << R"(],"textures":[{"source":0}],"materials":[{"name":"Color and data","pbrMetallicRoughness":{"baseColorFactor":[1,0.5,0.25,1],"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":0},"metallicFactor":0.7,"roughnessFactor":0.4}}],"meshes":[{"name":"Two triangles","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0},{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],"nodes":[{"name":"Parent","translation":[2,3,4],"children":[1]},{"name":"Mesh child","mesh":0,"translation":[0,1,0]}]})";
    std::string text = json.str();
    if (!binary) { files.text("model.gltf",text); files.bytes("model.bin",buffer); files.bytes("image.png",image); return files.path("model.gltf"); }
    while (text.size()%4) text += ' '; while (buffer.size()%4) buffer.push_back(0);
    std::vector<unsigned char> glb; le32(glb,0x46546c67); le32(glb,2); le32(glb,std::uint32_t(28+text.size()+buffer.size()));
    le32(glb,std::uint32_t(text.size())); le32(glb,0x4e4f534a); glb.insert(glb.end(),text.begin(),text.end());
    le32(glb,std::uint32_t(buffer.size())); le32(glb,0x004e4942); glb.insert(glb.end(),buffer.begin(),buffer.end());
    files.bytes("model.glb",glb); return files.path("model.glb");
}
Texture small_texture(std::string name = "Existing") {
    Texture texture; texture.name = std::move(name); texture.levels.emplace_back(1,1,glm::vec4(.2f,.4f,.8f,.6f)); return texture;
}
Scene old_scene() {
    Scene scene; scene.name = "Old scene"; scene.revision = 45; scene.sky_top = {1,2,3}; scene.sky_bottom = {.1f,.2f,.3f};
    scene.textures = {small_texture(),small_texture("Other")}; scene.materials.resize(2);
    scene.materials[0].base_texture = 0; scene.materials[1].mr_texture = 1; scene.materials[1].emissive = {2,3,4};
    Mesh mesh; mesh.name = "Old quad";
    for (auto point : {glm::vec3(-.5f,0,-.5f),glm::vec3(.5f,0,-.5f),glm::vec3(.5f,0,.5f),glm::vec3(-.5f,0,.5f)}) {
        Vertex v; v.position = point; v.normal = {0,-1,0}; mesh.vertices.push_back(v);
    }
    mesh.indices = {0,1,2,0,2,3}; mesh.primitives = {{0,6,1}};
    MeshLod lod; lod.vertices.assign(mesh.vertices.begin(),mesh.vertices.begin()+3); lod.indices = {0,1,2}; lod.primitives = {{0,3,1}}; lod.geometric_error = .2;
    mesh.lods.push_back(std::move(lod)); scene.meshes.push_back(std::move(mesh));
    Node parent; parent.name = "Old parent"; parent.local = glm::translate(glm::mat4(1),{1,2,3}); parent.previous_world = parent.local;
    Node child; child.name = "Old child"; child.parent = 0; child.mesh = 0; child.local = glm::translate(glm::mat4(1),{0,1,0}); child.previous_world = parent.local*child.local;
    scene.nodes = {parent,child}; Light light; light.kind = LightKind::rectangle; light.linked_node = 1;
    light.position = {1,3,3}; light.direction = {0,-1,0}; light.size = {1,1}; light.color = {.5f,.75f,1}; light.intensity = 4;
    scene.lights = {light}; update_editor_light(scene,0,light);
    scene.nodes[1].previous_world = scene_world_transforms(scene)[1]; return scene;
}
std::string snapshot(const Scene& scene) {
    ProjectDocument document; document.scene = scene; document.scene.baked_resources.reset();
    return std::to_string(scene.revision) + ":" + serialize_project(document);
}
void old_prefix_unchanged(const Scene& before, const Scene& after) {
    auto prefix = after; prefix.meshes.resize(before.meshes.size()); prefix.materials.resize(before.materials.size());
    prefix.textures.resize(before.textures.size()); prefix.nodes.resize(before.nodes.size()); prefix.lights.resize(before.lights.size());
    prefix.revision = before.revision; check(snapshot(prefix) == snapshot(before),"Append changed an old scene field");
}
const char* triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
const char* textured_triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\n";
void check_obj_frame(const Vertex& v) {
    check(close(glm::length(v.normal),1) && close(glm::length(glm::vec3(v.tangent)),1),"OBJ normal/tangent must be finite unit vectors");
    check(close(glm::dot(v.normal,glm::vec3(v.tangent)),0),"OBJ tangent must be perpendicular to normal");
    check(v.tangent.w == -1 || v.tangent.w == 1,"OBJ tangent handedness must be a sign");
}
void same_obj_frame(const Vertex& a, const Vertex& b) {
    check_obj_frame(a); check_obj_frame(b);
    check(glm::length(a.normal-b.normal) < 1e-5f && glm::length(a.tangent-b.tangent) < 1e-5f,"OBJ corner tangent frame changed across an isolated boundary");
}
} // namespace

TestResults test_editor_assets() {
    TestResults results;
    auto test = [&](std::string name, auto&& function) {
        TestResult result; result.name = std::move(name);
        try { function(); result.passed = true; result.detail = "All deterministic assertions passed"; }
        catch (const std::exception& e) { result.detail = e.what(); }
        catch (...) { result.detail = "Non-standard exception"; }
        results.push_back(std::move(result));
    };
    test("OBJ concave polygon negative indices winding UV normals tangents", [] {
        Fixtures files;
        const std::string obj = "o Concave\nv 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 1 0\nv 0 2 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt .5 .5\nvt 0 1\nf -5/-5 -4/-4 -3/-3 -2/-2 -1/-1\n";
        files.text("concave.OBJ",obj); const auto loaded = load_editor_model(files.path("concave.OBJ")); const auto& mesh = loaded.scene.meshes.at(0);
        check(mesh.indices.size() == 9 && mesh.vertices.size() == 9 && loaded.scene.nodes[0].mesh == 0,"Concave polygon was not triangulated");
        double area = 0;
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
            const auto a = mesh.vertices[mesh.indices[i]].position, b = mesh.vertices[mesh.indices[i+1]].position, c = mesh.vertices[mesh.indices[i+2]].position;
            const float signed_area = glm::cross(b-a,c-a).z*.5f; check(signed_area > 0,"Ear clipping reversed winding"); area += signed_area;
            const auto center = (a+b+c)/3.f;
            check(!(center.y > 1 && center.y > center.x && center.y > 2-center.x),"Triangle crossed the concave notch");
        }
        check(std::abs(area-3) < 1e-8,"Concave polygon coverage changed");
        for (const auto& v : mesh.vertices) {
            check(close(v.normal.z,1) && close(glm::length(glm::vec3(v.tangent)),1) && close(glm::dot(v.normal,glm::vec3(v.tangent)),0),"OBJ normal/tangent frame invalid");
            check(v.tangent.w == -1,"Flipped OBJ UV handedness was lost");
            if (v.position == glm::vec3(0)) check(v.uv == glm::vec2(0,1),"OBJ vertical UV convention incorrect");
        }
        files.text("reverse.obj",obj.substr(0,obj.find("f -5"))+"f 5/5 4/4 3/3 2/2 1/1\n");
        const auto reverse = load_editor_model(files.path("reverse.obj"));
        for (const auto& v : reverse.scene.meshes[0].vertices) check(close(v.normal.z,-1),"Reverse winding normal was lost");
    });
    test("OBJ mixed zero-area triangles retain valid geometry and report skipped surfaces", [] {
        Fixtures files;
        files.text("export.obj",std::string(triangle)+"v 2 0 0\nv 0 0 0\ns 1\nf 1 2 3\nf 1 2 4\nf 1 5 2\nf 1 1 1\nf 1 2 3\n");
        const auto loaded=load_editor_model(files.path("export.obj"));
        check(loaded.scene.meshes.size()==1&&loaded.scene.meshes[0].indices.size()==6,"Zero-area exporter triangles blocked or changed usable surfaces");
        check(std::any_of(loaded.warnings.begin(),loaded.warnings.end(),[](const auto& w){return w.find("Zero-area")!=std::string::npos;}),"Skipped triangles must be reported");
        // 单个坏面不应生成一个“导入成功但空白”的模型。
        files.text("empty.obj",std::string(triangle)+"f 1 2 2\n");
        check(!error([&]{(void)load_editor_model(files.path("empty.obj"));}).empty(),"Entirely degenerate asset was accepted");
    });
    test("OBJ explicit normals smoothing groups flat fallback and colors", [] {
        Fixtures files;
        files.text("smooth.obj","v 0 0 0 1 0 .5\nv 1 0 0\nv 0 1 0\nv 0 0 1\nvn 0 0 -2\ns 7\nf 1 2 3\nf 1 4 2\ns off\nf 1//-1 2//-1 3//-1\n");
        const auto mesh = load_editor_model(files.path("smooth.obj")).scene.meshes[0];
        const auto expected = glm::normalize(glm::vec3(0,1,1));
        check(glm::length(mesh.vertices[0].normal-expected) < 1e-5f && glm::length(mesh.vertices[3].normal-expected) < 1e-5f,"Area weighted smoothing failed");
        check(mesh.vertices[6].normal == glm::vec3(0,0,-1),"Explicit vn was overwritten");
        check(mesh.vertices[0].color == glm::vec4(1,0,.5f,1),"OBJ vertex color was lost");
        files.text("flat.obj",std::string(triangle)+"f 1 2 3\n");
        const auto flat = load_editor_model(files.path("flat.obj"));
        for (const auto& v : flat.scene.meshes[0].vertices) check(v.normal == glm::vec3(0,0,1),"Missing normals not generated");
    });
    test("OBJ smooth tangents and normal maps remain continuous across UV duplicates materials and groups", [] {
        Fixtures files; files.bytes("normal.tga",tga());
        files.text("smooth.mtl","newmtl A\nnorm normal.tga\nnewmtl B\nnorm normal.tga\n");
        const std::string header = "v 0 0 0 1 .5 .25\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                                   "vt 0 0\nvt 1 0\nvt 0 1\nvt -1 1\nvt 0 0\nvt 0 1\nvn 1 0 1\nvn 2 0 2\nmtllib smooth.mtl\ns 7\nusemtl A\n";
        const auto normal = glm::normalize(glm::vec3(1,0,1));
        const auto sum = glm::vec3(1,0,0)+glm::normalize(glm::vec3(0,1,-1));
        const auto expected = glm::normalize(sum-normal*glm::dot(normal,sum));
        for (bool authored : {false,true}) for (int split : {0,1,2}) {
            auto obj = header + (authored ? "f 1/1/1 2/2/1 3/3/1\n" : "f 1/1 2/2 3/3\n");
            if (split == 1) obj += "usemtl B\n";
            if (split == 2) obj += "g Other\nusemtl B\n";
            obj += authored ? "f 1/5/2 3/6/2 4/4/2\n" : "f 1/5 3/6 4/4\n";
            files.text("smooth.obj",obj); const auto loaded = load_editor_model(files.path("smooth.obj")); const auto& scene = loaded.scene;
            std::vector<const Vertex*> corners; std::size_t indices = 0, primitives = 0;
            for (const auto& mesh : scene.meshes) {
                indices += mesh.indices.size(); primitives += mesh.primitives.size();
                for (const auto& v : mesh.vertices) { check_obj_frame(v); corners.push_back(&v); }
                for (std::size_t i = 0; i < mesh.indices.size(); ++i)
                    check(mesh.indices[i] == i,"OBJ tangent smoothing rewrote corner indices");
            }
            check(corners.size() == 6 && indices == 6 && primitives == std::size_t(split ? 2 : 1),"OBJ tangent smoothing changed geometry/material counts");
            check(scene.meshes.size() == std::size_t(split == 2 ? 2 : 1) && scene.nodes.size() == scene.meshes.size(),"OBJ smoothing changed group/node structure");
            for (const auto pair : {std::array<std::size_t,2>{0,3},std::array<std::size_t,2>{2,4}}) {
                const auto& a = *corners[pair[0]]; const auto& b = *corners[pair[1]]; same_obj_frame(a,b);
                check(glm::length(a.normal-normal) < 1e-5f && glm::length(glm::vec3(a.tangent)-expected) < 1e-5f && a.tangent.w == -1,"Smooth corner lost angle-weighted direction or OBJ V handedness");
                SurfaceSample first, second; first.normal = a.normal; first.tangent = a.tangent; first.uv = a.uv;
                second.normal = b.normal; second.tangent = b.tangent; second.uv = b.uv;
                first.material = int(scene.meshes.front().primitives.front().material); second.material = int(scene.meshes.back().primitives.back().material);
                const auto mapped = sample_normal(scene,first,Settings{});
                check(glm::length(mapped-sample_normal(scene,second,Settings{})) < 1e-5f,"Imported normal map still exposes a smooth OBJ edge");
                check(glm::length(mapped-normal) > .1f,"Normal-map regression sampled a flat/untextured normal");
            }
            check(corners[0]->color == glm::vec4(1,.5f,.25f,1) && corners[3]->color == corners[0]->color,"OBJ tangent smoothing lost vertex colors");
        }
    });
    test("OBJ tangents isolate flat faces smoothing groups UV seams mirrored orientation and source vertices", [] {
        Fixtures files;
        const std::string header = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv -1 0 0\nv 0 0 0\n"
                                   "vt 0 0\nvt 1 0\nvt 0 1\nvt -1 1\nvt .25 0\nvt .25 1\nvt -.75 1\nvt 1 0\n"
                                   "vn 0 0 1\nvn 0 1 1\n";
        const std::vector<std::array<std::string,2>> cases = {
            {"s off\nf 1/1 2/2 3/3\n","s off\nf 1/1 3/3 4/4\n"},
            {"s 1\nf 1/1 2/2 3/3\n","s 2\nf 1/1 3/3 4/4\n"},
            {"s 1\nf 1/1 2/2 3/3\n","s 1\nf 1/5 3/6 4/7\n"},
            {"s 1\nf 1/1 2/2 3/3\n","s 1\nf 1/1 3/3 4/8\n"},
            {"s 1\nf 1/1 2/2 3/3\n","s 1\nf 5/1 3/3 4/4\n"},
            {"s off\nf 1/1/1 2/2/1 3/3/1\n","s off\nf 1/1/2 3/3/2 4/4/2\n"}
        };
        for (std::size_t index = 0; index < cases.size(); ++index) {
            const auto& faces = cases[index]; files.text("boundary.obj",header+faces[0]+faces[1]);
            const auto mesh = load_editor_model(files.path("boundary.obj")).scene.meshes.at(0);
            check(mesh.vertices.size() == 6 && mesh.indices == std::vector<std::uint32_t>({0,1,2,3,4,5}),"Boundary preservation changed expanded OBJ topology");
            for (int face = 0; face < 2; ++face) {
                files.text("single.obj",header+faces[std::size_t(face)]); const auto reference = load_editor_model(files.path("single.obj")).scene.meshes.at(0);
                // 重合但不同源位置仅隔离该角，另一个共享角仍允许平滑。
                for (std::size_t k = 0; k < 3; ++k) if (index != 4 || k == 0)
                    same_obj_frame(mesh.vertices[std::size_t(face)*3+k],reference.vertices[k]);
            }
            if (index == 3) {
                check(mesh.vertices[0].tangent.w == -1 && mesh.vertices[3].tangent.w == 1,"Mirrored OBJ handedness was averaged together");
                for (const auto& v : mesh.vertices)
                    check(glm::length(glm::cross(v.normal,glm::vec3(v.tangent))*v.tangent.w-glm::vec3(0,-1,0)) < 1e-5f,"Mirrored OBJ bitangent opposes the flipped V axis");
            }
        }
    });
    test("OBJ missing and degenerate UV triangles keep a finite isolated tangent fallback", [] {
        Fixtures files;
        const std::string header = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv -1 0 0\nvt 0 0\nvt 0 1\nvt -1 0\ns 1\n";
        for (const auto* face : {"f 1/1 3/1 4/1\n","f 1/1 3/3 4\n"}) {
            files.text("fallback.obj",header+"f 1/1 2/2 3/3\n"+face);
            const auto mesh = load_editor_model(files.path("fallback.obj")).scene.meshes.at(0);
            check(mesh.vertices.size() == 6,"UV fallback expanded geometry");
            for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
                const auto& v = mesh.vertices[i]; check_obj_frame(v);
                if (i < 3) check(glm::length(glm::vec3(v.tangent)-glm::vec3(0,-1,0)) < 1e-5f && v.tangent.w == -1,"Invalid UV neighbor contaminated the usable tangent chart");
                else check(glm::length(glm::vec3(v.tangent)-glm::vec3(1,0,0)) < 1e-5f && v.tangent.w == 1,"Missing/degenerate UV fallback is not deterministic");
            }
        }
        // 很小但非退化的 UV 图仍有有效导数，不应被绝对 determinant 阈值替换为回退。
        files.text("tiny.obj",std::string(triangle)+"vt 0 0\nvt 0 1\nvt -1e-24 0\nf 1/1 2/2 3/3\n");
        const auto tiny = load_editor_model(files.path("tiny.obj")).scene.meshes.at(0);
        for (const auto& v : tiny.vertices) { check_obj_frame(v); check(glm::length(glm::vec3(v.tangent)-glm::vec3(0,-1,0)) < 1e-5f && v.tangent.w == -1,"Tiny valid OBJ UVs fell back to an unrelated axis"); }
    });
    test("OBJ MTL quoted Unicode Windows paths map options scalar packing and bump", [] {
        Fixtures files; files.bytes(std::filesystem::path(u8"maps/颜色 图.png"),png3()); files.bytes("maps/height.tga",tga());
        // base/alpha 合并为一张 RGBA，两个输入必须使用相同的 clamp/repeat 语义。
        files.text("surface.mtl", "newmtl Painted surface\nKd .8 .7 .6\nKe 0 0 0\nPm .3\nPr .4\nNs 100\nd .8\n"
                   "map_Kd -s 1 1 1 -o 0 0 -t 0 -mm 0 1 -clamp on \"maps\\颜色 图.png\" # comment\n"
                   "map_Ke maps/颜色 图.png\nmap_d -imfchan m -clamp on maps/颜色 图.png\n"
                   "map_Pr -imfchan g -clamp on maps/颜色 图.png\nmap_Pm -imfchan b -clamp on maps/颜色 图.png\n"
                   "map_ao -imfchan r maps/height.tga\nbump -bm 2 -imfchan r maps/height.tga\n");
        files.text("surface.obj",std::string(textured_triangle)+"usemtl Painted surface\nmtllib surface.mtl\nf -3/-3 -2/-2 -1/-1\n");
        const auto loaded = load_editor_model(files.path("surface.obj")); const auto& material = loaded.scene.materials[1];
        check(loaded.samplers.size() == loaded.scene.textures.size(),"OBJ sampler metadata not parallel");
        check(material.base_texture >= 0 && material.mr_texture >= 0 && material.normal_texture >= 0 && material.ao_texture >= 0 && material.emissive_texture >= 0,"MTL maps missing slots");
        check(material.base_color == glm::vec4(.8f,.7f,.6f,.8f) && material.emissive == glm::vec3(0) && close(material.metallic,.3f) && close(material.roughness,.4f),"Explicit MTL factors changed");
        check(material.alpha_mode == 2 && close(material.normal_scale,1),"Opacity/height normal defaults wrong");
        const auto& base = loaded.scene.textures[std::size_t(material.base_texture)];
        check(base.srgb && base.wrap_s == Texture::Wrap::clamp_to_edge && close(base.levels[0].pixels[1].a,(128.f/255)*(128.f/255)),"Map options/path/opacity combination failed");
        check(loaded.samplers[std::size_t(material.base_texture)].wrap_s == 33071,"MTL clamp metadata lost");
        const auto& mr = loaded.scene.textures[std::size_t(material.mr_texture)];
        check(!mr.srgb && mr.levels[0].pixels[2].g == 0 && mr.levels[0].pixels[2].b == 0,"Scalar rough/metal maps not packed into G/B");
        const auto& normal = loaded.scene.textures[std::size_t(material.normal_texture)];
        check(!normal.srgb && normal.levels[0].pixels[0] == glm::vec4(.5f,.5f,1,1),"Constant scalar bump was mistaken for RGB normal");
        check(!loaded.warnings.empty(),"MTL approximation warnings missing");
    });
    test("MTL normal packed ORM defaults multiple libraries and escaped filenames", [] {
        Fixtures files; files.bytes("map #1.png",png3()); files.bytes("normal.tga",tga());
        files.text("first library.mtl","newmtl One\nnorm -bm .6 normal.tga\nmap_ORM map\\ \\#1.png\n");
        files.text("second.mtl","newmtl Two\nmap_Ke map\\ \\#1.png\n");
        files.text("model.obj",std::string(textured_triangle)+"mtllib \"first library.mtl\" second.mtl\nusemtl One\nf 1/1 2/2 3/3\ng Other\nusemtl Two\nf 1/1 2/2 3/3\n");
        const auto loaded = load_editor_model(files.path("model.obj")); const auto& first = loaded.scene.materials[1]; const auto& second = loaded.scene.materials[2];
        check(loaded.scene.meshes.size() == 2 && loaded.scene.meshes[1].primitives[0].material == 2,"Groups/material names not resolved");
        check(first.mr_texture == first.ao_texture && first.metallic == 1 && first.roughness == 1 && close(first.normal_scale,.6f),"Packed ORM/default factors wrong");
        check(second.emissive == glm::vec3(1) && loaded.scene.textures[std::size_t(second.emissive_texture)].srgb,"Emissive map without Ke is unusable");
    });
    test("OBJ malformed indices geometry numeric data and face budget errors", [] {
        Fixtures files;
        const std::vector<std::string> bad = {
            std::string(triangle)+"f 0 2 3\n", std::string(triangle)+"f -4 -2 -1\n", std::string(triangle)+"f 1 2 4\n",
            std::string(triangle)+"f 1/1 2/1 3/1\n", std::string(triangle)+"f 1/ 2 3\n", std::string(triangle)+"f 1//1 2 3\n",
            std::string(triangle)+"f 1 2 2\n", "v 0 0 0\nv 1 1 0\nv 0 1 0\nv 1 0 0\nf 1 2 3 4\n",
            "v 0 0 0\nv 1 0 0\nv 1 1 1\nv 0 1 0\nf 1 2 3 4\n", std::string(triangle)+"curv 0 1 1 2 3\n",
            "v nan 0 0\nf 1 1 1\n", "vn 0 0 0\n", std::string(triangle)+"usemtl Absent\nf 1 2 3\n", std::string(triangle),
            std::string(triangle)+"mtllib absent.mtl\nf 1 2 3\n", std::string(triangle)+"f 1 2 9223372036854775808\n"
        };
        for (const auto& obj : bad) { files.text("bad.obj",obj); check(!error([&]{(void)load_editor_model(files.path("bad.obj"));}).empty(),"OBJ failure has no diagnostic"); }
        std::string large = triangle; large += "f"; for (std::size_t i = 0; i <= EditorAssetLimits::face_corners; ++i) large += " 1";
        files.text("bad.obj",large); check(error([&]{(void)load_editor_model(files.path("bad.obj"));}).find("corner budget") != std::string::npos,"Face budget not enforced");
        auto nul = std::string(triangle); nul.push_back('\0'); files.text("bad.obj",nul); (void)error([&]{(void)load_editor_model(files.path("bad.obj"));});
        check(error([&]{(void)load_editor_model(files.path("unsupported.fbx"));}).find("FBX") != std::string::npos,"FBX claimed as supported");
    });
    test("MTL missing maps unrepresentable options conflicts and invalid syntax fail", [] {
        Fixtures files; files.bytes("good.png",png3()); files.bytes("other.tga",tga(2,1));
        const std::vector<std::string> bad = {
            "Kd 1 1 1\n", "newmtl M\nKd nan 0 0\n", "newmtl M\nnewmtl M\n", "newmtl M\nPm 2\n",
            "newmtl M\nmap_Kd absent.png\n", "newmtl M\nmap_Kd -s 2 1 good.png\n", "newmtl M\nmap_Kd -clamp maybe good.png\n",
            "newmtl M\nmap_Kd -clamp\n", "newmtl M\nmap_Kd -unknown good.png\n", "newmtl M\nmap_Kd \"unfinished\n",
            "newmtl M\nnorm good.png\nbump good.png\n", "newmtl M\nmap_Pr good.png\nmap_metallic_roughness good.png\n",
            "newmtl M\nmap_Pr good.png\nmap_Pm other.tga\n", "newmtl M\nmap_Kd good.png\nmap_d other.tga\n",
            "newmtl M\nmap_Kd -clamp on good.png\nmap_d -clamp off good.png\n"
        };
        files.text("model.obj",std::string(textured_triangle)+"mtllib bad.mtl\nusemtl M\nf 1/1 2/2 3/3\n");
        for (const auto& mtl : bad) { files.text("bad.mtl",mtl); (void)error([&]{(void)load_editor_model(files.path("model.obj"));}); }
        files.text("bad.mtl","newmtl M\nmap_Kd good.png\n"); files.text("model.obj",std::string(triangle)+"mtllib bad.mtl\nusemtl M\nf 1 2 3\n");
        check(error([&]{(void)load_editor_model(files.path("model.obj"));}).find("missing UV") != std::string::npos,"Textured OBJ with absent UV accepted");
    });
    test("PNG JPEG TGA BMP Unicode paths role encoding NPOT linear mips", [] {
        Fixtures files; const auto unicode = std::filesystem::path(u8"纹理 空间/样例.PNG"); files.bytes(unicode,png3());
        files.bytes("color.JpEg",jpeg3()); files.bytes("color.TGA",tga()); files.bytes("color.BmP",bmp3());
        for (const auto& path : {files.path(unicode),files.path("color.JpEg"),files.path("color.TGA"),files.path("color.BmP")}) {
            const auto texture = load_editor_texture(path,TextureRole::base_color);
            check(texture.srgb && texture.levels.size() == 2 && texture.levels[0].width == 3 && texture.levels[1].width == 1,"Image format or NPOT mip chain failed");
            check(texture.min_filter == Texture::Filter::linear_mipmap_linear && texture.mag_filter == Texture::Filter::linear,"Editor image default filter wrong");
        }
        const auto color = load_editor_texture(files.path(unicode),TextureRole::base_color);
        const auto data = load_editor_texture(files.path(unicode),TextureRole::metallic_roughness);
        check(close(srgb_to_linear(color.levels[1].pixels[0].r),2.f/3) && close(srgb_to_linear(color.levels[1].pixels[0].g),1.f/3),"sRGB mip averaged encoded values or lost last NPOT texel");
        check(close(data.levels[1].pixels[0].r,2.f/3) && close(data.levels[1].pixels[0].g,1.f/3) && !data.srgb,"Data mip underwent sRGB conversion");
        check(close(color.levels[1].pixels[0].a,(128.f/255+1)/3),"Alpha mip is not linear");
        check(load_editor_texture(files.path(unicode),TextureRole::emissive).srgb && !load_editor_texture(files.path(unicode),TextureRole::occlusion).srgb,"Texture role encoding wrong");
        const auto normal = load_editor_texture(files.path("color.TGA"),TextureRole::normal);
        check(!normal.srgb && close(glm::length(glm::vec3(normal.levels[1].pixels[0])*2.f-1.f),1),"Normal mip not renormalized");
        const auto jpeg = load_editor_texture(files.path("color.JpEg"),TextureRole::occlusion);
        check(close(jpeg.levels[0].pixels[0].r,128.f/255,1.f/255),"Generated JPEG decoded incorrectly");
        check(close(load_editor_texture(files.path("color.TGA"),TextureRole::occlusion).levels[0].pixels[0].a,192.f/255),"TGA alpha lost");
    });
    test("Texture header pixel byte format and corrupt-data limits", [] {
        Fixtures files; files.bytes("huge.tga",tga(8192,8192,false));
        check(error([&]{(void)load_editor_texture(files.path("huge.tga"),TextureRole::normal);}).find("pixel budget") != std::string::npos,"Oversized dimensions were not rejected before decode");
        files.text("broken.png","invalid PNG"); files.text("empty.png","");
        (void)error([&]{(void)load_editor_texture(files.path("broken.png"),TextureRole::base_color);});
        (void)error([&]{(void)load_editor_texture(files.path("empty.png"),TextureRole::base_color);});
        (void)error([&]{(void)load_editor_texture(files.path("image.gif"),TextureRole::base_color);});
        (void)error([&]{(void)load_editor_texture(files.path("broken.png"),TextureRole(100));});
        {
            std::ofstream sparse(files.path("over.png"),std::ios::binary); sparse.seekp(std::streamoff(EditorAssetLimits::file_bytes)); sparse.put('x'); check(bool(sparse),"Oversize fixture failed");
        }
        check(error([&]{(void)load_editor_texture(files.path("over.png"),TextureRole::base_color);}).find("byte budget") != std::string::npos,"Encoded byte cap ignored");
    });
    test("glTF GLB delegated loads append material texture LOD nodes lights offsets", [] {
        Fixtures files;
        for (bool binary : {false,true}) {
            const auto loaded = load_editor_model(model_fixture(files,binary));
            check(loaded.scene.textures.size() == 2 && loaded.samplers.size() == 2,"glTF role views/samplers not preserved");
            const auto& material = loaded.scene.materials[0];
            check(loaded.scene.textures[std::size_t(material.base_texture)].srgb && !loaded.scene.textures[std::size_t(material.mr_texture)].srgb,"glTF color/data view distinction lost");
            Scene source = loaded.scene; const auto& mesh = source.meshes[0];
            MeshLod lod; lod.vertices.assign(mesh.vertices.begin(),mesh.vertices.begin()+3); lod.indices = {0,1,2}; lod.primitives = {{0,3,0}}; lod.geometric_error = .1;
            source.meshes[0].lods.push_back(std::move(lod));
            const int linked_light = add_editor_light(source,LightKind::rectangle);
            const int source_linked_node = source.lights[std::size_t(linked_light)].linked_node;
            source.nodes[std::size_t(source_linked_node)].parent = 0;
            update_editor_light(source,std::size_t(linked_light),source.lights[std::size_t(linked_light)]);
            source.nodes[std::size_t(source_linked_node)].previous_world = scene_world_transforms(source)[std::size_t(source_linked_node)];
            Light free; free.linked_node = -1; source.lights.push_back(free);
            const auto source_before = snapshot(source); auto destination = old_scene(); destination.baked_resources = std::make_shared<SceneBakeResources>();
            const auto before = destination; const auto old_nodes = destination.nodes.size(), old_meshes = destination.meshes.size(), old_materials = destination.materials.size(), old_textures = destination.textures.size();
            append_scene_asset(destination,source);
            check(snapshot(source) == source_before,"Append mutated caller's source"); old_prefix_unchanged(before,destination);
            check(destination.nodes[old_nodes].name == source.name && destination.nodes[old_nodes].parent == -1 && destination.nodes[old_nodes].mesh == -1,"Named root was not the first appended node");
            check(destination.nodes[old_nodes+1].parent == int(old_nodes) && destination.nodes[old_nodes+2].parent == int(old_nodes+1) && destination.nodes[old_nodes+2].mesh == int(old_meshes),"Imported hierarchy/mesh references not remapped");
            check(destination.materials[old_materials].base_texture == int(old_textures)+material.base_texture && destination.materials[old_materials].mr_texture == int(old_textures)+material.mr_texture,"Material texture offsets incorrect");
            check(destination.meshes[old_meshes].primitives[0].material == old_materials && destination.meshes[old_meshes].lods[0].primitives[0].material == old_materials,"LOD/material references not remapped");
            check(destination.lights[1].linked_node == int(old_nodes+1)+source_linked_node && destination.lights[2].linked_node == -1,"Light ownership offset/sentinel lost");
            check(destination.revision == before.revision+1 && !destination.baked_resources,"Geometry append did not invalidate bake/revision");
            const auto worlds = scene_world_transforms(destination);
            check(glm::vec3(worlds[old_nodes+2][3]) == glm::vec3(2,4,4),"Imported world transform changed under root");
            // 直接验证完整合并场景的持久化往返，所有 linked_node 保持真实值。
            ProjectDocument combined; combined.scene = destination;
            const auto restored = deserialize_project(serialize_project(combined));
            check(snapshot(restored.scene) == snapshot(destination),"Appended linked emitters did not survive project persistence");
        }
    });
    test("Append implicit instances nonindexed primitives defaults and first root", [] {
        Scene destination; destination.name = "Implicit old"; Mesh mesh; mesh.name = "Implicit triangle";
        for (auto p : {glm::vec3(0,0,0),glm::vec3(1,0,0),glm::vec3(0,1,0)}) { Vertex v; v.position = p; mesh.vertices.push_back(v); }
        destination.meshes.push_back(mesh); Scene source; source.name = "Implicit incoming"; source.meshes.push_back(mesh);
        source.materials.resize(1); source.materials[0].name = "Incoming material";
        append_scene_asset(destination,source);
        check(destination.nodes[0].name == source.name && destination.nodes[1].mesh == 1 && destination.nodes[1].parent == 0,"Root is not first new node for implicit destination");
        check(destination.nodes[2].mesh == 0 && destination.nodes[2].parent == -1,"Old implicit mesh disappeared or moved");
        check(destination.materials.size() == 2 && destination.meshes[1].primitives[0].material == 1 && destination.meshes[1].primitives[0].index_count == 3,"Implicit primitive material wasn't remapped");
    });
    test("Append rejects invalid references cycles LODs and aggregates without mutation", [] {
        auto destination = old_scene(); destination.baked_resources = std::make_shared<SceneBakeResources>(); const auto before = snapshot(destination); const auto bake = destination.baked_resources;
        auto rejects = [&](Scene source) { (void)error([&]{append_scene_asset(destination,std::move(source));}); check(snapshot(destination) == before && destination.baked_resources == bake,"Failed append changed destination"); };
        auto source = old_scene(); source.materials[0].base_texture = 99; rejects(source);
        source = old_scene(); source.meshes[0].lods[0].primitives[0].material = 99; rejects(source);
        source = old_scene(); source.meshes[0].indices[0] = 99; rejects(source);
        source = old_scene(); source.nodes[1].mesh = 99; rejects(source);
        source = old_scene(); source.nodes[0].parent = 1; rejects(source);
        source = old_scene(); source.nodes[0].parent = -2; rejects(source);
        source = old_scene(); source.lights[0].linked_node = 99; rejects(source);
        source = old_scene(); source.lights[0].linked_node = -2; rejects(source);
        source = old_scene(); source.lights[0].kind = LightKind::directional; rejects(source);
        source = old_scene(); source.lights.push_back(source.lights[0]); rejects(source);
        source = old_scene(); source.meshes[0].vertices[0].normal = {0,0,1}; rejects(source);
        source = old_scene(); source.textures[0].levels[0].pixels.clear(); rejects(source);
        source = old_scene(); source.textures[0].levels[0].width = 8192; source.textures[0].levels[0].height = 8192; rejects(source);
        source = old_scene(); source.meshes[0].lods[0].geometric_error = -1; rejects(source);
        source = {}; source.nodes.resize(EditorAssetLimits::records); rejects(source);
        auto invalid_destination = destination; invalid_destination.nodes[0].parent = 99;
        const auto parent = invalid_destination.nodes[0].parent; (void)error([&]{append_scene_asset(invalid_destination,old_scene());});
        check(invalid_destination.nodes[0].parent == parent && invalid_destination.revision == destination.revision,"Invalid destination was partially modified");
        destination.revision = std::numeric_limits<std::uint64_t>::max(); const auto max_before = snapshot(destination);
        (void)error([&]{append_scene_asset(destination,old_scene());}); check(snapshot(destination) == max_before,"Revision overflow mutated destination");
    });
    test("Binding each role appends views preserves all factors and is exception safe", [] {
        auto scene = old_scene(); auto& material = scene.materials[1]; material.base_color = {.1f,.2f,.3f,.4f}; material.metallic = .35f; material.roughness = .65f;
        material.normal_scale = .7f; material.ao_strength = .8f; material.clearcoat = .3f; material.emissive = {3,2,1};
        const auto factors = material; const auto old_revision = scene.revision;
        std::size_t ordinal = 0;
        for (auto role : {TextureRole::base_color,TextureRole::metallic_roughness,TextureRole::normal,TextureRole::occlusion,TextureRole::emissive}) {
            auto texture = small_texture("Bound " + std::to_string(ordinal++)); texture.srgb = true;
            texture.levels.clear(); texture.levels.emplace_back(3,1,glm::vec4(.25f,.5f,1,.75f));
            const auto index = bind_material_texture(scene,1,std::move(texture),role);
            check(index == int(scene.textures.size()-1),"Binding did not return appended index"); const auto& view = scene.textures[std::size_t(index)];
            check(view.srgb == (role == TextureRole::base_color || role == TextureRole::emissive) && view.levels.size() == 2,"Binding role view/mips wrong");
            int slot = -1;
            switch (role) {
            case TextureRole::base_color: slot = material.base_texture; break;
            case TextureRole::metallic_roughness: slot = material.mr_texture; break;
            case TextureRole::normal: slot = material.normal_texture; break;
            case TextureRole::occlusion: slot = material.ao_texture; break;
            case TextureRole::emissive: slot = material.emissive_texture; break;
            }
            check(slot == index,"Binding changed the wrong material slot");
        }
        check(scene.revision == old_revision+5 && material.base_color == factors.base_color && material.emissive == factors.emissive &&
              material.metallic == factors.metallic && material.roughness == factors.roughness && material.normal_scale == factors.normal_scale &&
              material.ao_strength == factors.ao_strength && material.clearcoat == factors.clearcoat,"Binding reset unrelated factors");
        const auto before = snapshot(scene);
        auto rejects = [&](std::size_t id, Texture texture, TextureRole role) {
            (void)error([&]{(void)bind_material_texture(scene,id,std::move(texture),role);}); check(snapshot(scene) == before,"Failed binding changed scene");
        };
        rejects(99,small_texture(),TextureRole::normal); rejects(1,small_texture(),TextureRole(99)); rejects(1,{},TextureRole::base_color);
        auto malformed = small_texture(); malformed.levels[0].width = 2; rejects(1,malformed,TextureRole::occlusion);
        malformed = small_texture(); malformed.levels[0].pixels[0].r = std::numeric_limits<float>::quiet_NaN(); rejects(1,malformed,TextureRole::normal);
    });
    test("glTF preflight caps referenced external bytes vertices and decoded pixels", [] {
        Fixtures files;
        files.text("huge.gltf",R"({"asset":{"version":"2.0"},"buffers":[{"uri":"absent.bin","byteLength":67108865}]})");
        check(error([&]{(void)load_editor_model(files.path("huge.gltf"));}).find("buffer byte budget") != std::string::npos,"Declared external byte cap not checked before loading");
        files.text("huge.gltf",R"({"asset":{"version":"2.0"},"accessors":[{"componentType":5126,"count":1048577,"type":"VEC3"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}]})");
        check(error([&]{(void)load_editor_model(files.path("huge.gltf"));}).find("vertex budget") != std::string::npos,"Vertex cap not checked before allocating geometry");
        files.bytes("huge.tga",tga(8192,8192,false));
        files.text("huge.gltf",R"({"asset":{"version":"2.0"},"images":[{"uri":"huge.tga"}]})");
        check(error([&]{(void)load_editor_model(files.path("huge.gltf"));}).find("pixel budget") != std::string::npos,"Referenced image cap not checked before decode");
        // 每个视图单独合法，但同一图像同时用于 sRGB/线性会产生两份解码资源并超总预算。
        // 大尺寸 IHDR 与完整小 PNG 容器足以做 info 检查，超限必须在尝试解压像素之前失败。
        files.bytes("views.png",png3(4096,2048));
        files.text("huge.gltf",R"({"asset":{"version":"2.0"},"images":[{"uri":"views.png"}],"textures":[{"source":0}],"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":0}}}]})");
        check(error([&]{(void)load_editor_model(files.path("huge.gltf"));}).find("views including mips") != std::string::npos,"Aggregate decoded color/data view cap ignored");
    });
    return results;
}
} // namespace emberframe::lab

#ifdef EMBERFRAME_EDITOR_ASSETS_TEST_MAIN
int main() {
    int failed = 0; const auto results = emberframe::lab::test_editor_assets();
    for (const auto& result : results) { std::cout << (result.passed ? "PASS " : "FAIL ") << result.name << ": " << result.detail << '\n'; failed += !result.passed; }
    std::cout << results.size()-std::size_t(failed) << '/' << results.size() << " passed\n"; return failed ? 1 : 0;
}
#endif
