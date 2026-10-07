#pragma once

#include "core/resource_registry.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace emberframe::renderer {

// 与 Fragment Shader 的 16 字节 Push Constant 对应；目前仅声明一种最小材质参数。
struct TriangleMaterial {
    float tint[4] { 1.0F, 1.0F, 1.0F, 1.0F };
};
static_assert(sizeof(TriangleMaterial) == 16);

class TrianglePipeline {
public:
    TrianglePipeline(VkDevice borrowed_device, VkFormat color_format,
        std::filesystem::path shader_directory);
    ~TrianglePipeline();

    TrianglePipeline(const TrianglePipeline&) = delete;
    TrianglePipeline& operator=(const TrianglePipeline&) = delete;

    [[nodiscard]] VkPipeline pipeline() const noexcept;
    [[nodiscard]] VkPipelineLayout layout() const noexcept;
    [[nodiscard]] VkFormat color_format() const noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept;
    [[nodiscard]] std::size_t pending_releases() const noexcept;

    // 构造新 Pipeline 成功后才替换旧的；失败保留旧版本。
    // retire_after_serial 表示旧 Pipeline 最后可能被哪个 GPU 提交使用。
    [[nodiscard]] bool reload(VkFormat new_format, std::uint64_t retire_after_serial);
    [[nodiscard]] bool reload_from(const std::filesystem::path& directory,
        VkFormat new_format, std::uint64_t retire_after_serial);
    void collect(std::uint64_t completed_serial);

private:
    struct PipelineResource {
        VkDevice device { VK_NULL_HANDLE };
        VkPipeline pipeline { VK_NULL_HANDLE };
        VkPipelineLayout layout { VK_NULL_HANDLE };

        PipelineResource() = default;
        PipelineResource(const PipelineResource&) = delete;
        PipelineResource& operator=(const PipelineResource&) = delete;
        PipelineResource(PipelineResource&& other) noexcept;
        PipelineResource& operator=(PipelineResource&& other) noexcept;
        ~PipelineResource();
    };

    [[nodiscard]] PipelineResource build(VkFormat format,
        const std::filesystem::path& directory) const;

    VkDevice device_ { VK_NULL_HANDLE };
    VkPipelineCache pipeline_cache_ { VK_NULL_HANDLE };
    VkFormat format_ { VK_FORMAT_UNDEFINED };
    std::filesystem::path shader_directory_;
    std::string last_error_;
    core::ResourceRegistry<PipelineResource> resources_;
    core::DeferredReleaseQueue<PipelineResource> deferred_;
    core::ResourceHandle active_;
};

} // namespace emberframe::renderer
