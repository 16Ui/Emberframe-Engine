#include "renderer/triangle_pipeline.h"

#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace emberframe::renderer {
namespace {

void require_success(const VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed, VkResult=" + std::to_string(result));
    }
}

[[nodiscard]] std::vector<std::uint32_t> read_spirv(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Cannot open shader: " + path.string());
    }
    const std::streamsize bytes = file.tellg();
    if (bytes <= 0 || bytes % sizeof(std::uint32_t) != 0) {
        throw std::runtime_error("Invalid SPIR-V byte count: " + path.string());
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / sizeof(std::uint32_t));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(words.data()), bytes)) {
        throw std::runtime_error("Cannot read shader: " + path.string());
    }
    return words;
}

struct ShaderModule {
    VkDevice device { VK_NULL_HANDLE };
    VkShaderModule handle { VK_NULL_HANDLE };
    ShaderModule(VkDevice owner, VkShaderModule module) noexcept : device(owner), handle(module) {}
    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;
    ShaderModule(ShaderModule&& other) noexcept
        : device(std::exchange(other.device, VK_NULL_HANDLE))
        , handle(std::exchange(other.handle, VK_NULL_HANDLE)) {}
    ~ShaderModule() { if (handle != VK_NULL_HANDLE) vkDestroyShaderModule(device, handle, nullptr); }
};

[[nodiscard]] ShaderModule create_shader_module(VkDevice device, const std::vector<std::uint32_t>& code)
{
    VkShaderModuleCreateInfo info { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = code.size() * sizeof(std::uint32_t);
    info.pCode = code.data();
    ShaderModule module { device, VK_NULL_HANDLE };
    require_success(vkCreateShaderModule(device, &info, nullptr, &module.handle), "vkCreateShaderModule");
    return module;
}

} // namespace

TrianglePipeline::PipelineResource::PipelineResource(PipelineResource&& other) noexcept
    : device(std::exchange(other.device, VK_NULL_HANDLE))
    , pipeline(std::exchange(other.pipeline, VK_NULL_HANDLE))
    , layout(std::exchange(other.layout, VK_NULL_HANDLE))
{
}

TrianglePipeline::PipelineResource& TrianglePipeline::PipelineResource::operator=(PipelineResource&& other) noexcept
{
    if (this != &other) {
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, layout, nullptr);
        device = std::exchange(other.device, VK_NULL_HANDLE);
        pipeline = std::exchange(other.pipeline, VK_NULL_HANDLE);
        layout = std::exchange(other.layout, VK_NULL_HANDLE);
    }
    return *this;
}

TrianglePipeline::PipelineResource::~PipelineResource()
{
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
    if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, layout, nullptr);
}

TrianglePipeline::TrianglePipeline(const VkDevice borrowed_device, const VkFormat color_format,
    std::filesystem::path shader_directory)
    : device_(borrowed_device), shader_directory_(std::move(shader_directory))
{
    if (device_ == VK_NULL_HANDLE || color_format == VK_FORMAT_UNDEFINED) {
        throw std::invalid_argument("TrianglePipeline requires Device and color format");
    }
    VkPipelineCacheCreateInfo cache_info { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    require_success(vkCreatePipelineCache(device_, &cache_info, nullptr, &pipeline_cache_),
        "vkCreatePipelineCache");
    try {
        active_ = resources_.create(build(color_format, shader_directory_));
        format_ = color_format;
    } catch (...) {
        vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        pipeline_cache_ = VK_NULL_HANDLE;
        throw;
    }
}

TrianglePipeline::~TrianglePipeline()
{
    // 活跃和延迟销毁的 Pipeline 都不再被 GPU 使用后，成员析构才能真正释放它们。
    vkDeviceWaitIdle(device_);
    deferred_.collect(std::numeric_limits<std::uint64_t>::max());
    vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
}

TrianglePipeline::PipelineResource TrianglePipeline::build(
    const VkFormat format, const std::filesystem::path& directory) const
{
    const ShaderModule vertex = create_shader_module(device_,
        read_spirv(directory / "triangle.vert.spv"));
    const ShaderModule fragment = create_shader_module(device_,
        read_spirv(directory / "triangle.frag.spv"));

    PipelineResource output;
    output.device = device_;

    VkPushConstantRange material_range {};
    material_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    material_range.offset = 0;
    material_range.size = sizeof(TriangleMaterial);
    VkPipelineLayoutCreateInfo layout_info { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &material_range;
    require_success(vkCreatePipelineLayout(device_, &layout_info, nullptr, &output.layout),
        "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo vertex_stage { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    vertex_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertex_stage.module = vertex.handle;
    vertex_stage.pName = "main";
    VkPipelineShaderStageCreateInfo fragment_stage { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    fragment_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragment_stage.module = fragment.handle;
    fragment_stage.pName = "main";
    const std::array stages { vertex_stage, fragment_stage };

    VkPipelineVertexInputStateCreateInfo vertex_input { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment {};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blending.attachmentCount = 1;
    blending.pAttachments = &attachment;
    const std::array dynamic_states { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkPipelineRenderingCreateInfo rendering { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;

    VkGraphicsPipelineCreateInfo pipeline_info { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipeline_info.pNext = &rendering;
    pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blending;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = output.layout;
    pipeline_info.renderPass = VK_NULL_HANDLE;
    require_success(vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &pipeline_info,
        nullptr, &output.pipeline), "vkCreateGraphicsPipelines");
    return output;
}

VkPipeline TrianglePipeline::pipeline() const noexcept
{
    const auto* resource = resources_.get(active_);
    return resource ? resource->pipeline : VK_NULL_HANDLE;
}

VkPipelineLayout TrianglePipeline::layout() const noexcept
{
    const auto* resource = resources_.get(active_);
    return resource ? resource->layout : VK_NULL_HANDLE;
}

VkFormat TrianglePipeline::color_format() const noexcept { return format_; }
const std::string& TrianglePipeline::last_error() const noexcept { return last_error_; }
std::size_t TrianglePipeline::pending_releases() const noexcept { return deferred_.pending_count(); }

bool TrianglePipeline::reload(const VkFormat new_format, const std::uint64_t retire_after_serial)
{
    return reload_from(shader_directory_, new_format, retire_after_serial);
}

bool TrianglePipeline::reload_from(const std::filesystem::path& directory,
    const VkFormat new_format, const std::uint64_t retire_after_serial)
{
    try {
        PipelineResource replacement = build(new_format, directory);
        const core::ResourceHandle next = resources_.create(std::move(replacement));
        auto previous = resources_.release(active_);
        if (previous) {
            try {
                deferred_.retire(retire_after_serial, std::move(*previous));
            } catch (...) {
                // 内存分配失败时退回保守同步，保证旧 Pipeline 析构前 GPU 已不用它。
                vkDeviceWaitIdle(device_);
            }
        }
        active_ = next;
        format_ = new_format;
        last_error_.clear();
        return true;
    } catch (const std::exception& error) {
        last_error_ = error.what();
        return false;
    } catch (...) {
        last_error_ = "Unknown pipeline reload failure";
        return false;
    }
}

void TrianglePipeline::collect(const std::uint64_t completed_serial)
{
    deferred_.collect(completed_serial);
}

} // namespace emberframe::renderer
