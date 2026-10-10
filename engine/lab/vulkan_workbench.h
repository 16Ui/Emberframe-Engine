#pragma once
#include "types.h"
#include "systems.h"
#include "frame_profile.h"
#include <memory>
#include <optional>

struct SDL_Window;
union SDL_Event;

namespace emberframe::lab {
struct WorkbenchStats {
    FrameProfile completed_profile; // 只在对应 Fence 与 timestamp 完成后发布。
    // GPU timestamp interval includes scene, postprocess and UI, excludes screenshot transfer.
    // A negative value means timestamps are unavailable or no completed frame exists yet.
    double gpu_ms=-1;
    std::uint64_t gpu_sample_serial=0; // 与 gpu_ms 配对的已完成提交，不重复采样 UI 帧。
    bool timestamp_available=false;
    std::uint64_t graph_serial=0,graph_revision=0;
    bool graph_submitted=false;
    std::size_t effect_dispatches=0;
    bool environment_ready=false,temporal_history_valid=false;
    std::vector<std::string> effect_passes;
    // triangles/visibility are from the most recently completed indirect-buffer readback
    // (primary camera + shadow geometry, excluding auxiliary capture G-buffer passes).
    // draw_calls counts commands recorded for the current frame, including fullscreens.
    std::uint64_t triangles=0,draw_calls=0;
    // VMA allocation bytes, excluding swapchain and ImGui's own allocations.
    std::size_t allocated_bytes=0;
    std::string gpu_name;
    int render_width=0,render_height=0,present_width=0,present_height=0;
    float display_scale=1,font_raster_scale=1;
    std::filesystem::path last_screenshot;
    std::string screenshot_error;
    std::string shader_error;
    std::uint64_t candidate_objects=0,visible_objects=0;
    std::size_t graph_passes=0,graph_barriers=0;
    bool uploadPending=false;
    std::size_t uploaded=0,total=0; // GPU fence-confirmed bytes / requested GPU payload.
    std::size_t uploadSubmitted=0,snapshotCopied=0,snapshotTotal=0;
    std::size_t uploadBudget=4*1024*1024;
    std::string uploadStage="idle",uploadError;
    std::uint64_t displayedRevision=0;
    std::uint64_t displayedAssetRevision=0; // 与姿态无关的静态资源版本，用于连续运动时的就绪判定。
    bool scene_resources_ready=false; // 所选 SH/PRT/SDF 已准备并随当前场景上传完成。
    bool scene_resources_pending=false; // 主机快照、后台烘焙或 GPU 发布仍在进行。
    std::string scene_resources_error; // 独立错误，不与旧场景上传成功状态混淆。
    std::uint64_t scene_resources_geometry_hash=0; // 当前有效烘焙的世界几何指纹。
};
struct FrameReadback {
    Image<glm::vec3> image;
    DebugView requested=DebugView::final_color;
    RenderPath path=RenderPath::forward;
    std::uint64_t serial=0;
};

// Vulkan 1.3 workbench; SDL_Window must outlive this object. Main thread only.
// Owns a separate ImGui context and its SDL2/Vulkan backends. Pass SDL_WINDOW_VULKAN.
// Scene is letterboxed to the drawable area remaining after optional editor insets.
// Camera aspect = actual offscreen render_width/render_height, identical to CPU reference.
// Geometry/material/texture changes require incrementing Scene::revision. Node transforms,
// lights, sky and camera are read each frame. Never destroy/mutate input during draw().
// Replacements are copied incrementally, uploaded through a shared fence-backed staging
// ring, then atomically published. Pending/failed replacements retain the old committed
// snapshot (or a placeholder). Never a background borrowed read of the caller's Scene.
// Host preparation has a soft 2 ms deadline per phase; single driver allocations or string
// copies can exceed it. Upload submission is strictly byte-budgeted; no per-upload waits.
// shader_dir contains <shader filename>.spv produced by VULKAN_BUILD.cmake.
// Native VkPipelineCache is shared by graphics/compute/ImGui and stored in user cache/;
// vendor/device/driver/UUID/ABI + checksum are validated. Bad data rebuilds automatically.
// 实际 GPU：前向/七目标 G-buffer 延迟、GGX/Disney/KC、TBN、软件各向异性过滤、
// LTC 矩形光、预过滤 IBL、PCSS/三层 CSM/VSM+SAT/VSSM/MSM、SSAO/GTAO、
// SSR/SSGI、RSM/LPV/VCT、TAA/HW5/SVGF 基础版、多级 Bloom 和 NPR。
// 16x16 tiled / 16x16x16 clustered 灯表最多 64 盏；输出 ACES 后编码 sRGB 一次。
// Compute tests camera/light frustum spheres and writes indexed indirect commands;
// one indirect draw per primitive (no optional multiDrawIndirect/device-address feature).
// Scene passes use RenderGraph::compile/execute to emit actual Vulkan barriers.
// 边界：透明排序而非 OIT；没有 MSAA、硬件光追或蒙皮运动。
// 体积 GI 通过 GPU BVH 处理完整几何，受设备内存/SSBO 限制；网格为 16/32。
// 当前使用一盏主灯：方向光、点光六面或面积光灯心近似；LPV 为 l<=1 SH，VCT 各向同性。
// LTC 粗表、有限采样、屏幕外信息缺失与未分离漫/镜面的降噪均有近似误差。
// IBL 支持解析天空和外部 Radiance HDR；背景任务共享不可变环境，GPU 每帧查表。
// 运动向量重投影相机+持久对象刚体/仿射变换；新显露区域拒绝历史，不冒充蒙皮 motion。
// Kulla-Conty uploads the real shared default_energy_lut() and uses its endpoint grid.
// Finite environment samples and LUT resolution are numerical approximation limits.
// Texture wrap modes are honored. FilterMode::trilinear honors imported glTF min/mag
// filters; nearest/bilinear explicitly override filtering for the lab comparison.
// 多级 Bloom 在 compute 中执行；post.frag 的旧局部 Bloom 关闭，避免叠加两次。
// unsupported_modes() lists unsupported real-time settings. draw() pauses scene execution
// and retains the last GPU output plus live UI; it never starts an implicit CPU renderer.
// Explicit CPU paths display set_reference_image() directly.
class VulkanWorkbench {
public:
    VulkanWorkbench(SDL_Window*,const std::filesystem::path& shader_dir);
    ~VulkanWorkbench();
    VulkanWorkbench(const VulkanWorkbench&)=delete;
    VulkanWorkbench& operator=(const VulkanWorkbench&)=delete;
    void process_event(const SDL_Event&);
    void resize();
    // Drawable framebuffer pixels; default 0. Applies to GPU and CPU-image presentation
    // on the next draw, including calls from ui(). Does not alter projection/readback.
    // If the sidebar fills the drawable width, only UI is drawn over the clear color.
    void set_viewport_inset_left(std::uint32_t pixels) noexcept;
    // 四边留白以 drawable 像素计。编辑器将逻辑坐标换算后传入，避免高 DPI 下遮住画面。
    // 只影响最终呈现区域，不改变投影、离屏分辨率或数值读回。
    void set_viewport_insets(std::uint32_t left,std::uint32_t right,std::uint32_t top,std::uint32_t bottom) noexcept;
    // Per draw transfer cap, rounded down to 4 bytes; range 64 KiB..12 MiB, default 4 MiB.
    // Shared ring has 3 x 4 MiB slices. Busy ring/host preparation can submit less.
    void set_upload_budget(std::size_t bytes);
    // 开发者 A/B：只改变包围球计算位置，不改变剔除或绘制结果。
    void set_bounds_cache_enabled(bool enabled) noexcept;
    // Starts ImGui, calls ui(), then reads scene/settings (ui may change them), draws/presents.
    // False means minimized/out-of-date and no image was presented. Throws on real errors.
    bool draw(const Scene&,const Camera&,const Settings&,const std::function<void()>& ui);
    const WorkbenchStats& stats() const;
    // 自适应分辨率需同时符合灯表范围与保守显存预算；限制触发时由 UI 明示。
    std::uint64_t max_render_tiles() const noexcept;
    std::uint64_t max_render_pixels() const noexcept;
    std::uintptr_t application_icon_texture() const noexcept;
    // 最近一次真实场景执行图；只读诊断快照，不可将它交给别的 RenderGraph 执行。
    const GraphPlan& render_graph() const;
    // 主线程只读取得最近完成的不可变 CPU 烘焙；pending 时可能仍是上一场景。
    std::shared_ptr<const SceneBakeResources> scene_bake_resources() const;
    void wait_idle();
    // Copies linear HDR image. Each fence-retired frame slot reuses its own Image/staging;
    // transfer and transfer->sample barriers are submitted with the normal frame, not an idle wait.
    // Empty clears the reference. No second exposure/tone mapping
    // if debug!=final_color; otherwise applies exposure/ACES. Always bypasses GPU bloom.
    void set_reference_image(const Image<glm::vec3>&);
    // The next successful frame is copied AFTER UI composition, fence-waited, and saved
    // as 24-bit BMP regardless of filename suffix. Use .bmp. No external dependencies.
    // Reports completion/failure through stats().last_screenshot / screenshot_error.
    void request_screenshot(const std::filesystem::path&);
    // Reads linear HDR AFTER GPU effects but BEFORE exposure/tone mapping/UI, or opaque/masked albedo
    // (zero background; transparent surfaces are not in G-buffer). GPU frames only;
    // forward albedo requests add a G-buffer pass. Completion fence-waits that frame.
    // take_frame_readback consumes the result; nullopt means no completed request.
    // Pending requests also wait for the requested scene revision to commit (inspect
    // uploadError if it fails), and survive minimized/CPU frames. Readback includes copy cost
    // in gpu_ms and should be excluded from normal rendering performance comparisons.
    void request_frame_readback(DebugView requested=DebugView::final_color);
    std::optional<FrameReadback> take_frame_readback();
    // Main thread only. SPIR-V must preserve this workbench's descriptor/push interfaces.
    // Builds all candidate pipelines first; waits idle and replaces only on total success.
    // Failure keeps the old pipelines/path and reports error without faulting the app.
    bool reload_pipelines(const std::filesystem::path& new_spv_dir,std::string* error=nullptr);
    static std::vector<std::string> unsupported_modes(const Settings&);
    static std::vector<std::string> unsupported_modes(const Scene&,const Settings&);
    static std::string unsupported_reason(const Settings&);
private:
    friend TestResults test_vulkan_uploads(VulkanWorkbench&);
    friend TestResults test_vulkan_scene_resources(VulkanWorkbench&);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Deterministic CPU checks of the exact host math/upload contracts used by the workbench.
// Does not pretend to validate a GPU: windowed Vulkan validation is an integration test.
TestResults test_vulkan_workbench();
// Explicit real-GPU ring/cache/scene diagnostic. Renders temporary test scenes, changes
// targets and may wait for its own readbacks. Call in a dedicated test workbench only.
TestResults test_vulkan_uploads(VulkanWorkbench&);
// 真实前向/延迟 HDR 读回：SH 常量、PRT 遮挡、SDF blocker/lit 与缓存更新。
TestResults test_vulkan_scene_resources(VulkanWorkbench&);
// 彩色房间配置回归：真实 GPU HDR A/B，阴影遮挡/材质/KC/AO 与前向延迟一致性。
TestResults test_vulkan_colored_room(VulkanWorkbench&,const std::filesystem::path& evidence={});
} // namespace emberframe::lab
