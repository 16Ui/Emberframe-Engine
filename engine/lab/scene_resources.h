#pragma once
#include "types.h"
#include <atomic>
#include <memory>

namespace emberframe::lab {

using Sh9 = std::array<float,9>;
using EnvironmentSh9 = std::array<glm::vec3,9>;
struct ScenePrtInstance {
    // 世界空间静态 transfer；node=-1 表示无节点场景的恒等 mesh 实例。
    int node=-1,mesh=-1;
    // 与源 mesh.vertices 一一对应，不含材质或天空；法线贴图不改变几何 transfer 法线。
    std::vector<Sh9> transfer;
};
struct ScenePrtResources { std::vector<ScenePrtInstance> instances; };
struct SceneSdfGrid {
    glm::vec3 min{0},max{0};
    int resolution=0; // 各轴格点数；values[(z*resolution+y)*resolution+x]。
    // 开放、非流形网格没有可靠内外符号，统一保存真实三角面 unsigned 距离。
    bool unsigned_distance=true;
    std::vector<float> values;
};
struct SceneBakeResources {
    std::uint64_t geometry_hash=0,environment_hash=0;
    int bake_samples=0;
    EnvironmentSh9 environment_sh9{}; // 环境辐亮度 SH，PRT 已含 cosine 积分。
    // 不可变大数组可独立复用：天空变化只生成小资源壳，PRT/SDF 不重烘焙。
    std::shared_ptr<const ScenePrtResources> prt;
    std::shared_ptr<const SceneSdfGrid> sdf;
};

// 指纹来自实际顶点/索引/实例变换；排除 revision、材质、baked 属性及缓存。
std::uint64_t geometry_fingerprint(const Scene&);
// 预算不足/非法输入抛异常；取消返回 nullptr，未完成烘焙不会写入缓存。
// 内部有界共享缓存；Scene 在调用期间必须保持只读。
std::shared_ptr<const SceneBakeResources> prepare_scene_resources(
    const Scene&,const Settings&,std::atomic<bool>* cancel=nullptr);
// 验证指纹后按实例复制 mesh、写入烘焙照度，源场景完全只读。
Scene apply_scene_resources(const Scene&,std::shared_ptr<const SceneBakeResources>,const Settings&);
// 有版本/长度/校验和的二进制 string；坏数据抛 invalid_argument。
std::string serialize_scene_resources(const SceneBakeResources&);
std::shared_ptr<const SceneBakeResources> deserialize_scene_resources(const std::string&);

glm::vec3 evaluate_environment_sh(const EnvironmentSh9&,glm::vec3 normal);
glm::vec3 evaluate_prt_irradiance(const EnvironmentSh9&,const Sh9& transfer);
// 插值结果减去格点覆盖半径，得到保守距离下界，避免开放网格假符号。
float sample_scene_sdf(const SceneSdfGrid&,glm::vec3 position);
float scene_sdf_visibility(const SceneSdfGrid&,glm::vec3 position,glm::vec3 normal,
                           glm::vec3 direction,float max_distance,float softness,float bias);
TestResults test_scene_resources();

} // namespace emberframe::lab
