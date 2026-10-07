#pragma once
#include "types.h"
#include <cstddef>

namespace emberframe::lab {
// GI 主光源独立于阴影主光源。只选真正发光的灯，不能在无灯场景凭空补一盏灯。
inline int volume_gi_primary_light(const Scene& scene) {
    for(auto kind:{LightKind::directional,LightKind::rectangle,LightKind::point})
        for(std::size_t i=0;i<scene.lights.size();++i) {
            const auto& light=scene.lights[i];
            if(light.kind==kind&&light.intensity>0&&
               (light.color.x>0||light.color.y>0||light.color.z>0))return int(i);
        }
    return -1;
}
// 只读配置事实；不依赖 ImGui/Vulkan，不改场景、参数或实际执行路径。
// 容量与当前工作台的 GPU 灯表、体积 GI 约束一致，便于独立自动测试。
struct ConfigApplicability {
    static constexpr std::size_t gpu_light_limit=64;
    // 旧 UI 字段保留源兼容；几何容量现由设备实际 storage-buffer range 判断。
    static constexpr std::size_t gpu_volume_triangle_limit=std::size_t(std::numeric_limits<int>::max()/2);
    std::size_t light_count=0,rectangle_light_count=0,instanced_triangles=0;
    int shadow_light_index=-1;
    bool has_directional_light=false;
    bool has_environment_input=false;
    bool has_texture_input=false;
    bool csm_supported=false,csm_unsupported=false;
    bool gpu_sdf_supported=false,gpu_sdf_unsupported=false;
    bool rectangle_center_shadow=false,rectangle_penumbra_approximate=false;
    float rectangle_physical_shadow_radius=0,rectangle_softness_multiplier=1;
    bool low_light_count=false;
    bool ao_enabled=false,ao_no_indirect_input=false,ao_final_color_unaffected=false;
    bool gpu_path_requested=false,volume_gi_selected=false;
    bool gpu_volume_missing_directional=false;
    bool gpu_volume_triangle_limit_exceeded=false;
    bool gpu_volume_resolution_unsupported=false;
    int gpu_volume_light_index=-1;
    bool gpu_volume_area_center_approximate=false,gpu_volume_point_cubemap=false;
    bool gpu_volume_primary_source_only=false,gpu_volume_no_emitting_light=false;
    bool gpu_light_limit_exceeded=false,gpu_fallback_expected=false;
    bool can_use_gpu_ssgi=false;
};

inline ConfigApplicability inspect_config(const Scene& scene,const Settings& settings) {
    ConfigApplicability result;
    result.light_count=scene.lights.size();
    result.gpu_path_requested=settings.path==RenderPath::forward||settings.path==RenderPath::deferred;
    result.low_light_count=result.light_count<=1;
    result.has_directional_light=std::any_of(scene.lights.begin(),scene.lights.end(),
        [](const Light& light){return light.kind==LightKind::directional;});
    // 不用亮度阈值把微弱天空误报为黑色；输入是场景天空，非烘焙缓存或直接光。
    auto emits=[](glm::vec3 value){return value.x>0||value.y>0||value.z>0;};
    result.has_environment_input=emits(scene.sky_top)||emits(scene.sky_bottom);
    result.csm_supported=result.has_directional_light;
    result.csm_unsupported=settings.shadows==ShadowMode::csm&&!result.csm_supported;
    // GPU SDF 当前只有方向射线；CPU 参考已有到灯心的有限距离查询。
    result.gpu_sdf_supported=result.has_directional_light;
    result.gpu_sdf_unsupported=result.gpu_path_requested&&settings.sdf_shadows&&!result.gpu_sdf_supported;
    int directional_index=-1,rectangle_index=-1;
    for(std::size_t i=0;i<scene.lights.size();++i){
        const auto kind=scene.lights[i].kind;
        if(kind==LightKind::rectangle)++result.rectangle_light_count;
        // 和 GPU 阴影源选择一致：灯表范围内优先第一盏方向光，再选第一盏矩形光。
        if(i>=ConfigApplicability::gpu_light_limit)continue;
        if(kind==LightKind::directional&&directional_index<0)directional_index=int(i);
        if(kind==LightKind::rectangle&&rectangle_index<0)rectangle_index=int(i);
    }
    result.shadow_light_index=directional_index>=0?directional_index:
        settings.shadows!=ShadowMode::csm?rectangle_index:-1;
    // 仅有未绑定的资源或无图像的纹理槽，不算可供过滤的采样输入。
    auto usable_texture=[&](int index){
        if(index<0||std::size_t(index)>=scene.textures.size())return false;
        const auto& levels=scene.textures[std::size_t(index)].levels;
        return !levels.empty()&&!levels.front().empty()&&!levels.front().pixels.empty();
    };
    for(const auto& material:scene.materials) {
        if(usable_texture(material.base_texture)||usable_texture(material.mr_texture)||
           usable_texture(material.normal_texture)||usable_texture(material.ao_texture)||
           usable_texture(material.emissive_texture)){result.has_texture_input=true;break;}
    }
    // 和 GPU 能力检查一样按实例计数；未挂入节点树的网格不计入节点场景。
    auto count_mesh=[&](const Mesh& mesh){
        if(mesh.primitives.empty())result.instanced_triangles+=(mesh.indices.empty()?mesh.vertices.size():mesh.indices.size())/3;
        else for(const auto& primitive:mesh.primitives)result.instanced_triangles+=primitive.index_count/3;
    };
    if(scene.nodes.empty())for(const auto& mesh:scene.meshes)count_mesh(mesh);
    else for(const auto& node:scene.nodes)
        if(node.mesh>=0&&std::size_t(node.mesh)<scene.meshes.size())count_mesh(scene.meshes[std::size_t(node.mesh)]);
    result.ao_enabled=settings.ao!=AoMode::none;
    // 路径追踪独立积分多次反弹，不能套用光栅环境光/AO 的无输入结论。
    result.ao_no_indirect_input=settings.path!=RenderPath::path_trace&&
        (settings.gi==GiMode::none||(settings.gi==GiMode::environment&&!result.has_environment_input));
    result.ao_final_color_unaffected=result.ao_enabled&&result.ao_no_indirect_input;
    result.volume_gi_selected=settings.gi==GiMode::rsm||settings.gi==GiMode::lpv||settings.gi==GiMode::voxel;
    result.gpu_volume_missing_directional=false; // 点光六面和面积光灯心投影已可实际注入。
    result.gpu_volume_triangle_limit_exceeded=result.volume_gi_selected&&
        result.instanced_triangles>ConfigApplicability::gpu_volume_triangle_limit;
    result.gpu_volume_resolution_unsupported=result.volume_gi_selected&&
        settings.voxel_resolution!=16&&settings.voxel_resolution!=32;
    result.gpu_volume_light_index=volume_gi_primary_light(scene);
    if(result.volume_gi_selected) {
        result.gpu_volume_no_emitting_light=result.gpu_volume_light_index<0;
        if(result.gpu_volume_light_index>=0) {
            const auto kind=scene.lights[std::size_t(result.gpu_volume_light_index)].kind;
            result.gpu_volume_area_center_approximate=kind==LightKind::rectangle;
            result.gpu_volume_point_cubemap=kind==LightKind::point;
        }
        result.gpu_volume_primary_source_only=std::count_if(scene.lights.begin(),scene.lights.end(),
            [&](const Light& light){return light.intensity>0&&emits(light.color);})>1;
    }
    result.gpu_light_limit_exceeded=result.light_count>ConfigApplicability::gpu_light_limit;
    // 能力约束的预期回退；实际执行仍以渲染器 unsupported_modes 为准。
    // 不改写 CSM/SDF；主代理可用同一标志为加载/脚本设置加显式守卫。
    result.gpu_fallback_expected=result.gpu_path_requested&&
        (result.csm_unsupported||result.gpu_sdf_unsupported||result.gpu_light_limit_exceeded||result.gpu_volume_missing_directional||
         result.gpu_volume_triangle_limit_exceeded||result.gpu_volume_resolution_unsupported);
    // 完整配置可执行时才报告 GPU 面积阴影；请求 GPU 但回退 CPU 不算启用。
    result.rectangle_center_shadow=result.gpu_path_requested&&!result.gpu_fallback_expected&&
        directional_index<0&&result.shadow_light_index>=0;
    result.rectangle_penumbra_approximate=result.rectangle_center_shadow&&
        (settings.shadows==ShadowMode::pcss||settings.shadows==ShadowMode::vssm);
    if(result.rectangle_center_shadow){
        const auto size=scene.lights[std::size_t(result.shadow_light_index)].size;
        result.rectangle_physical_shadow_radius=.5f*std::max(std::abs(size.x),std::abs(size.y));
        // 0.15 对应实际矩形最大半边长；参数只调整半影估计的软化倍率。
        result.rectangle_softness_multiplier=std::clamp(settings.light_size/.15f,0.f,5.f);
    }
    // 已加载的不适用 CSM 必须由用户显式改选，SSGI 按钮不能暗改阴影模式。
    // 从 CPU 参考切到 GPU 时，同样检查仍保留的 SDF 选项。
    result.can_use_gpu_ssgi=!result.gpu_light_limit_exceeded&&!result.csm_unsupported&&
        !(settings.sdf_shadows&&!result.gpu_sdf_supported);
    return result;
}
} // namespace emberframe::lab
