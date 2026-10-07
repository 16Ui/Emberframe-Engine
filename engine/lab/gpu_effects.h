#pragma once
#include "types.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <memory>
#include <span>

namespace emberframe::lab {
// 全部图像为同一 graphics+compute queue family；不做队列 ownership transfer。
// 输入图像必须带 SAMPLED usage，extent 与本模块一致。模块将其转换到只读布局；
// 调用方必须同步更新 RenderGraph 的状态，不可继续沿用旧 color-attachment 布局。
struct EffectsImageInput {
    VkImage image{};
    VkImageView view{};
    VkImageLayout layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkPipelineStageFlags2 stage=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkAccessFlags2 access=VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT;
    explicit operator bool() const noexcept { return image && view; }
};
struct EffectsInputs {
    EffectsImageInput position,normal_roughness,albedo_metallic,emission_ao,hdr;
    // 可选：Lighting 实际加到 HDR 的完整间接贡献（包括 material AO/GPU AO）。
    // 提供时 SSR/SSGI 在命中部分替换此基线；缺省时必须令 HDR 不含间接光，
    // 本模块使用 sky fallback 补齐间接光。不能传含 IBL 的 HDR 却省略此图。
    EffectsImageInput indirect_baseline;
    // 可选 GB[4]=world tangent+handedness；GB[5].xy=material ID,object ID。
    // 主渲染的 GB[5].zw 保存八面体编码的几何法线；effects 只用 xy 判断历史身份。
    // ID 必须是准确整数值（推荐 R32G32B32A32_SFLOAT）；历史按两个 ID 拒绝。
    EffectsImageInput optional_tangent,optional_meta;
    // GiMode::rsm/lpv/voxel：线性 outgoing 间接贡献，已做 BRDF/material 调制，
    // 尚未乘 material AO / screen AO。缺少相应图像时报错，不假装该 GI 已实现。
    EffectsImageInput volume_indirect;
    std::uint32_t width=0,height=0,frame_slot=0; // 0/1；复用前 caller 已等待对应 fence。
    Camera camera;
    Settings settings;
    std::uint64_t scene_revision=0;
    glm::vec3 sky_top{.25f,.35f,.55f},sky_bottom{.03f};
    glm::mat4 current_vp{1},previous_vp{1}; // 已包含 Y 翻转及实际 jitter。
    glm::vec2 current_jitter{0},previous_jitter{0}; // NDC，诊断实际值。
    bool invalidate_history=false; // 相机切换、node 变换、未提交帧、外部光照变化等。
};
struct EffectsOutput {
    VkImage image{};
    VkImageView view{};
    VkImageLayout layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkFormat format=VK_FORMAT_R32G32B32A32_SFLOAT;
    std::uint32_t width=0,height=0;
};
struct EffectsDiagnostics {
    std::vector<std::string> pass_names;
    std::size_t dispatches=0,barriers=0;
    std::size_t allocated_bytes=0;
    bool history_valid=false,baseline_replaced=false;
    std::string history_reset_reason;
    glm::vec2 jitter{0};
};

// Vulkan 1.3，仅 synchronization2；不需要可选 storage 格式、descriptor indexing、
// timeline、shaderInt64、shaderFloat64、subgroup 或 buffer-device-address 功能。
// 独立 set=0/layout；sampler 采用 nearest，重投影双线性由 shader 手工实现。
// resize/析构前 caller wait_idle；正常记录不等待、不回读 GPU。所有 record 必须
// 成对、按提交顺序调用并提交到同一 graphics queue；不支持并行录制/丢弃已录帧。
// 若丢弃帧，请 wait_idle + resize 重建 image layout tracking，不仅 invalidate_history。
class GpuEffects {
public:
    using Inputs=EffectsInputs;
    using Output=EffectsOutput;
    GpuEffects(VkPhysicalDevice,VkDevice,VmaAllocator,const std::filesystem::path& shaders,
               VkPipelineCache=VK_NULL_HANDLE);
    ~GpuEffects();
    GpuEffects(const GpuEffects&)=delete;
    GpuEffects& operator=(const GpuEffects&)=delete;
    class PreparedPipelines {
    public:
        ~PreparedPipelines();
        PreparedPipelines(const PreparedPipelines&)=delete;
        PreparedPipelines& operator=(const PreparedPipelines&)=delete;
    private:
        friend class GpuEffects;
        struct State;
        explicit PreparedPipelines(std::unique_ptr<State>);
        std::unique_ptr<State> state_;
    };
    // 主 hot-reload 事务：先 prepare 所有模块；全部成功后 caller wait_idle，再 commit。
    // 候选 token 不改变 live pipelines，可直接析构回滚；需比 GpuEffects 更早析构。
    std::unique_ptr<PreparedPipelines> prepare_pipelines(const std::filesystem::path&,
                                                       std::string* error=nullptr) const;
    bool commit_pipelines(std::unique_ptr<PreparedPipelines>) noexcept; // 已 wait_idle；token 属于本模块。
    bool reload_pipelines(const std::filesystem::path&,std::string* error=nullptr);
    static std::span<const char* const> shader_files() noexcept; // 不含公共 include。
    void resize(std::uint32_t width,std::uint32_t height);
    // 返回本次将要 record 的 Halton(2,3) NDC 偏移；仅 TAA 打开时应用。
    glm::vec2 jitter() const noexcept;
    static glm::mat4 jittered_projection(glm::mat4 projection,glm::vec2 ndc_jitter) noexcept;
    // record_occlusion 在 Lighting 前执行（即使 AO=none 也生成全白有效图像）。
    Output record_occlusion(VkCommandBuffer,const Inputs&);
    // 与同一帧 record_occlusion 配对。HDR 为线性颜色；此处不做曝光/ACES/sRGB。
    // 主集成需关闭原 post.frag 的局部 bloom，避免再次叠加。
    Output record_post(VkCommandBuffer,const Inputs&);
    Output occlusion() const noexcept;
    Output indirect() const noexcept; // 最终间接项；用于 indirect debug，不含直接光。
    Output variance() const noexcept; // RGBA: variance, E[L], E[L²], history length。
    Output motion() const noexcept;   // RG: currentUV-previousUV（相机+静态 world position）。
    const EffectsDiagnostics& diagnostics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 显式 GPU 检查（会 queue-wait/readback）；与生产 record 完全分离。
// 可由主 CMake 的 GPU checks 调用，或以 EMBERFRAME_GPU_EFFECTS_SELF_TEST 独立构建。
TestResults test_gpu_effects(VkPhysicalDevice,VkDevice,VmaAllocator,VkQueue,
                           std::uint32_t queue_family,const std::filesystem::path& shaders);
} // namespace emberframe::lab
