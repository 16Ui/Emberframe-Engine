#pragma once
#include "types.h"
#include <atomic>
namespace emberframe::lab {
struct ReferenceFrameInput {
    // 真正作用于本帧采样：clip.xy += jitter_ndc*clip.w，NDC +Y 向上。
    // 与 TemporalInput::jitter_ndc 传入相同值。零值保持已有调用方式。
    glm::vec2 jitter_ndc{0};
    // 未提供时使用当前相机；Node::previous_world 仍决定物体的上一帧姿态。
    const Camera* previous_camera=nullptr;
    // false 把 NPR/bloom 延后给持有 TemporalFilter 的调用者；toon BRDF 本身
    // 仍返回 Lambert，色带只在 apply_npr 中发生，避免 session 重复处理。
    bool apply_postprocess=true;
    // 可选输出：CURRENT 像素对应表面在上一帧的世界位置，供 TemporalInput
    // 验证动态物体深度。背景为零；调用期间不能被其他线程访问或共用。
    Image<glm::vec3>* previous_world_positions=nullptr;
};
// 输出保持线性 HDR，供 GPU 最后统一曝光和编码；取消时允许返回未完成结果。
// motion = 当前未抖动像素 - 上一帧未抖动像素（+Y 向下），包含相机与物体运动。
// 搭配 MotionConvention::current_minus_previous_pixels_explicit，零运动也有效。
// 上一位置不可投影时输出越界运动，强制拒绝历史。这里不执行时域处理。
RenderOutput render_reference(const Scene&,const Camera&,const Settings&,std::atomic<bool>* cancel=nullptr);
RenderOutput render_reference(const Scene&,const Camera&,const Settings&,
                              const ReferenceFrameInput&,std::atomic<bool>* cancel=nullptr);
// 可在开始逐帧计时前调用。IBL 按环境值+revision 缓存，最多保留四项；LTC
// 全局只拟合一次。创建在主调线程完成，像素任务仅访问不可变共享数据。
// Scene 在整个调用期间必须保持只读；缓存不能使并发修改 Scene 本身变安全。
void prepare_reference_lighting(const Scene&,const Settings&);
// 光栅积分限制：IBL 为 Lambert + 各向同性 GGX split-sum；LTC 为 Lambert +
// 各向同性单次散射 GGX。Disney 附加层/各向异性与 Blinn 高光不在表的模型内，
// 使用基础 GGX 近似；不重复附加 KC。矩形遮挡采用 ShadowGi 的确定性可见率
// 乘 LTC，不能视为带 BRDF 权重的精确软阴影。path_trace 保留完整 BRDF、
// 面积采样/BSDF MIS 数值积分，不读取 IBL/LTC；显式最大深度仍产生截断偏差。
// 光栅使用统一 geometry::rasterize 的 alpha-mask 与 UV 导数，所有材质贴图
// 使用相同纹理足迹；路径追踪目前将三角形视为不透明双面（无 alpha mask）。
// 两条路径均不实现 alpha blend 或折射；阴影/GI 的遮挡也仍是几何不透明近似。
// PT G-buffer 是像素中心的几何引导；随机子像素样本在轮廓处可能与之不同。
// 有界 CPU 参考：总像素 <= 16M，samples [1,65536]，max_bounces [1,64]。
void save_bmp(const std::filesystem::path&,const Image<glm::vec3>& hdr,float exposure=1);
void save_pfm(const std::filesystem::path&,const Image<glm::vec3>& hdr);
TestResults test_reference();
}

