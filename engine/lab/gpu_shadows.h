#pragma once
#include "types.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstddef>
#include <memory>
#include <span>

namespace emberframe::lab {
// 与 gpu_shadow_sampling.glsl 的 std140 完全相同；光源深度始终普通 Z。
struct alignas(16) GpuShadowUniform {
    std::array<glm::mat4,4> light_vp{glm::mat4(1),glm::mat4(1),glm::mat4(1),glm::mat4(1)};
    glm::mat4 camera_view{1};
    glm::vec4 split_far{1}; // 三个 camera-linear 级联远平面；w=near
    glm::ivec4 controls{0,1,-1,256}; // ShadowMode, cascade count, light index, resolution
    glm::vec4 tuning{0.002f,1e-7f,0,1e-5f}; // world bias, min variance, bleed, MSM bias
    std::array<glm::vec4,4> cascade{}; // depth span, world texel, penumbra scale, search radius
    // x>0 表示面积光中心的透视阴影：near / far / tan(halfFov) / 等效光源半径。
    // 深度图仍存普通投影 Z；矩/SAT 与软阴影距离计算使用线性光源深度。
    glm::vec4 area_projection{0};
    glm::mat4 plane_transform{1}; // inverse-transpose(lightVP)，将世界平面变为裁剪平面。
};
static_assert(sizeof(GpuShadowUniform)==512);
static_assert(offsetof(GpuShadowUniform,camera_view)==256&&offsetof(GpuShadowUniform,split_far)==320&&
    offsetof(GpuShadowUniform,controls)==336&&offsetof(GpuShadowUniform,tuning)==352&&offsetof(GpuShadowUniform,cascade)==368&&
    offsetof(GpuShadowUniform,area_projection)==432&&offsetof(GpuShadowUniform,plane_transform)==448);
struct GpuShadowTarget {
    VkImage image{};
    VkImageView view{};
    VkFormat format=VK_FORMAT_UNDEFINED;
    std::uint32_t width=0,height=0;
    VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
};

// 纯 CPU 投影构造，方便主代理的测试检查。worlds 对应 nodes；无 nodes 时忽略。
// 优先选择前 64 个灯中的第一个 directional，否则选第一个 rectangle。
// 面积光用中心透视深度图近似可见性，并非整个发光面的精确积分；CSM 仅支持方向光。
GpuShadowUniform make_gpu_shadow_uniform(const Scene&,const Camera&,const Settings&,
    std::span<const glm::mat4> worlds,std::uint32_t resolution);

// device/allocator/graphics queue 由 caller 持有，必须比本对象活得更久。
// 仅依赖 Vulkan 1.3 dynamicRendering/synchronization2，不启用任何额外 feature。
// 两个 frameIndex 各有独立 UBO；调用 configure 前必须回收对应 FrameSlot fence。
// 图像共享：所有 prepare/draw/finish/sample 必须在同一 graphics+compute queue
// 按录制顺序提交，不能在另一个 queue 上读写。不能取消已记录的布局转换后继续复用。
// 析构、resize 和 shader 重建前 caller 必须 wait_idle（本类不隐式等待/提交）。
class GpuShadowSystem {
public:
    static constexpr std::uint32_t frame_count=2,max_cascades=3;
    GpuShadowSystem(VkPhysicalDevice,VkDevice,VmaAllocator,VkQueue,std::uint32_t family,
        const std::filesystem::path& shaders,VkPipelineCache cache=VK_NULL_HANDLE);
    ~GpuShadowSystem();
    GpuShadowSystem(const GpuShadowSystem&)=delete;
    GpuShadowSystem& operator=(const GpuShadowSystem&)=delete;
    VkDescriptorSetLayout descriptor_layout() const noexcept;
    VkDescriptorSet descriptor_set(std::uint32_t frameIndex) const;
    // 显式分配，分辨率限制 [32,2048]；configure 不重分配，不改别的帧描述符。
    void resize(std::uint32_t resolution);
    std::uint32_t resolution() const noexcept;
    void configure(const Scene&,const Camera&,const Settings&,
        std::span<const glm::mat4> worlds,std::uint32_t frameIndex);
    std::uint32_t cascade_count() const noexcept;
    int light_index() const noexcept;
    const GpuShadowUniform& uniform(std::uint32_t frameIndex) const;
    GpuShadowTarget depth_target(std::uint32_t cascade) const;
    // 只记录 barrier，caller 用每个 target 的 view 开启 dynamic rendering：
    // depth clear=1/store=STORE/compare=LESS_OR_EQUAL，正高度 viewport。
    // 必须 draw_items 逐级绘制所有投影遮挡物，不能沿用 camera cull 的 instanceCount。
    void prepare(VkCommandBuffer);
    // prepare 的逐级集成入口；第一次调用自动 prepare，后续级联检查同一 cmd。
    // 必须在 vkCmdBeginRendering 之前调用，不能在 rendering scope 内调用。
    void prepare_before_depth(std::uint32_t cascade,VkCommandBuffer);
    void finish(VkCommandBuffer,std::uint32_t frameIndex);
    class PreparedPipelines {
    public:
        ~PreparedPipelines();
        PreparedPipelines(const PreparedPipelines&)=delete;
        PreparedPipelines& operator=(const PreparedPipelines&)=delete;
    private:
        friend class GpuShadowSystem;
        struct State;
        explicit PreparedPipelines(std::unique_ptr<State>);
        std::unique_ptr<State> state_;
    };
    // prepare 不等待、不修改旧 pipeline；丢弃 token 自动回滚。
    // commit 前 caller 必须统一 wait_idle，token 必须比本模块早析构。
    std::unique_ptr<PreparedPipelines> prepare_pipelines(const std::filesystem::path& newDirectory,std::string* error=nullptr) const;
    bool commit_pipelines(std::unique_ptr<PreparedPipelines>) noexcept;
    // 单模块快捷事务：候选完整成功后才 vkDeviceWaitIdle + commit。
    bool reload_pipelines(const std::filesystem::path& newDirectory,std::string* error=nullptr);
    static std::span<const char* const> shader_files() noexcept; // 不含 include .glsl
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

TestResults test_gpu_shadow_formulas();
// 可选显式诊断：独立真实三角形深度绘制、矩/SAT 计算、visibility GPU 回读。
// caller 必须确保 queue 空闲且外部同步；无需修改 workbench Impl。
TestResults test_gpu_shadows(VkPhysicalDevice,VkDevice,VmaAllocator,VkQueue,
    std::uint32_t family,const std::filesystem::path& shaders,VkPipelineCache cache=VK_NULL_HANDLE);
} // namespace emberframe::lab
