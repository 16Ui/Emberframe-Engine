#include "triangle_pipeline_probe.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
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
    if (!file.read(
            reinterpret_cast<char*>(words.data()),
            byte_count)) {
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

} // namespace

TrianglePipelineProbe::TrianglePipelineProbe(
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
    if (device_ == VK_NULL_HANDLE
        || graphics_queue_ == VK_NULL_HANDLE
        || present_queue_ == VK_NULL_HANDLE
        || swapchain_image_count == 0
        || swapchain_format == VK_FORMAT_UNDEFINED) {
        throw std::invalid_argument(
            "TrianglePipelineProbe requires valid Device, Queue and Swapchain data");
    }

    try {
        create_frame_slots();
        render_finished_semaphores_ = create_semaphores(swapchain_image_count);
        create_graphics_pipeline(swapchain_format);
    } catch (...) {
        cleanup();
        throw;
    }

    std::cout << "[B9] Graphics Pipeline ready for Swapchain format "
              << static_cast<int>(pipeline_color_format_) << ".\n";
}

TrianglePipelineProbe::~TrianglePipelineProbe()
{
    cleanup();
}

void TrianglePipelineProbe::create_frame_slots()
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

std::vector<VkSemaphore> TrianglePipelineProbe::create_semaphores(
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

void TrianglePipelineProbe::create_graphics_pipeline(const VkFormat color_format)
{
    const std::vector<std::uint32_t> vertex_spirv =
        read_spirv(shader_directory_ / "b9_triangle.vert.spv");
    const std::vector<std::uint32_t> fragment_spirv =
        read_spirv(shader_directory_ / "b9_triangle.frag.spv");

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

        // 顶点来自 gl_VertexIndex，因此 B9 的 Vertex Input 暂时为空。
        VkPipelineVertexInputStateCreateInfo vertex_input {
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        };

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
        blend_attachment.blendEnable = VK_FALSE;

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

        // B9 没有 Descriptor 和 Push Constant，所以 Pipeline Layout 当前为空。
        VkPipelineLayoutCreateInfo layout_info {
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        };
        require_success(
            vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_),
            "vkCreatePipelineLayout");

        // Dynamic Rendering 不创建 VkRenderPass；Pipeline 仍需知道输出 Attachment 的格式。
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

    // Pipeline 创建后已经拥有所需的可执行状态，Shader Module 可以立即销毁。
    vkDestroyShaderModule(device_, fragment_module, nullptr);
    vkDestroyShaderModule(device_, vertex_module, nullptr);
}

void TrianglePipelineProbe::destroy_graphics_pipeline() noexcept
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

void TrianglePipelineProbe::record_triangle_commands(
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
    require_success(
        vkBeginCommandBuffer(command_buffer, &begin_info),
        "vkBeginCommandBuffer");

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
    // ImageView 决定本次 Rendering 通过什么格式和子资源范围访问 Image。
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

    // 从这里开始，后续 Draw 的 Fragment Shader 输出会写入上面的 Color Attachment。
    vkCmdBeginRendering(command_buffer, &rendering_info);
    vkCmdBindPipeline(
        command_buffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        graphics_pipeline_);

    VkViewport viewport {};
    viewport.x = 0.0F;
    viewport.y = 0.0F;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);

    VkRect2D scissor {};
    scissor.offset = { 0, 0 };
    scissor.extent = extent;
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);

    // 3 个顶点、1 个实例。顶点着色器通过 gl_VertexIndex 生成顶点与颜色。
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
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

TrianglePipelineProbe::DrawResult TrianglePipelineProbe::draw_frame(
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
        throw std::invalid_argument("B9 draw_frame received mismatched Swapchain resources");
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

    record_triangle_commands(
        slot.command_buffer,
        borrowed_swapchain_images.at(image_index),
        borrowed_swapchain_image_views.at(image_index),
        extent);

    VkSemaphoreSubmitInfo acquire_wait {
        VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
    };
    acquire_wait.semaphore = slot.image_available;
    // 第一处真正使用 Swapchain Image 的阶段已经从 B8 的 Transfer 改成颜色输出阶段。
    acquire_wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkCommandBufferSubmitInfo command_buffer_info {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
    };
    command_buffer_info.commandBuffer = slot.command_buffer;

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
    submit_info.pCommandBufferInfos = &command_buffer_info;
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
        std::cout << "[B9] Frame " << frame_number_
                  << " | Slot " << slot_index
                  << " | Image " << image_index
                  << " | BeginRendering -> BindPipeline -> Draw(3) -> Present"
                  << " | slot submissions=" << slot.submission_count << '\n';
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

void TrianglePipelineProbe::rebuild_swapchain_resources(
    const std::size_t swapchain_image_count,
    const VkFormat swapchain_format)
{
    if (swapchain_image_count == 0 || swapchain_format == VK_FORMAT_UNDEFINED) {
        throw std::invalid_argument("Swapchain rebuild requires valid image data");
    }

    require_success(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(before B9 rebuild)");
    std::vector<VkSemaphore> replacement_semaphores =
        create_semaphores(swapchain_image_count);

    for (const VkSemaphore semaphore : render_finished_semaphores_) {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    render_finished_semaphores_ = std::move(replacement_semaphores);

    // Dynamic Viewport/Scissor 允许尺寸变化不重建 Pipeline；只有输出 Format 变化才需要重建。
    if (pipeline_color_format_ != swapchain_format) {
        destroy_graphics_pipeline();
        create_graphics_pipeline(swapchain_format);
    }
}

void TrianglePipelineProbe::cleanup() noexcept
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
    std::cout << "[B9] Pipeline, synchronization and command resources destroyed.\n";
}

} // namespace emberframe::samples

