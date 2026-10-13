// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: SMAA 1x and AMD CAS on the presented image (postfx_pass.h).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/cas_frag.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/smaa_blend_frag.h"
#include "video_core/host_shaders/smaa_blend_vert.h"
#include "video_core/host_shaders/smaa_edges_frag.h"
#include "video_core/host_shaders/smaa_edges_vert.h"
#include "video_core/host_shaders/smaa_weights_frag.h"
#include "video_core/host_shaders/smaa_weights_vert.h"
#include "video_core/renderer_vulkan/host_passes/postfx_pass.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include <vk_mem_alloc.h>

// SMAA's precomputed lookup textures (MIT, github.com/iryoku/smaa Textures/).
#include "video_core/host_shaders/smaa/AreaTex.h"
#include "video_core/host_shaders/smaa/SearchTex.h"

#define A_CPU
#include "video_core/host_shaders/fsr/ffx_a.h"
#include "video_core/host_shaders/cas/ffx_cas.h"

namespace Vulkan::HostPasses {

namespace {

constexpr vk::ImageSubresourceRange ColorRange = {
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};

constexpr vk::Format ColorFormat = vk::Format::eR16G16B16A16Sfloat;

struct SmaaConstants {
    float rt_metrics[4];
    u32 input_linear;
};

vk::ImageMemoryBarrier2 Barrier(vk::Image image, vk::PipelineStageFlags2 src_stage,
                                vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                                vk::AccessFlags2 dst_access, vk::ImageLayout old_layout,
                                vk::ImageLayout new_layout) {
    return {
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = image,
        .subresourceRange = ColorRange,
    };
}

void PipelineBarrier(vk::CommandBuffer cmdbuf, std::initializer_list<vk::ImageMemoryBarrier2> barriers) {
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = u32(barriers.size()),
        .pImageMemoryBarriers = barriers.begin(),
    });
}

/// The target is about to be written again: previous frames read it in any stage.
vk::ImageMemoryBarrier2 ToAttachment(vk::Image image) {
    return Barrier(image, vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eNone,
                   vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                   vk::AccessFlagBits2::eColorAttachmentWrite, vk::ImageLayout::eUndefined,
                   vk::ImageLayout::eColorAttachmentOptimal);
}

vk::ImageMemoryBarrier2 AttachmentToRead(vk::Image image, vk::PipelineStageFlags2 dst_stage) {
    return Barrier(image, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                   vk::AccessFlagBits2::eColorAttachmentWrite, dst_stage,
                   vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eColorAttachmentOptimal,
                   vk::ImageLayout::eShaderReadOnlyOptimal);
}

vk::UniqueSampler MakeSampler(vk::Device device, vk::Filter filter) {
    return Check<"create postfx sampler">(device.createSamplerUnique(vk::SamplerCreateInfo{
        .magFilter = filter,
        .minFilter = filter,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxAnisotropy = 1.0f,
        .minLod = 0.0f,
        .maxLod = 0.0f,
    }));
}

vk::UniquePipeline MakeGraphicsPipeline(vk::Device device, vk::PipelineLayout layout,
                                        std::span<const u32> vs_code, std::span<const u32> fs_code,
                                        vk::Format format, const char* name) {
    const auto vs_module = CompileSPV(vs_code, device);
    const auto fs_module = CompileSPV(fs_code, device);
    ASSERT(vs_module && fs_module);
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = vs_module, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = fs_module, .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering_ci{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport_info{
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState attachment{
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo blend{
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };
    auto pipeline = Check<"create smaa pipeline">(device.createGraphicsPipelineUnique(
        {}, vk::GraphicsPipelineCreateInfo{
                .pNext = &rendering_ci,
                .stageCount = stages.size(),
                .pStages = stages.data(),
                .pVertexInputState = &vertex_input,
                .pInputAssemblyState = &input_assembly,
                .pViewportState = &viewport_info,
                .pRasterizationState = &raster,
                .pMultisampleState = &multisample,
                .pColorBlendState = &blend,
                .pDynamicState = &dynamic,
                .layout = layout,
            }));
    SetObjectName(device, *pipeline, name);
    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);
    return pipeline;
}

/// Frees images a frame in flight may still use once the GPU is past it.
template <typename... T>
void DeferDestroy(Scheduler& scheduler, T&&... objects) {
    scheduler.DeferOperation([... kept = std::move(objects)]() mutable {});
}

} // namespace

// ---------------------------------------------------------------------------------------------
// SMAA

void SmaaPass::Create(vk::Device device_, VmaAllocator allocator_) {
    device = device_;
    allocator = allocator_;
    linear_sampler = MakeSampler(device, vk::Filter::eLinear);
    point_sampler = MakeSampler(device, vk::Filter::eNearest);

    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {
            .binding = i,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        };
    }
    desc_set_layout = Check<"create smaa descriptor set layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = bindings.size(),
            .pBindings = bindings.data(),
        }));
    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(SmaaConstants),
    };
    pipeline_layout = Check<"create smaa pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));
    pipelines[0] = MakeGraphicsPipeline(device, *pipeline_layout, SMAA_EDGES_VERT, SMAA_EDGES_FRAG,
                                        vk::Format::eR8G8Unorm, "smaa edges");
    pipelines[1] = MakeGraphicsPipeline(device, *pipeline_layout, SMAA_WEIGHTS_VERT,
                                        SMAA_WEIGHTS_FRAG, vk::Format::eR8G8B8A8Unorm, "smaa weights");
    pipelines[2] = MakeGraphicsPipeline(device, *pipeline_layout, SMAA_BLEND_VERT, SMAA_BLEND_FRAG,
                                        ColorFormat, "smaa blend");
}

void SmaaPass::CreateTarget(Target& target, vk::Format format, vk::Extent2D size,
                            const char* name) {
    target.image = VideoCore::UniqueImage(device, allocator);
    const bool lookup = &target == &area || &target == &search;
    target.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled |
                 (lookup ? vk::ImageUsageFlagBits::eTransferDst
                         : vk::ImageUsageFlagBits::eColorAttachment |
                                   vk::ImageUsageFlagBits::eTransferSrc), // frame dump
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    SetObjectName(device, static_cast<vk::Image>(target.image), name);
    target.view = Check<"create smaa image view">(device.createImageViewUnique({
        .image = target.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = ColorRange,
    }));
}

void SmaaPass::UploadLookupTextures(vk::CommandBuffer cmdbuf, Scheduler& scheduler) {
    CreateTarget(area, vk::Format::eR8G8Unorm, {AREATEX_WIDTH, AREATEX_HEIGHT}, "SMAA area");
    CreateTarget(search, vk::Format::eR8Unorm, {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT}, "SMAA search");
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = AREATEX_SIZE + SEARCHTEX_SIZE,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    ASSERT(vmaCreateBuffer(allocator, &buffer_ci, &alloc_ci, &buffer, &allocation, &info) ==
           VK_SUCCESS);
    std::memcpy(info.pMappedData, areaTexBytes, AREATEX_SIZE);
    std::memcpy(static_cast<u8*>(info.pMappedData) + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
    vmaFlushAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);

    PipelineBarrier(cmdbuf, {
        Barrier(area.image, vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal),
        Barrier(search.image, vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal),
    });
    cmdbuf.copyBufferToImage(buffer, area.image, vk::ImageLayout::eTransferDstOptimal,
                             vk::BufferImageCopy{
                                 .bufferOffset = 0,
                                 .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                 .imageExtent = {AREATEX_WIDTH, AREATEX_HEIGHT, 1},
                             });
    cmdbuf.copyBufferToImage(buffer, search.image, vk::ImageLayout::eTransferDstOptimal,
                             vk::BufferImageCopy{
                                 .bufferOffset = AREATEX_SIZE,
                                 .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                 .imageExtent = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1},
                             });
    PipelineBarrier(cmdbuf, {
        Barrier(area.image, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal),
        Barrier(search.image, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal),
    });
    scheduler.DeferOperation([allocator = allocator, buffer, allocation] {
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
    lookup_ready = true;
}

void SmaaPass::Draw(vk::CommandBuffer cmdbuf, u32 pass, Target& target, vk::Extent2D size,
                    std::initializer_list<vk::DescriptorImageInfo> inputs, bool input_linear) {
    const vk::RenderingAttachmentInfo attachment{
        .imageView = *target.view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear, // edge detection discards edgeless pixels
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    std::array<vk::WriteDescriptorSet, 3> writes{};
    u32 count = 0;
    for (const auto& input : inputs) {
        writes[count] = {
            .dstBinding = count,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &input,
        };
        ++count;
    }
    const SmaaConstants constants{
        .rt_metrics = {1.0f / float(size.width), 1.0f / float(size.height), float(size.width),
                       float(size.height)},
        .input_linear = input_linear ? 1u : 0u,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipelines[pass]);
    cmdbuf.setViewport(0, vk::Viewport{.width = float(size.width),
                                       .height = float(size.height),
                                       .minDepth = 0.0f,
                                       .maxDepth = 1.0f});
    cmdbuf.setScissor(0, vk::Rect2D{.extent = size});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0,
                                std::span{writes.data(), count});
    cmdbuf.pushConstants(*pipeline_layout,
                         vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
                         sizeof(constants), &constants);
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {.extent = size},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();
}

vk::ImageView SmaaPass::Render(vk::CommandBuffer cmdbuf, Scheduler& scheduler, vk::ImageView input,
                               vk::Extent2D size, bool input_linear) {
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = "Host/SMAA"});
    }
    if (!lookup_ready) {
        UploadLookupTextures(cmdbuf, scheduler);
    }
    if (size != cur_size) {
        if (edges.image) {
            DeferDestroy(scheduler, std::move(edges), std::move(weights), std::move(output));
        }
        CreateTarget(edges, vk::Format::eR8G8Unorm, size, "SMAA edges");
        CreateTarget(weights, vk::Format::eR8G8B8A8Unorm, size, "SMAA weights");
        CreateTarget(output, ColorFormat, size, "SMAA output");
        cur_size = size;
    }
    const auto sampled = [](vk::Sampler sampler, vk::ImageView view) {
        return vk::DescriptorImageInfo{.sampler = sampler,
                                       .imageView = view,
                                       .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    };
    PipelineBarrier(cmdbuf, {ToAttachment(edges.image)});
    Draw(cmdbuf, 0, edges, size, {sampled(*linear_sampler, input)}, input_linear);
    PipelineBarrier(cmdbuf, {AttachmentToRead(edges.image, vk::PipelineStageFlagBits2::eFragmentShader),
                             ToAttachment(weights.image)});
    Draw(cmdbuf, 1, weights, size,
         {sampled(*linear_sampler, *edges.view), sampled(*linear_sampler, *area.view),
          sampled(*point_sampler, *search.view)},
         input_linear);
    PipelineBarrier(cmdbuf, {AttachmentToRead(weights.image, vk::PipelineStageFlagBits2::eFragmentShader),
                             ToAttachment(output.image)});
    Draw(cmdbuf, 2, output, size,
         {sampled(*linear_sampler, input), sampled(*linear_sampler, *weights.view)}, input_linear);
    PipelineBarrier(cmdbuf, {AttachmentToRead(output.image, vk::PipelineStageFlagBits2::eAllCommands)});
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
    return *output.view;
}

// ---------------------------------------------------------------------------------------------
// CAS

void CasPass::Create(vk::Device device_, VmaAllocator allocator_) {
    device = device_;
    allocator = allocator_;
    sampler = MakeSampler(device, vk::Filter::eNearest);
    const vk::DescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    desc_set_layout = Check<"create cas descriptor set layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = 1,
            .pBindings = &binding,
        }));
    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = 2 * 4 * sizeof(u32),
    };
    pipeline_layout = Check<"create cas pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));
    pipeline = MakeGraphicsPipeline(device, *pipeline_layout, FS_TRI_VERT, CAS_FRAG, ColorFormat,
                                    "cas");
}

vk::ImageView CasPass::Render(vk::CommandBuffer cmdbuf, Scheduler& scheduler, vk::ImageView input,
                              vk::Extent2D size, float sharpness) {
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = "Host/CAS"});
    }
    if (size != cur_size) {
        if (output) {
            DeferDestroy(scheduler, std::move(output), std::move(output_view));
        }
        output = VideoCore::UniqueImage(device, allocator);
        output.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = ColorFormat,
            .extent = {size.width, size.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eTransferSrc, // frame dump
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        SetObjectName(device, static_cast<vk::Image>(output), "CAS output");
        output_view = Check<"create cas image view">(device.createImageViewUnique({
            .image = output,
            .viewType = vk::ImageViewType::e2D,
            .format = ColorFormat,
            .subresourceRange = ColorRange,
        }));
        cur_size = size;
    }
    varAU4(const0);
    varAU4(const1);
    CasSetup(const0, const1, sharpness, AF1(size.width), AF1(size.height), AF1(size.width),
             AF1(size.height));
    u32 constants[8];
    std::memcpy(constants, const0, sizeof(const0));
    std::memcpy(constants + 4, const1, sizeof(const1));

    PipelineBarrier(cmdbuf, {ToAttachment(output)});
    const vk::DescriptorImageInfo input_info{.sampler = *sampler,
                                             .imageView = input,
                                             .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::WriteDescriptorSet write{.dstBinding = 0,
                                       .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                       .pImageInfo = &input_info};
    const vk::RenderingAttachmentInfo attachment{
        .imageView = *output_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    cmdbuf.setViewport(0, vk::Viewport{.width = float(size.width),
                                       .height = float(size.height),
                                       .minDepth = 0.0f,
                                       .maxDepth = 1.0f});
    cmdbuf.setScissor(0, vk::Rect2D{.extent = size});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, write);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
                         sizeof(constants), constants);
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {.extent = size},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();
    PipelineBarrier(cmdbuf, {AttachmentToRead(output, vk::PipelineStageFlagBits2::eAllCommands)});
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
    return *output_view;
}

// ---------------------------------------------------------------------------------------------
// Timing

namespace {
double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void PostFxTimer::Create(vk::Device device_, float timestamp_period_ns) {
    const char* env = std::getenv("TLG_POSTFX_PROFILE");
    if (!env || env[0] != '1') {
        return;
    }
    device = device_;
    period_ns = timestamp_period_ns;
    pool = Check<"create postfx query pool">(device.createQueryPoolUnique({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = Ring * Stamps,
    }));
    device.resetQueryPool(*pool, 0, Ring * Stamps);
    window_start = NowSeconds();
    std::printf("PostFX profile: on\n");
}

void PostFxTimer::Begin(vk::CommandBuffer cmdbuf) {
    cmdbuf.resetQueryPool(*pool, slot * Stamps, Stamps);
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool, slot * Stamps);
}

void PostFxTimer::Stamp(vk::CommandBuffer cmdbuf, Stage stage) {
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool,
                           slot * Stamps + 1 + stage);
}

void PostFxTimer::End(bool smaa, bool cas, bool color) {
    written[slot] = true;
    slot = (slot + 1) % Ring;
    // The slot written Ring - 1 frames ago, when the GPU is done with it (never waits).
    const u32 old = (slot + 1) % Ring;
    std::array<u64, Stamps> stamps{};
    if (written[old] &&
        device.getQueryPoolResults(*pool, old * Stamps, Stamps, sizeof(stamps), stamps.data(),
                                   sizeof(u64), vk::QueryResultFlagBits::e64) ==
            vk::Result::eSuccess) {
        for (u32 i = 0; i < StageCount; ++i) {
            total_ms[i] += double(stamps[i + 1] - stamps[i]) * period_ns / 1e6;
        }
        ++frames;
    }
    const double now = NowSeconds();
    if (now - window_start < 5.0 || frames == 0) {
        return;
    }
    std::printf("PostFX profile: %u frames; GPU ms/frame: SMAA %.3f%s, FSR 1/RCAS %.3f, CAS %.3f%s, "
                "post process %.3f%s\n",
                frames, total_ms[Smaa] / frames, smaa ? "" : " (off)", total_ms[FsrRcas] / frames,
                total_ms[Cas] / frames, cas ? "" : " (off)", total_ms[PostProcess] / frames,
                color ? " (with color)" : "");
    total_ms = {};
    frames = 0;
    window_start = now;
}

} // namespace Vulkan::HostPasses
