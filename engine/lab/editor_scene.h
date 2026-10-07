#pragma once
#include "editor_assets.h"
#include "editor_lights.h"
#include "geometry.h"

namespace emberframe::lab::editor {
// 新文档不复制内置演示的模型和算法设置。地板、初始灯光均由用户选择。
inline Scene make_new_scene(std::string name, bool floor, bool initial_light) {
    Scene scene; scene.name=name.empty()?"Untitled scene":std::move(name);
    if(floor){
        Material material;material.name="Ground";material.base_color={1,1,1,1};material.roughness=.7f;
        scene.materials.push_back(material);
        Mesh mesh;mesh.name="Ground";
        for(auto uv:{glm::vec2(0,0),glm::vec2(0,1),glm::vec2(1,1),glm::vec2(1,0)}){
            Vertex vertex;vertex.position={(uv.x-.5f)*8,0,(uv.y-.5f)*8};
            vertex.normal={0,1,0};vertex.uv=uv*4.f;vertex.tangent={1,0,0,-1};mesh.vertices.push_back(vertex);
        }
        mesh.indices={0,1,2,0,2,3};mesh.primitives.push_back({0,6,0});scene.meshes.push_back(std::move(mesh));
        Node node;node.name="Ground";node.mesh=0;scene.nodes.push_back(std::move(node));
    }
    if(initial_light){Light light;light.direction=safe_normalize(glm::vec3(-.4f,-1,-.35f));light.intensity=2.5f;scene.lights.push_back(light);}
    return scene;
}

struct ImportSample { const char* name; const char* file; };
inline constexpr ImportSample import_samples[]{
    {"彩色立方体","BoxVertexColors"},
    {"送奶车（静态模型）","CesiumMilkTruck"},
};

// 使用正常导入器创建可编辑工程，而不是用特制渲染路径假装导入成功。
inline Scene make_import_sample_scene(const std::filesystem::path& directory) {
    auto scene=make_new_scene("Free asset import studio",true,true);
    for(std::size_t i=0;i<std::size(import_samples);++i){
        const auto& sample=import_samples[i];
        auto loaded=load_editor_model(directory/"models"/sample.file/(std::string(sample.file)+".glb"));
        // 示例工程显式使用最多 512px 的贴图副本；原始 GLB 与完整贴图均不改写。
        // 当前 .ember 会把像素写成十进制文本，直接嵌入大图会放大文件与解析成本。
        for(auto& texture:loaded.scene.textures){
            while(texture.levels.size()>1&&(texture.levels.front().width>512||texture.levels.front().height>512))
                texture.levels.erase(texture.levels.begin());
            texture.name+=" (max 512px sample preview)";
        }
        Aabb bounds;for(const auto& triangle:flatten_scene(loaded.scene))for(const auto& vertex:triangle.vertices)bounds.expand(vertex.position);
        if(bounds.empty())throw std::runtime_error("Sample model contains no triangles");
        const auto extent=bounds.extent();const float scale=(i==0?1.8f:3.7f)/std::max({extent.x,extent.y,extent.z,.0001f});
        const auto center=bounds.center();const float bottom=center.y-extent.y*.5f;
        const glm::vec3 destination{i==0?-2.f:1.7f,.02f,0};
        const auto placement=glm::translate(glm::mat4(1),destination)*glm::scale(glm::mat4(1),glm::vec3(scale))*
            glm::translate(glm::mat4(1),glm::vec3(-center.x,-bottom,-center.z));
        const auto root=scene.nodes.size();append_scene_asset(scene,std::move(loaded.scene));
        scene.nodes[root].local=placement;scene.nodes[root].previous_world=placement;
    }
    const auto textures=directory/"textures"/"Tiles074";
    // 入门工程明确使用现有 Mip 的 512px 预览，避免教学文本格式嵌入大图后读取缓慢。
    // 下载的 1K 原图保留不动，用户仍可在材质页选择完整原图重新绑定。
    auto ground_texture=[&](const char* file,TextureRole role){
        auto texture=load_editor_texture(textures/file,role);
        if(texture.levels.size()>1)texture.levels.erase(texture.levels.begin());
        texture.name+=" (512px sample preview)";return texture;
    };
    bind_material_texture(scene,0,ground_texture("Tiles074_1K-JPG_Color.jpg",TextureRole::base_color),TextureRole::base_color);
    bind_material_texture(scene,0,ground_texture("Tiles074_1K-JPG_NormalGL.jpg",TextureRole::normal),TextureRole::normal);
    // 下载站提供独立灰度粗糙度图，而引擎使用 glTF 的 G=roughness / B=metallic。
    // 显式打包数据通道，不能把灰度图直接绑定后误把其 B 通道当金属度。
    auto roughness=ground_texture("Tiles074_1K-JPG_Roughness.jpg",TextureRole::metallic_roughness);
    roughness.name="Tiles074 packed roughness (G), nonmetal (B=0)";
    roughness.levels.resize(1);
    for(auto& pixel:roughness.levels.front().pixels)pixel={1,pixel.r,0,1};
    bind_material_texture(scene,0,std::move(roughness),TextureRole::metallic_roughness);
    scene.materials[0].roughness=1;scene.materials[0].metallic=0;
    const int point=add_editor_light(scene,LightKind::point);auto fill=scene.lights[point];
    fill.position={-2,2,2};fill.color={.4f,.65f,1};fill.intensity=12;fill.range=12;update_editor_light(scene,point,fill);
    const int area=add_editor_light(scene,LightKind::rectangle);auto panel=scene.lights[area];
    panel.position={1.5f,3.5f,0};panel.direction={0,-1,0};panel.color={1,.85f,.65f};panel.intensity=4;panel.size={2,1.5f};
    update_editor_light(scene,area,panel);
    ++scene.revision;return scene;
}
} // namespace emberframe::lab::editor
