#pragma once
#include "systems.h"

namespace emberframe::lab::editor {
// 图像本来就是浮点像素；不要为了撤销把数百万像素序列化为十进制文本。
// 快照仅在一次操作开始/结束时生成，连续拖动中不反复复制纹理。
struct Snapshot { Scene scene;Camera camera;Settings settings; };
inline std::size_t scene_bytes(const Scene& scene) {
    std::size_t n=sizeof(Scene)+scene.name.size();
    for(const auto& mesh:scene.meshes){
        n+=sizeof(Mesh)+mesh.name.size()+mesh.vertices.size()*sizeof(Vertex)+mesh.indices.size()*4+mesh.primitives.size()*sizeof(Primitive);
        for(const auto& lod:mesh.lods)n+=sizeof(MeshLod)+lod.vertices.size()*sizeof(Vertex)+lod.indices.size()*4+lod.primitives.size()*sizeof(Primitive);
    }
    for(const auto& texture:scene.textures){n+=sizeof(Texture)+texture.name.size();for(const auto& image:texture.levels)n+=sizeof(image)+image.pixels.size()*sizeof(glm::vec4);}
    for(const auto& m:scene.materials)n+=sizeof(Material)+m.name.size();
    for(const auto& node:scene.nodes)n+=sizeof(Node)+node.name.size();
    return n+scene.lights.size()*sizeof(Light);
}
inline void restore_snapshot(const Snapshot& snapshot,Scene& scene,Camera& camera,Settings& settings) {
    // 版本不能倒退，否则异步 GPU 上传可能误把恢复后的内容当作旧缓存。
    if(scene.revision==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Scene revision exhausted");
    const auto revision=scene.revision+1;
    Scene restored=snapshot.scene;restored.revision=revision;
    // 先完成所有分配，再整体交换；内存不足不能留下“旧节点 + 新网格”的半恢复状态。
    using std::swap;swap(scene,restored);
    camera=snapshot.camera;settings=snapshot.settings;
}
inline void record_snapshot(UndoStack& history,std::string label,std::shared_ptr<const Snapshot> before,
    Scene& scene,Camera& camera,Settings& settings) {
    auto after=std::make_shared<Snapshot>(Snapshot{scene,camera,settings});
    const auto bytes=scene_bytes(before->scene)+scene_bytes(after->scene)+2*sizeof(Camera)+2*sizeof(Settings);
    history.push_applied(std::move(label),[before,&scene,&camera,&settings](){restore_snapshot(*before,scene,camera,settings);},
        [after,&scene,&camera,&settings](){restore_snapshot(*after,scene,camera,settings);},bytes);
}
}
