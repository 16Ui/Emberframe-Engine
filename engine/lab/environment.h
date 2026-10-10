#pragma once
#include "shading.h"
#include <cstddef>
#include <span>
#include <string_view>

namespace emberframe::lab {
struct EnvironmentLimits {
    static constexpr std::size_t file_bytes=32*1024*1024;
    static constexpr std::size_t pixels=8*1024*1024;
    static constexpr int dimension=8192;
    static constexpr float radiance=65504.f;
};

// 工厂完成验证后才发布；只提供 const 访问，场景/撤销/异步任务共享同一张线性 HDR。
// 经纬图约定：顶行是 +Y，u=atan2(z,x)/(2*pi)，水平循环、垂直钳制。
struct EnvironmentMap final {
    const Image<glm::vec3>& image() const noexcept { return image_; }
    const std::string& name() const noexcept { return name_; }
    std::uint64_t fingerprint() const noexcept { return fingerprint_; }
    std::size_t retained_bytes() const noexcept { return image_.pixels.size()*sizeof(glm::vec3)+encoded_.size(); }
private:
    EnvironmentMap(Image<glm::vec3>,std::string);
    Image<glm::vec3> image_;
    std::string name_;
    // 外部 RGBE 保留原编码，保存时无需展开 4K HDR 成为近 200 MiB 的 hex float 记录。
    std::vector<std::byte> encoded_;
    std::uint64_t fingerprint_=0;
    friend std::shared_ptr<const EnvironmentMap> make_environment_map(Image<glm::vec3>,std::string);
    friend std::shared_ptr<const EnvironmentMap> load_environment_hdr(std::span<const std::byte>,std::string);
    friend std::string serialize_environment(const EnvironmentMap&);
};

std::shared_ptr<const EnvironmentMap> make_environment_map(Image<glm::vec3>,std::string name="Embedded environment");
std::shared_ptr<const EnvironmentMap> load_environment_hdr(const std::filesystem::path&);
std::shared_ptr<const EnvironmentMap> load_environment_hdr(std::span<const std::byte>,std::string name="Radiance HDR");
void validate_environment_settings(const Scene&);
// 世界方向转到贴图方向；正角度把环境绕世界 +Y 旋转，不移动相机或场景。
glm::vec3 environment_lookup_direction(glm::vec3 direction,float rotation_degrees);
glm::vec3 sample_environment_map(const EnvironmentMap&,glm::vec3 direction);
std::uint64_t environment_fingerprint(const Scene&);
// Scene 预过滤统一入口，CPU 与 GPU 使用同一采样/积分实现。
// HDR 使用 128x64 / 8 roughness levels，解析天空保留旧 32x16 配置；BRDF 表保持一致。
IblOptions scene_ibl_options(const Scene&) noexcept;
IblData precompute_scene_ibl(const Scene&);
IblData precompute_scene_ibl(const Scene&,IblOptions options);
std::shared_ptr<const IblData> cached_scene_ibl(const Scene&);
std::shared_ptr<const IblData> cached_scene_ibl(const Scene&,IblOptions options);
// 固定小端封装 + 校验和：外部图保存原 RGBE，程序化图保存 float32，不依赖导入路径。
std::string serialize_environment(const EnvironmentMap&);
std::shared_ptr<const EnvironmentMap> deserialize_environment(std::string_view);
} // namespace emberframe::lab
