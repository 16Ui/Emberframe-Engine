#pragma once
#include "types.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <memory>
#include <span>
#include <string_view>
#include <functional>
#include <cstddef>

namespace emberframe::lab {
struct GraphPlan;
class RenderGraph;
struct ResourceUse;
struct ResourceBarrier;
// 数组索引必须等于当前 GB[5].y；previous_object_id 对应上一帧 GB[5].y。
// 仅刚体/仿射对象变换，不代表蒙皮或顶点形变运动。history_valid=0 拒绝该对象历史。
struct alignas(16) EffectsObjectMotion {
    glm::mat4 current_to_previous_world{1};
    std::uint32_t previous_object_id=0,history_valid=0;
    std::uint32_t reserved0=0,reserved1=0;
};
static_assert(sizeof(EffectsObjectMotion)==80);
static_assert(offsetof(EffectsObjectMotion,previous_object_id)==64);
struct EffectsPassCallbacks {
    void* user=nullptr;
    // 回调只记录命令，不得抛异常/提交/等待；begin/end 包含该 Pass 的前后图像屏障。
    void (*begin)(void*,VkCommandBuffer,std::string_view) noexcept=nullptr;
    void (*end)(void*,VkCommandBuffer,std::string_view) noexcept=nullptr;
};
struct EffectsGraphImage {
    std::string resource;
    VkImage image{};
    VkImageView view{};
    std::uint32_t width=0,height=0;
};
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
    // 非空时必须提供 optional_meta；空表保留旧调用者的静态场景重投影。
    // 主线负责 previous_object_id 的持久对应关系，不能仅以本帧排序猜测对象身份。
    std::span<const EffectsObjectMotion> object_motion;
    EffectsPassCallbacks pass_callbacks;
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
    bool invalidate_history=false; // 相机切换、未提交帧、光照/资源拓扑变化；刚体运动不清整幅历史。
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
    std::size_t motion_upload_bytes=0,motion_objects=0;
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
    // description 只用于规划开关/extent/slot，不读取尚未产生的 volume 输出。
    // lazyInputFactory 仅在首个实际 Pass 执行时调用，返回与 occlusion 配对的完整输入。
    // externalResourceUses 按 position/normal/albedo/emission/HDR/baseline/tangent/meta/volume
    // 顺序传入 9 个槽位；可选槽位可用空 resource。可额外传第 10 个 AO 槽位，
    // 将本模块 ao 图像映射到外层已有 screen-AO 资源，避免同图异名。资源须已注册。
    // 每个真实 dispatch 单独进入同一个 graph；外层 execute 的 observer 已负责计时，
    // 此入口不再调用 Inputs.pass_callbacks，避免重复嵌套统计同一 Pass。
    // output 在规划时返回最终图像句柄，执行后内容才有效；调用方后续 tonemap/UI/present
    // 声明它的 shader_read 用途，并将内部资源屏障交给 record_graph_barrier。
    void add_post_passes(RenderGraph&,VkCommandBuffer,const Inputs& description,
                         std::function<Inputs()> lazyInputFactory,
                         std::span<const ResourceUse> externalResourceUses,Output& output);
    // 返回 false 表示不是本模块资源，由外层处理；不负责外部 G-buffer/HDR 图像。
    bool record_graph_barrier(VkCommandBuffer,const ResourceBarrier&);
    Output occlusion() const noexcept;
    Output indirect() const noexcept; // 最终间接项；用于 indirect debug，不含直接光。
    Output variance() const noexcept; // RGBA: variance, E[L], E[L²], history length。
    Output motion() const noexcept;   // RG: currentUV-previousUV（相机+有效刚体变换）。
    const EffectsDiagnostics& diagnostics() const noexcept;
    // 最近一次 record_post wrapper 的实际执行图（只作诊断，不能再次 execute）。
    // add_post_passes 集成后，整帧 plan 由外层 graph.compile() 获取。
    const GraphPlan& post_graph() const noexcept;
    std::span<const EffectsGraphImage> post_graph_images() const noexcept;
    std::string_view post_output_resource() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 显式 GPU 检查（会 queue-wait/readback）；与生产 record 完全分离。
// 可由主 CMake 的 GPU checks 调用，或以 EMBERFRAME_GPU_EFFECTS_SELF_TEST 独立构建。
TestResults test_gpu_effects(VkPhysicalDevice,VkDevice,VmaAllocator,VkQueue,
                           std::uint32_t queue_family,const std::filesystem::path& shaders);
} // namespace emberframe::lab
