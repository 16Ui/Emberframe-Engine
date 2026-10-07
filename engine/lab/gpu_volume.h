#pragma once
#include "types.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <memory>
#include <string_view>
#include <span>

namespace emberframe::lab {
// 教学规模的真实 GPU GI：RSM 单次漫反射、l<=1 SH LPV、各向同性 VCT。
// 不需要 float atomics、descriptor indexing、storage image extended formats。
// SH 用每格单写者 gather 的 FP32 系数，无固定点量化/溢出范围问题。世界三角形
// 坐标限制 +/-100000；材质/顶点颜色限制有限 +/-1e6，light intensity/color 在 [0,1e6]。
// 只使用 lightIndex 指定的主光源：方向光单面，点/面积光六面 atlas；面积光为灯心辐射近似。
// 主光源之外的灯仍参与直接光，但不宣称全部参与体积 GI；无发光灯时注入为零。
// VCT 用不透明 base material+平均 vertex color；texture/alpha/方向性辐亮度不烘焙。
// 薄面扩张到整格；LPV occupancy 只能基础抑制穿墙；各向同性 mip 仍有漏光可能。
// VCT 稀疏模式：GPU 将所有非空 mip 格压紧到 SSBO，页表将 (mip,cell) 映射到节点。
// 保留稠密体素化/mip 中间态和节点容量预留，不宣称释放稠密图像或减少物理显存。
// 所有方法及共享 VkPipelineCache 外部串行；析构前 caller wait_idle，device/allocator 后销毁。
class GpuVolumeGi {
public:
    static constexpr std::uint32_t frame_slots=3;
    // 索引的数值上界，不再是场景质量限制；真实容量在 configure 按设备 SSBO range 核验。
    static constexpr std::uint32_t max_triangles=std::uint32_t(std::numeric_limits<int>::max()/2);
    static constexpr std::uint32_t max_rsm_samples=4096;
    // 全占据时的严格上界，包含 1^3 根格；这是容量，不是实时 active 统计。
    static constexpr std::uint32_t sparse_node_capacity(std::uint32_t resolution) noexcept {
        if(resolution!=16&&resolution!=32) return 0;
        std::uint32_t count=0;
        for(auto size=resolution;size;size>>=1) count+=size*size*size;
        return count;
    }
    static std::span<const char* const> shader_files() noexcept;
    inline static constexpr std::array<std::string_view,2> shader_includes={
        "volume_common.glsl","volume_geometry.glsl"};
    struct SampledImage {
        VkImage image=VK_NULL_HANDLE;
        VkImageView view=VK_NULL_HANDLE;
        VkImageLayout layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkExtent2D extent{};
        // 同一 queue family；record 显式把最近的生产者写入同步到 compute 读取。
        VkPipelineStageFlags2 stage=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkAccessFlags2 access=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    };
    struct Inputs {
        const Camera& camera;
        const Scene& scene;
        const Settings& settings;
        SampledImage position,normal,albedo; // world position.a=coverage；normal.a=roughness；albedo.a=metallic
        SampledImage rsmPosition,rsmNormal,rsmAlbedo; // 真正 light-view G-buffer，线性颜色
        glm::mat4 lightViewProjection{1}; // Vulkan NDC z=[0,1]，与 RSM raster 完全相同
        std::uint32_t lightIndex=0;
        std::uint32_t frameSlot=0; // 重用前 caller 必须等该 slot 的提交 fence 完成
        bool lightViewportYDown=true; // 正 viewport height：uv.y=ndc.y*.5+.5
        std::array<glm::mat4,6> lightFaceViewProjections{};
        std::uint32_t lightFaceCount=1; // 六面按 +X,-X,+Y,-Y,+Z,-Z 排列；单面仍用上面的 lightVP。
        bool rsmAtlas=false; // 工作台 atlas 固定六个横向 tile；独立诊断仍可传单幅图。
        bool cacheStaticTransport=false; // caller 保证 scene revision/光源参数覆盖所有 RSM 内容变化。
    };
    struct Output {
        VkImage image=VK_NULL_HANDLE;
        VkImageView view=VK_NULL_HANDLE;
        VkImageLayout layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkExtent2D extent{};
        // RGB 是已乘接收面 diffuse albedo 的线性 HDR 间接 Lo，post 直接加一次。
    };
    GpuVolumeGi(VkPhysicalDevice,VkDevice,VmaAllocator,std::filesystem::path shaders,
                VkPipelineCache=VK_NULL_HANDLE);
    ~GpuVolumeGi();
    GpuVolumeGi(const GpuVolumeGi&)=delete;
    GpuVolumeGi& operator=(const GpuVolumeGi&)=delete;
    // 在 beginCommandBuffer 之前调用。scene 身份/revision、输出尺寸或分辨率变化时
    // vkDeviceWaitIdle 后重配；常规帧只检查 key，不 CPU bake、不 GPU readback。
    // 分辨率仅 16 或 32；完整几何建立 stackless BVH，GPU 不再对每格扫描所有三角形。
    // RSM 不需要体素几何 SSBO；硬件容量不足、非法数据明确报错，绝不截断或转 CPU 渲染。
    void configure(const Scene&,VkExtent2D outputExtent,std::uint32_t resolution=32,GiMode mode=GiMode::voxel);
    struct RsmProjection {
        int lightIndex=-1;
        std::uint32_t faceCount=1;
        std::array<glm::mat4,6> viewProjection{};
    };
    // 和 volume Shader 一致的光源投影；不依赖当前相机，也不挪用直接光的阴影选择。
    static RsmProjection rsm_projection(const Scene&,glm::vec3 minimum,glm::vec3 maximum);
    struct AccelerationStats {
        std::uint64_t instancedTriangles=0,uploadedTriangles=0,bvhNodes=0,geometryBytes=0,stagingBytes=0;
        bool geometryBacked=false;
    };
    AccelerationStats acceleration_stats() const noexcept;
    // 只支持 rsm/lpv/voxel，其他模式输出黑色。必须先 configure，cmd 在 rendering 外。
    // set0 全独立；结束后调用方需重新绑定自己的 pipeline/descriptor sets。
    // 输入转为 SHADER_READ_ONLY_OPTIMAL；caller 须更新 RenderGraph 的状态。
    // 输出已同步到 fragment/compute sampled read。同 queue family，图像须有 SAMPLED usage。
    // record 要按顺序提交；丢弃已录制帧时 wait_idle 后重新 configure/重建模块。
    // GiMode::voxel 内部按 settings.sparse_voxels 选择稠密/SSBO 查询；GiMode 编号不变。
    Output record(VkCommandBuffer,const Inputs&);
    Output output(std::uint32_t frameSlot=0) const; // 首次 record 前 layout=UNDEFINED，不能直接采样。
    glm::vec3 bounds_minimum() const;
    glm::vec3 bounds_maximum() const;
    class PreparedPipelines {
    public:
        ~PreparedPipelines();
        PreparedPipelines(const PreparedPipelines&)=delete;
        PreparedPipelines& operator=(const PreparedPipelines&)=delete;
    private:
        friend class GpuVolumeGi;
        struct State;
        explicit PreparedPipelines(std::unique_ptr<State>);
        std::unique_ptr<State> state_;
    };
    // 与 GpuEffects 相同的主事务协议：prepare 全部模块 -> caller wait_idle -> commit。
    // token 析构即取消候选；应比本模块/device 更早析构。prepare 不修改 live 状态。
    std::unique_ptr<PreparedPipelines> prepare_pipelines(const std::filesystem::path&,
                                                       std::string* error=nullptr) const;
    bool commit_pipelines(std::unique_ptr<PreparedPipelines>) noexcept;
    // 路径是已编译的 <shader_files()[i]>.spv 所在目录。先创建全部候选，成功后
    // vkDeviceWaitIdle 再替换；失败旧 pipeline/descriptor/image/buffer 均保留。
    // 主事务需跨 core/effects/volume 原子切换时，可以保存旧目录并回调本方法 rollback。
    bool reload_pipelines(const std::filesystem::path& newDirectory,std::string* error=nullptr);
private:
    friend TestResults test_gpu_volume(VkPhysicalDevice,VkDevice,VmaAllocator,VkQueue,
                                      std::uint32_t,const std::filesystem::path&,VkPipelineCache);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// 显式诊断：使用 GPU clear 生成解析图像 fixture，执行真实 compute 并有限回读核对。
// 不替代 renderer 的真实 light-view raster 集成测试，也不是普通 frame 的 CPU GI 路径。
TestResults test_gpu_volume(VkPhysicalDevice,VkDevice,VmaAllocator,VkQueue,std::uint32_t queueFamily,
                           const std::filesystem::path& shaders,VkPipelineCache=VK_NULL_HANDLE);
} // namespace emberframe::lab
