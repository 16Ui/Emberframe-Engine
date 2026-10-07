#pragma once
#include "systems.h"

namespace emberframe::lab {

enum class TextureRole { base_color, metallic_roughness, normal, occlusion, emissive };

// 编辑器入口的固定预算；超限明确抛异常，不截断资产或静默降低质量。
struct EditorAssetLimits {
    static constexpr std::size_t file_bytes = 64 * 1024 * 1024;
    static constexpr std::size_t obj_text_bytes = 32 * 1024 * 1024;
    static constexpr std::size_t dependency_bytes = 128 * 1024 * 1024;
    static constexpr std::size_t vertices = 1024 * 1024;
    static constexpr std::size_t indices = 3 * 1024 * 1024;
    static constexpr std::size_t records = 16384;
    static constexpr std::size_t face_corners = 256;
    static constexpr std::size_t base_pixels = 8 * 1024 * 1024;
    static constexpr std::size_t mip_pixels = 16 * 1024 * 1024;
    static constexpr int image_dimension = 8192;
};

// glTF/GLB 委托现有静态加载器并验证编辑器预算；OBJ/MTL 支持简单平面凹多边形、
// 正/负索引、平面/平滑/显式法线、UV 导数切线（非 MikkTSpace）。FBX 明确拒绝。
// MTL 支持 Kd/Ke/d/Tr/Ns/Pr/Pm、map_Kd/map_Ke/map_d、norm、bump、
// map_Pr/map_Pm、map_ORM/map_metallic_roughness、map_ao；不能表示的功能报告警告或错误。
LoadedSceneAsset load_editor_model(const std::filesystem::path& path);

// PNG/JPEG/TGA/BMP，保留原始归一化通道；base/emissive 为 sRGB，其他角色为线性。
// 颜色在解码后的线性空间生成 NPOT mip；法线 mip 重新归一化，默认重复/三线性采样。
Texture load_editor_texture(const std::filesystem::path& path, TextureRole role);

// 先校验、复制并重映射，再 noexcept 提交；任何异常保持 destination 完全不变。
// 导入节点挂到 source.name 命名的恒等根节点；无显式节点的网格转为等价实例。
// 材质、纹理、LOD、层级、linked_node 一并重映射，失效烘焙缓存并递增 revision。
void append_scene_asset(Scene& destination, Scene source);

// 追加角色匹配的纹理并返回编号。重建 mip，但保留材质已有颜色/金属度/粗糙度/
// 发光/法线强度/AO 强度等所有因子（默认法线/AO 强度来自 Material 的默认值 1）。
// role 决定原始通道的编码解释；异常不修改 scene。
int bind_material_texture(Scene& scene, std::size_t material, Texture texture, TextureRole role);

TestResults test_editor_assets();

} // namespace emberframe::lab
