#pragma once
#include <array>
#include <string>
#include <string_view>

namespace emberframe::lab::editor {
// 演示只选场景和对比参数，不复制一套算法。实际计算仍走正常渲染器。
struct DemoDescription {
    const char* profile;
    const char* name;
    const char* observe;
    const char* first;
    const char* second;
};
inline constexpr std::array<DemoDescription,5> demo_scenes{{
    {"pbr","材质与环境光",
     "观察金属/非金属与不同粗糙度的反射；切到白炉后，用同一粗糙金属对比 KC 能量补偿。",
     "A  关闭能量补偿","B  开启能量补偿"},
    {"shadows","阴影与半影",
     "观察遮挡物与接收面之间的阴影边缘：硬阴影与 PCSS 的接触、远离遮挡物时的半影变化。其他阴影算法在渲染设置中选择。",
     "A  硬阴影","B  PCSS 半影"},
    {"lights","多光源与灯表",
     "对比全部灯与 Clustered 聚簇候选灯；最终颜色应接近一致。可用灯数调试视图观察候选灯分布，耗时是否改善取决于场景。",
     "A  全部灯","B  聚簇灯表"},
    {"temporal","屏幕空间间接光",
     "同一相机下比较环境光与 SSGI：观察墙角和接触面的间接光。可在渲染设置启用 TAA 后移动相机，观察稳定性及历史误差。",
     "A  环境光","B  SSGI"},
    {"geometry","几何与 LOD",
     "先生成示例网格的 LOD 链，再比较 LOD0 与自动 LOD；拉远相机，观察三角形数与法线轮廓。没有 LOD 链时不会伪造减面。",
     "A  固定 LOD0","B  自动 LOD"},
}};
inline std::string gpu_issue_text(std::string_view reason) {
    if(reason.find("at most 64 lights")!=reason.npos)return "GPU 实时渲染最多支持 64 盏灯；请减少光源。";
    if(reason.find("CSM requires")!=reason.npos)return "CSM 需要方向光；请添加方向光或选择其他阴影方式。";
    if(reason.find("SDF visibility requires")!=reason.npos)return "GPU 距离场阴影需要方向光；请关闭该选项或添加方向光。";
    if(reason.find("resolution must be 16 or 32")!=reason.npos)return "GPU 体积网格只支持 16³ / 32³；请调整网格大小。";
    return std::string(reason);
}
}
