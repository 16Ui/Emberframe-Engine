#pragma once
#include "types.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <memory>

namespace emberframe::lab {
// SET=3: 0 UBO，1 diffuse irradiance，2 specular roughness mip levels，
// 3 BRDF A/B，4 LTC inverse x/z block，5 LTC inverse yy + A/B + fit error。
// 纹理均为普通 2D；手工双线性采样不要求 float32 linear filtering/descriptor indexing。
class GpuLighting {
public:
    static constexpr std::uint32_t descriptor_set_index=3,frame_count=2,light_limit=64;
    GpuLighting(VkPhysicalDevice,VkDevice,VmaAllocator,VkQueue,std::uint32_t queue_family,
                const std::filesystem::path& shaders,VkPipelineCache=VK_NULL_HANDLE);
    ~GpuLighting();
    GpuLighting(const GpuLighting&)=delete;
    GpuLighting& operator=(const GpuLighting&)=delete;
    VkDescriptorSetLayout descriptor_layout() const noexcept;
    VkDescriptorSet descriptor_set(std::uint32_t frame_index) const;
    // 调用前必须等待该 frame slot 的渲染 fence；仅更新该 slot 的 UBO/descriptor。
    // 与主渲染共用队列：在同一主线程调用，提交之间需遵守 Vulkan 外部同步。
    // 后台任务只持有天空颜色值。旧纹理由两个 frame slot 分别保活，普通帧不重烘焙。
    void configure(const Scene&,const Settings&,std::uint32_t frame_index);
    // 本模块没有 VkPipeline，GLSL 是 include-only，所有使用它的 core shader 由主事务编译。
    // 校验新目录后等待 device idle，再无异常提交目录；失败保持旧目录和全部资源。
    bool reload_pipelines(const std::filesystem::path& new_directory,std::string* error=nullptr);
    static constexpr std::array<const char*,1> shader_files{"gpu_lighting.glsl"};
    // 当前天空与预过滤结果一致时 true；变化期间继续使用上一套完整 IBL。
    bool environment_ready() const noexcept;
    std::uint64_t environment_bake_count() const noexcept;
    VkFormat texture_format() const noexcept;
    // 析构前调用方须保证两个渲染 slot 已完成；本类只等待自己的未完成上传。
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// GLSL 使用约定（gpu_lighting.glsl 可独立 include，无 common/surface 依赖）：
// gpu_disney_brdf(n,v,l,albedo,rough,metal,tangent,vec4(coat,coatRough,aniso,sheen))
// 返回 BRDF，不含 N.L。需要原 KC 时在 include 前定义 GPU_LIGHTING_KULLA 为 kulla。
// gpu_prefiltered_environment(n,v,albedo,rough,metal) 返回间接光，调用方乘 AO 一次。
// gpu_area_light(index,p,n,v,albedo,rough,metal,tangent,vec4(coat,coatRough,aniso,sheen))
// 返回矩形完整积分；调用方仅乘可见率，不再乘 N.L/面积/距离衰减。
// int/uint 均可；旧七参数重载委托退化 tangent 与 vec4(0,.15,0,0) 中性 lobes。
// index 保持原 Scene::lights 的下标（前 64 盏），非 rectangle 自动返回零。
// IBL 基底为共享 isotropic split-sum；energy ON 额外加 KC missing-energy closure。
// 恒定环境中 closure 与白炉积分等价，方向性天空用 diffuse irradiance 近似其卷积。
// 面积光按 ShadingMode 选择：PBR 保留 GGX LTC 粗表拟合，仅用 8x8 Gauss 补 KC；
// Blinn/Disney 用同一 8x8 矩形求积积分实际 BRDF，Toon 用解析 Lambert。
// isotropicKC 仅指各向同性 GGX：各向异性 Disney/Blinn 不加；清漆层不独立补偿。
// 生产 KC 使用 include 前的共享宏；独立 include 从 BRDF A/B/Eavg 表补面积光 KC。
// 尖锐高光/近灯会欠采样；IBL 仍为 isotropic split-sum，未实现完整 Disney IBL。
// 均不包含遮挡与软阴影，面积积分与中心 shadow map 的可见率由调用方组合。
} // namespace emberframe::lab
