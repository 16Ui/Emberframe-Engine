#include "buffer_mesh_probe.h"

#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace emberframe::samples {
namespace {

void require_success(const VkResult result, const char* const operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            std::string(operation) + " failed, VkResult=" + std::to_string(result));
    }
}

[[nodiscard]] VkImageSubresourceRange full_color_range() noexcept
{
    VkImageSubresourceRange range {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    return range;
}

[[nodiscard]] VkDeviceSize align_up(
    const VkDeviceSize value,
    const VkDeviceSize alignment) noexcept
{
    return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] std::vector<std::uint32_t> read_spirv(
    const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Unable to open SPIR-V shader: " + path.string());
    }

    const std::streamsize byte_count = file.tellg();
    if (byte_count <= 0
        || byte_count % static_cast<std::streamsize>(sizeof(std::uint32_t)) != 0) {
        throw std::runtime_error("Invalid SPIR-V byte size: " + path.string());
    }

    std::vector<std::uint32_t> words(
        static_cast<std::size_t>(byte_count) / sizeof(std::uint32_t));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(words.data()), byte_count)) {
        throw std::runtime_error("Unable to read SPIR-V shader: " + path.string());
    }
    return words;
}

[[nodiscard]] VkShaderModule create_shader_module(
    const VkDevice device,
    const std::vector<std::uint32_t>& spirv)
{
    VkShaderModuleCreateInfo create_info {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    };
    create_info.codeSize = spirv.size() * sizeof(std::uint32_t);
    create_info.pCode = spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    require_success(
        vkCreateShaderModule(device, &create_info, nullptr, &module),
        "vkCreateShaderModule");
    return module;
}

[[nodiscard]] std::string memory_flags_string(const VkMemoryPropertyFlags flags)
{
    std::string result;
    const auto append = [&result](const char* const name) {
        if (!result.empty()) {
            result += '|';
        }
        result += name;
    };

    if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
        append("DEVICE_LOCAL");
    }
    if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        append("HOST_VISIBLE");
    }
    if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
        append("HOST_COHERENT");
    }
    if ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
        append("HOST_CACHED");
    }
    return result.empty() ? "none" : result;
}

} // namespace

BufferMeshProbe::BufferMeshProbe(
    const VkInstance borrowed_instance,
    const VkPhysicalDevice borrowed_physical_device,
    const VkDevice borrowed_device,
    const VkQueue borrowed_graphics_queue,
    const VkQueue borrowed_present_queue,
    const std::uint32_t graphics_queue_family,
    const std::size_t swapchain_image_count,
    const VkFormat swapchain_format,
    std::filesystem::path shader_directory)
    : device_(borrowed_device)
    , graphics_queue_(borrowed_graphics_queue)
    , present_queue_(borrowed_present_queue)
    , graphics_queue_family_(graphics_queue_family)
    , shader_directory_(std::move(shader_directory))
{
    if (borrowed_instance == VK_NULL_HANDLE
        || borrowed_physical_device == VK_NULL_HANDLE
        || device_ == VK_NULL_HANDLE
        || graphics_queue_ == VK_NULL_HANDLE
        || present_queue_ == VK_NULL_HANDLE
        || swapchain_image_count == 0
        || swapchain_format == VK_FORMAT_UNDEFINED) {
        throw std::invalid_argument("BufferMeshProbe requires valid Vulkan and Swapchain data");
    }

    try {
        create_allocator(borrowed_instance, borrowed_physical_device);
        create_mesh_buffers();
        create_frame_slots();
        render_finished_semaphores_ = create_semaphores(swapchain_image_count);
        create_graphics_pipeline(swapchain_format);
    } catch (...) {
        cleanup();
        throw;
    }

    std::cout << "[B10] Graphics Pipeline and indexed mesh buffers are ready.\n";
}

BufferMeshProbe::~BufferMeshProbe()
{
    cleanup();
}

void BufferMeshProbe::create_allocator(
    const VkInstance instance,
    const VkPhysicalDevice physical_device)
{
    VmaAllocatorCreateInfo create_info {};
    create_info.instance = instance;
    create_info.physicalDevice = physical_device;
    create_info.device = device_;
    create_info.vulkanApiVersion = VK_API_VERSION_1_3;
    require_success(
        vmaCreateAllocator(&create_info, &allocator_),
        "vmaCreateAllocator");
}

BufferMeshProbe::AllocatedBuffer BufferMeshProbe::create_buffer(
    const VkDeviceSize size,
    const VkBufferUsageFlags usage,
    const VmaAllocationCreateInfo& allocation_info,
    VmaAllocationInfo* const output_allocation_info) const
{
    VkBufferCreateInfo buffer_info {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    };
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    AllocatedBuffer result;
    result.size = size;
    require_success(
        vmaCreateBuffer(
            allocator_,
            &buffer_info,
            &allocation_info,
            &result.buffer,
            &result.allocation,
            output_allocation_info),
        "vmaCreateBuffer");
    return result;
}

void BufferMeshProbe::create_mesh_buffers()
{
    const std::array<Vertex, 4> vertices {{
        { { -0.68F, -0.58F }, { 1.0F, 0.18F, 0.12F } },
        { { 0.68F, -0.58F }, { 0.12F, 0.85F, 0.28F } },
        { { 0.68F, 0.58F }, { 0.15F, 0.35F, 1.0F } },
        { { -0.68F, 0.58F }, { 1.0F, 0.82F, 0.12F } },
    }};

    // 两个三角形共享顶点 0 和 2，因此只存 4 个顶点而不是重复存 6 个。
    const std::array<std::uint16_t, 6> indices { 0, 1, 2, 2, 3, 0 };

    VmaAllocationCreateInfo device_allocation {};
    device_allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    vertex_buffer_ = create_buffer(
        sizeof(vertices),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        device_allocation);
    index_buffer_ = create_buffer(
        sizeof(indices),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        device_allocation);

    upload_mesh_with_staging(vertices, indices);
    index_count_ = static_cast<std::uint32_t>(indices.size());
    print_memory_report();
}

void BufferMeshProbe::upload_mesh_with_staging(
    const std::array<Vertex, 4>& vertices,
    const std::array<std::uint16_t, 6>& indices)
{
    const VkDeviceSize vertex_bytes = sizeof(vertices);
    const VkDeviceSize index_bytes = sizeof(indices);
    // Copy Offset 按 4 字节对齐，避免不同数据块紧邻时破坏 Vulkan Copy 约束。
    const VkDeviceSize index_source_offset = align_up(vertex_bytes, 4);
    const VkDeviceSize staging_bytes = index_source_offset + index_bytes;

    VmaAllocationCreateInfo staging_allocation {};
    staging_allocation.usage = VMA_MEMORY_USAGE_AUTO;
    staging_allocation.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
        | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo staging_info {};
    AllocatedBuffer staging_buffer;
    VkCommandPool upload_pool = VK_NULL_HANDLE;
    VkFence upload_fence = VK_NULL_HANDLE;

    const auto release_temporary_resources = [&]() noexcept {
        if (upload_fence != VK_NULL_HANDLE) {
            vkDestroyFence(device_, upload_fence, nullptr);
            upload_fence = VK_NULL_HANDLE;
        }
        if (upload_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, upload_pool, nullptr);
            upload_pool = VK_NULL_HANDLE;
        }
        if (staging_buffer.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(
                allocator_,
                staging_buffer.buffer,
                staging_buffer.allocation);
            staging_buffer = {};
        }
    };

    try {
        staging_buffer = create_buffer(
            staging_bytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            staging_allocation,
            &staging_info);
        if (staging_info.pMappedData == nullptr) {
            throw std::runtime_error("VMA returned an unmapped staging allocation");
        }

        std::byte* const mapped_bytes =
            static_cast<std::byte*>(staging_info.pMappedData);
        std::memcpy(mapped_bytes, vertices.data(), static_cast<std::size_t>(vertex_bytes));
        std::memcpy(
            mapped_bytes + index_source_offset,
            indices.data(),
            static_cast<std::size_t>(index_bytes));
        require_success(
            vmaFlushAllocation(allocator_, staging_buffer.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation");

        VkMemoryPropertyFlags staging_memory_flags = 0;
        vmaGetAllocationMemoryProperties(
            allocator_,
            staging_buffer.allocation,
            &staging_memory_flags);
        std::cout << "[B10] Staging allocation: " << staging_bytes
                  << " bytes, " << memory_flags_string(staging_memory_flags) << '\n';

        VkCommandPoolCreateInfo pool_info {
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        };
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = graphics_queue_family_;
        require_success(
            vkCreateCommandPool(device_, &pool_info, nullptr, &upload_pool),
            "vkCreateCommandPool(upload)");

        VkCommandBuffer upload_command = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo allocate_info {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        };
        allocate_info.commandPool = upload_pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        require_success(
            vkAllocateCommandBuffers(device_, &allocate_info, &upload_command),
            "vkAllocateCommandBuffers(upload)");

        VkCommandBufferBeginInfo begin_info {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require_success(
            vkBeginCommandBuffer(upload_command, &begin_info),
            "vkBeginCommandBuffer(upload)");

        VkBufferCopy vertex_copy {};
        vertex_copy.srcOffset = 0;
        vertex_copy.dstOffset = 0;
        vertex_copy.size = vertex_bytes;
        vkCmdCopyBuffer(
            upload_command,
            staging_buffer.buffer,
            vertex_buffer_.buffer,
            1,
            &vertex_copy);

        VkBufferCopy index_copy {};
        index_copy.srcOffset = index_source_offset;
        index_copy.dstOffset = 0;
        index_copy.size = index_bytes;
        vkCmdCopyBuffer(
            upload_command,
            staging_buffer.buffer,
            index_buffer_.buffer,
            1,
            &index_copy);

        // Copy 写入完成后，后续 Draw 才能把同一批数据作为 Vertex/Index Input 读取。
        VkMemoryBarrier2 ready_for_vertex_input {
            VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        };
        ready_for_vertex_input.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        ready_for_vertex_input.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ready_for_vertex_input.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        ready_for_vertex_input.dstAccessMask =
            VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT
            | VK_ACCESS_2_INDEX_READ_BIT;

        VkDependencyInfo dependency_info {
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        };
        dependency_info.memoryBarrierCount = 1;
        dependency_info.pMemoryBarriers = &ready_for_vertex_input;
        vkCmdPipelineBarrier2(upload_command, &dependency_info);
        require_success(vkEndCommandBuffer(upload_command), "vkEndCommandBuffer(upload)");

        VkFenceCreateInfo fence_info {
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };
        require_success(
            vkCreateFence(device_, &fence_info, nullptr, &upload_fence),
            "vkCreateFence(upload)");

        VkCommandBufferSubmitInfo command_info {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        };
        command_info.commandBuffer = upload_command;

        VkSubmitInfo2 submit_info {
            VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        };
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &command_info;
        require_success(
            vkQueueSubmit2(graphics_queue_, 1, &submit_info, upload_fence),
            "vkQueueSubmit2(upload)");
        require_success(
            vkWaitForFences(
                device_,
                1,
                &upload_fence,
                VK_TRUE,
                std::numeric_limits<std::uint64_t>::max()),
            "vkWaitForFences(upload)");

        std::cout << "[B10] Uploaded " << vertex_bytes << " vertex bytes and "
                  << index_bytes << " index bytes; staging resources can now be destroyed.\n";
    } catch (...) {
        release_temporary_resources();
        throw;
    }

    release_temporary_resources();
}

void BufferMeshProbe::print_memory_report() const
{
    VkMemoryPropertyFlags vertex_flags = 0;
    VkMemoryPropertyFlags index_flags = 0;
    vmaGetAllocationMemoryProperties(allocator_, vertex_buffer_.allocation, &vertex_flags);
    vmaGetAllocationMemoryProperties(allocator_, index_buffer_.allocation, &index_flags);

    VmaTotalStatistics statistics {};
    vmaCalculateStatistics(allocator_, &statistics);

    std::cout << "[B10] Vertex Buffer: " << vertex_buffer_.size
              << " bytes, " << memory_flags_string(vertex_flags) << '\n'
              << "[B10] Index Buffer: " << index_buffer_.size
              << " bytes, " << memory_flags_string(index_flags) << '\n'
              << "[B10] VMA live allocations="
              << statistics.total.statistics.allocationCount
              << ", requested bytes="
              << statistics.total.statistics.allocationBytes
              << ", backing block bytes="
              << statistics.total.statistics.blockBytes << '\n';
}

void BufferMeshProbe::create_frame_slots()
{
    for (FrameSlot& slot : frame_slots_) {
        VkCommandPoolCreateInfo pool_info {
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        };
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = graphics_queue_family_;
        require_success(
            vkCreateCommandPool(device_, &pool_info, nullptr, &slot.command_pool),
            "vkCreateCommandPool");

        VkCommandBufferAllocateInfo allocate_info {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        };
        allocate_info.commandPool = slot.command_pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        require_success(
            vkAllocateCommandBuffers(device_, &allocate_info, &slot.command_buffer),
            "vkAllocateCommandBuffers");

        VkFenceCreateInfo fence_info {
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        require_success(
            vkCreateFence(device_, &fence_info, nullptr, &slot.in_flight_fence),
            "vkCreateFence");

        VkSemaphoreCreateInfo semaphore_info {
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        require_success(
            vkCreateSemaphore(device_, &semaphore_info, nullptr, &slot.image_available),
            "vkCreateSemaphore(image available)");
    }
}

std::vector<VkSemaphore> BufferMeshProbe::create_semaphores(
    const std::size_t count) const
{
    std::vector<VkSemaphore> semaphores;
    semaphores.reserve(count);

    VkSemaphoreCreateInfo create_info {
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    try {
        for (std::size_t index = 0; index < count; ++index) {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            require_success(
                vkCreateSemaphore(device_, &create_info, nullptr, &semaphore),
                "vkCreateSemaphore(render finished)");
            semaphores.push_back(semaphore);
        }
    } catch (...) {
        for (const VkSemaphore semaphore : semaphores) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        throw;
    }
    return semaphores;
}

void BufferMeshProbe::create_graphics_pipeline(const VkFormat color_format)
{
    static_assert(std::is_standard_layout_v<Vertex>);

    const std::vector<std::uint32_t> vertex_spirv =
        read_spirv(shader_directory_ / "b10_mesh.vert.spv");
    const std::vector<std::uint32_t> fragment_spirv =
        read_spirv(shader_directory_ / "b10_mesh.frag.spv");

    const VkShaderModule vertex_module = create_shader_module(device_, vertex_spirv);
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    try {
        fragment_module = create_shader_module(device_, fragment_spirv);

        VkPipelineShaderStageCreateInfo vertex_stage {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        };
        vertex_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertex_stage.module = vertex_module;
        vertex_stage.pName = "main";

        VkPipelineShaderStageCreateInfo fragment_stage {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        };
        fragment_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragment_stage.module = fragment_module;
        fragment_stage.pName = "main";
        const std::array shader_stages { vertex_stage, fragment_stage };

        VkVertexInputBindingDescription binding {};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        const std::array attributes {
            VkVertexInputAttributeDescription {
                0,
                0,
                VK_FORMAT_R32G32_SFLOAT,
                static_cast<std::uint32_t>(offsetof(Vertex, position)),
            },
            VkVertexInputAttributeDescription {
                1,
                0,
                VK_FORMAT_R32G32B32_SFLOAT,
                static_cast<std::uint32_t>(offsetof(Vertex, color)),
            },
        };

        VkPipelineVertexInputStateCreateInfo vertex_input {
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        };
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo input_assembly {
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        };
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport_state {
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        };
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization {
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        };
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterization.lineWidth = 1.0F;

        VkPipelineMultisampleStateCreateInfo multisampling {
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        };
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blend_attachment {};
        blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT
            | VK_COLOR_COMPONENT_G_BIT
            | VK_COLOR_COMPONENT_B_BIT
            | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo color_blending {
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        };
        color_blending.attachmentCount = 1;
        color_blending.pAttachments = &blend_attachment;

        const std::array dynamic_states {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        VkPipelineDynamicStateCreateInfo dynamic_state {
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        };
        dynamic_state.dynamicStateCount =
            static_cast<std::uint32_t>(dynamic_states.size());
        dynamic_state.pDynamicStates = dynamic_states.data();

        VkPipelineLayoutCreateInfo layout_info {
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        };
        require_success(
            vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_),
            "vkCreatePipelineLayout");

        VkPipelineRenderingCreateInfo rendering_info {
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        };
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachmentFormats = &color_format;

        VkGraphicsPipelineCreateInfo pipeline_info {
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        };
        pipeline_info.pNext = &rendering_info;
        pipeline_info.stageCount = static_cast<std::uint32_t>(shader_stages.size());
        pipeline_info.pStages = shader_stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisampling;
        pipeline_info.pColorBlendState = &color_blending;
        pipeline_info.pDynamicState = &dynamic_state;
        pipeline_info.layout = pipeline_layout_;
        pipeline_info.renderPass = VK_NULL_HANDLE;

        require_success(
            vkCreateGraphicsPipelines(
                device_,
                VK_NULL_HANDLE,
                1,
                &pipeline_info,
                nullptr,
                &graphics_pipeline_),
            "vkCreateGraphicsPipelines");
        pipeline_color_format_ = color_format;
    } catch (...) {
        if (fragment_module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, fragment_module, nullptr);
        }
        vkDestroyShaderModule(device_, vertex_module, nullptr);
        throw;
    }

    vkDestroyShaderModule(device_, fragment_module, nullptr);
    vkDestroyShaderModule(device_, vertex_module, nullptr);
}

void BufferMeshProbe::destroy_graphics_pipeline() noexcept
{
    if (graphics_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, graphics_pipeline_, nullptr);
        graphics_pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    pipeline_color_format_ = VK_FORMAT_UNDEFINED;
}

void BufferMeshProbe::record_draw_commands(
    const VkCommandBuffer command_buffer,
    const VkImage swapchain_image,
    const VkImageView swapchain_image_view,
    const VkExtent2D extent) const
{
    require_success(vkResetCommandBuffer(command_buffer, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo begin_info {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    require_success(vkBeginCommandBuffer(command_buffer, &begin_info), "vkBeginCommandBuffer");

    VkImageMemoryBarrier2 to_color_attachment {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
    };
    to_color_attachment.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    to_color_attachment.srcAccessMask = VK_ACCESS_2_NONE;
    to_color_attachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_color_attachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_color_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_color_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_color_attachment.image = swapchain_image;
    to_color_attachment.subresourceRange = full_color_range();

    VkDependencyInfo before_rendering {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
    };
    before_rendering.imageMemoryBarrierCount = 1;
    before_rendering.pImageMemoryBarriers = &to_color_attachment;
    vkCmdPipelineBarrier2(command_buffer, &before_rendering);

    VkRenderingAttachmentInfo color_attachment {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
    };
    color_attachment.imageView = swapchain_image_view;
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue.color = { { 0.025F, 0.035F, 0.065F, 1.0F } };

    VkRenderingInfo rendering_info {
        VK_STRUCTURE_TYPE_RENDERING_INFO,
    };
    rendering_info.renderArea.offset = { 0, 0 };
    rendering_info.renderArea.extent = extent;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachments = &color_attachment;

    vkCmdBeginRendering(command_buffer, &rendering_info);
    vkCmdBindPipeline(
        command_buffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        graphics_pipeline_);

    VkViewport viewport {};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);

    VkRect2D scissor {};
    scissor.extent = extent;
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);

    const VkDeviceSize vertex_offset = 0;
    vkCmdBindVertexBuffers(
        command_buffer,
        0,
        1,
        &vertex_buffer_.buffer,
        &vertex_offset);
    vkCmdBindIndexBuffer(
        command_buffer,
        index_buffer_.buffer,
        0,
        VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(command_buffer, index_count_, 1, 0, 0, 0);
    vkCmdEndRendering(command_buffer);

    VkImageMemoryBarrier2 to_present {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
    };
    to_present.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    to_present.dstAccessMask = VK_ACCESS_2_NONE;
    to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = swapchain_image;
    to_present.subresourceRange = full_color_range();

    VkDependencyInfo before_present {
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
    };
    before_present.imageMemoryBarrierCount = 1;
    before_present.pImageMemoryBarriers = &to_present;
    vkCmdPipelineBarrier2(command_buffer, &before_present);
    require_success(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer");
}

BufferMeshProbe::DrawResult BufferMeshProbe::draw_frame(
    const VkSwapchainKHR borrowed_swapchain,
    const std::vector<VkImage>& borrowed_swapchain_images,
    const std::vector<VkImageView>& borrowed_swapchain_image_views,
    const VkExtent2D extent)
{
    if (borrowed_swapchain == VK_NULL_HANDLE
        || borrowed_swapchain_images.empty()
        || borrowed_swapchain_images.size() != borrowed_swapchain_image_views.size()
        || borrowed_swapchain_images.size() != render_finished_semaphores_.size()
        || extent.width == 0
        || extent.height == 0) {
        throw std::invalid_argument("B10 draw_frame received mismatched Swapchain resources");
    }

    const std::size_t slot_index =
        static_cast<std::size_t>(frame_number_ % frame_slots_.size());
    FrameSlot& slot = frame_slots_[slot_index];

    require_success(
        vkWaitForFences(
            device_,
            1,
            &slot.in_flight_fence,
            VK_TRUE,
            std::numeric_limits<std::uint64_t>::max()),
        "vkWaitForFences");

    std::uint32_t image_index = 0;
    const VkResult acquire_result = vkAcquireNextImageKHR(
        device_,
        borrowed_swapchain,
        std::numeric_limits<std::uint64_t>::max(),
        slot.image_available,
        VK_NULL_HANDLE,
        &image_index);

    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
        return DrawResult::needs_swapchain_recreation;
    }
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error(
            "vkAcquireNextImageKHR failed, VkResult=" + std::to_string(acquire_result));
    }

    require_success(vkResetFences(device_, 1, &slot.in_flight_fence), "vkResetFences");
    record_draw_commands(
        slot.command_buffer,
        borrowed_swapchain_images.at(image_index),
        borrowed_swapchain_image_views.at(image_index),
        extent);

    VkSemaphoreSubmitInfo acquire_wait {
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
    };
    acquire_wait.semaphore = slot.image_available;
    acquire_wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkCommandBufferSubmitInfo command_info {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
    };
    command_info.commandBuffer = slot.command_buffer;

    const VkSemaphore render_finished = render_finished_semaphores_.at(image_index);
    VkSemaphoreSubmitInfo render_finished_signal {
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
    };
    render_finished_signal.semaphore = render_finished;
    render_finished_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit_info {
        VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
    };
    submit_info.waitSemaphoreInfoCount = 1;
    submit_info.pWaitSemaphoreInfos = &acquire_wait;
    submit_info.commandBufferInfoCount = 1;
    submit_info.pCommandBufferInfos = &command_info;
    submit_info.signalSemaphoreInfoCount = 1;
    submit_info.pSignalSemaphoreInfos = &render_finished_signal;
    require_success(
        vkQueueSubmit2(graphics_queue_, 1, &submit_info, slot.in_flight_fence),
        "vkQueueSubmit2");

    VkPresentInfoKHR present_info {
        VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
    };
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_finished;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &borrowed_swapchain;
    present_info.pImageIndices = &image_index;
    const VkResult present_result = vkQueuePresentKHR(present_queue_, &present_info);

    ++slot.submission_count;
    if (frame_number_ < 8 || frame_number_ % 120 == 0) {
        std::cout << "[B10] Frame " << frame_number_
                  << " | Slot " << slot_index
                  << " | Image " << image_index
                  << " | Bind Vertex/Index -> DrawIndexed(" << index_count_ << ") -> Present\n";
    }
    ++frame_number_;

    if (present_result == VK_ERROR_OUT_OF_DATE_KHR
        || present_result == VK_SUBOPTIMAL_KHR
        || acquire_result == VK_SUBOPTIMAL_KHR) {
        return DrawResult::needs_swapchain_recreation;
    }
    if (present_result != VK_SUCCESS) {
        throw std::runtime_error(
            "vkQueuePresentKHR failed, VkResult=" + std::to_string(present_result));
    }
    return DrawResult::presented;
}

void BufferMeshProbe::rebuild_swapchain_resources(
    const std::size_t swapchain_image_count,
    const VkFormat swapchain_format)
{
    if (swapchain_image_count == 0 || swapchain_format == VK_FORMAT_UNDEFINED) {
        throw std::invalid_argument("Swapchain rebuild requires valid image data");
    }

    require_success(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(before B10 rebuild)");
    std::vector<VkSemaphore> replacement_semaphores =
        create_semaphores(swapchain_image_count);
    for (const VkSemaphore semaphore : render_finished_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    render_finished_semaphores_ = std::move(replacement_semaphores);

    if (pipeline_color_format_ != swapchain_format) {
        destroy_graphics_pipeline();
        create_graphics_pipeline(swapchain_format);
    }
}

void BufferMeshProbe::cleanup() noexcept
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    vkDeviceWaitIdle(device_);
    destroy_graphics_pipeline();

    for (const VkSemaphore semaphore : render_finished_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    render_finished_semaphores_.clear();

    for (FrameSlot& slot : frame_slots_) {
        if (slot.image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, slot.image_available, nullptr);
            slot.image_available = VK_NULL_HANDLE;
        }
        if (slot.in_flight_fence != VK_NULL_HANDLE) {
            vkDestroyFence(device_, slot.in_flight_fence, nullptr);
            slot.in_flight_fence = VK_NULL_HANDLE;
        }
        if (slot.command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, slot.command_pool, nullptr);
            slot.command_pool = VK_NULL_HANDLE;
            slot.command_buffer = VK_NULL_HANDLE;
        }
    }

    if (allocator_ != VK_NULL_HANDLE) {
        if (index_buffer_.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator_, index_buffer_.buffer, index_buffer_.allocation);
            index_buffer_ = {};
        }
        if (vertex_buffer_.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator_, vertex_buffer_.buffer, vertex_buffer_.allocation);
            vertex_buffer_ = {};
        }
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }

    std::cout << "[B10] Buffers, VMA allocator, Pipeline and frame resources destroyed.\n";
}

} // namespace emberframe::samples

