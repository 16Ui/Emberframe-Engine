#pragma once
#include "types.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <memory>
#include <span>

namespace emberframe::lab {
struct SceneSdfGrid;

// set4：0 为每帧 std140 UBO，1 为世界空间距离格点 std430 float[]。
// 不拥有 pipeline；scene_resources.glsl 与 core shader 一起事务编译/替换。
class GpuSceneResources {
public:
    static constexpr std::uint32_t descriptor_set_index=4,frame_count=2;
    // CPU 不可变网格和 GPU buffer 共用一个 owner；上传环与渲染帧分别保活。
    class SdfUpload {
    public:
        ~SdfUpload();
        VkBuffer buffer() const noexcept;
        std::span<const std::byte> bytes() const noexcept;
    private:
        friend class GpuSceneResources;
        struct Impl;
        explicit SdfUpload(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
    GpuSceneResources(VkPhysicalDevice,VkDevice,VmaAllocator);
    ~GpuSceneResources();
    GpuSceneResources(const GpuSceneResources&)=delete;
    GpuSceneResources& operator=(const GpuSceneResources&)=delete;
    VkDescriptorSetLayout descriptor_layout() const noexcept;
    VkDescriptorSet descriptor_set(std::uint32_t frame_index) const;
    // 只分配 device-local buffer，不提交、不等待；调用方将 bytes 纳入原上传预算。
    // 返回值只能在上传 fence 完成、场景原子发布后交给 configure。
    std::shared_ptr<SdfUpload> prepare_sdf(std::shared_ptr<const SceneSdfGrid>);
    // 必须先等待该 frame slot fence；只替换该 slot 的 UBO/descriptor/owner。
    // prt_ready/sdf_ready 来自缓存有效性验证，不用旧烘焙伪装实时几何遮挡。
    void configure(const Scene&,const Settings&,std::uint32_t frame_index,
                   std::shared_ptr<SdfUpload>,bool prt_ready,bool sdf_ready);
    bool ready(const Settings&) const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace emberframe::lab
