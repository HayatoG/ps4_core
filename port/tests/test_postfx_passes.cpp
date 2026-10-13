// SPDX-License-Identifier: GPL-2.0-or-later
// The presenter's SMAA and CAS passes (postfx_pass.cpp) on a synthetic image: flat areas stay
// as they are, a stair-stepped edge is blended (SMAA) and edges gain contrast (CAS).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "video_core/renderer_vulkan/host_passes/postfx_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>

namespace {
float Half(u16 v) {
    const int exponent = (v >> 10) & 31;
    const float mantissa = float(v & 1023);
    return (v & 0x8000 ? -1.f : 1.f) *
           (exponent == 0 ? std::ldexp(mantissa, -24) : std::ldexp(1.f + mantissa / 1024.f, exponent - 15));
}
} // namespace

int main() {
    using namespace Vulkan;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Instance instance(0, false);
    // This executable's own vulkan.hpp dispatcher (hidden visibility, as in taa-shader-test).
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance());
    d.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    Scheduler scheduler(instance, true); // threaded recording, as the presenter's draw scheduler
    constexpr u32 W = 1920, H = 1080;
    // A dark/bright edge that steps one pixel right every 4 rows (aliased, about 76 degrees).
    const auto bright = [](u32 x, u32 y) { return x >= 24 + y / 4; };
    VideoCore::UniqueImage input(device, instance.GetAllocator());
    input.Create({.imageType = vk::ImageType::e2D, .format = vk::Format::eR8G8B8A8Unorm,
                  .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1,
                  .samples = vk::SampleCountFlagBits::e1,
                  .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst});
    auto input_view = device.createImageViewUnique({.image = vk::Image(input),
        .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}, nullptr, d).value;
    VkBuffer staging{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 8,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &staging, &allocation, &info) == VK_SUCCESS);
    auto* bytes = static_cast<u8*>(info.pMappedData);
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            u8* p = bytes + (y * W + x) * 4;
            p[0] = p[1] = p[2] = y >= 56 && y < 64 ? (x & 1 ? 255 : 0) : bright(x, y) ? 204 : 51;
            p[3] = 255;
        }
    }
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    {
        const auto cmd = scheduler.CommandBuffer();
        const vk::ImageMemoryBarrier2 to_copy{.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite, .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal, .image = vk::Image(input), .subresourceRange = range};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy}, d);
        cmd.copyBufferToImage(staging, vk::Image(input), vk::ImageLayout::eTransferDstOptimal,
            vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                .imageExtent = {W, H, 1}}, d);
        const vk::ImageMemoryBarrier2 to_read{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead, .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal, .image = vk::Image(input), .subresourceRange = range};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read}, d);
        scheduler.Finish();
    }
    // Runs a pass and reads its RGBA16F output back: the red channel per pixel.
    const auto read_back = [&](vk::Image image) {
        const auto cmd = scheduler.CommandBuffer();
        const vk::ImageMemoryBarrier2 to_copy{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite, .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead, .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal, .image = image, .subresourceRange = range};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy}, d);
        cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, staging,
            vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                .imageExtent = {W, H, 1}}, d);
        const vk::ImageMemoryBarrier2 back{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands, .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal, .image = image, .subresourceRange = range};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &back}, d);
        scheduler.Finish();
        std::vector<float> red(W * H);
        const auto* halfs = static_cast<const u16*>(info.pMappedData);
        for (u32 i = 0; i < W * H; ++i) red[i] = Half(halfs[i * 4]);
        return red;
    };
    const float dark = 51.f / 255.f, light = 204.f / 255.f;

    // The passes are never destroyed: their vk::Unique* members would be freed through this
    // executable's default dispatcher, which is not initialized.
    auto& cas = *new HostPasses::CasPass;
    cas.Create(device, instance.GetAllocator());
    cas.Render(scheduler.CommandBuffer(), scheduler, *input_view, {W, H}, 0.5f);
    const auto sharpened = read_back(cas.OutputImage());
    assert(std::abs(sharpened[10 * W + 2] - dark) < 0.01f && std::abs(sharpened[10 * W + 60] - light) < 0.01f);
    // Across the edge (row 10 steps at x = 26): darker on the dark side, brighter on the light one.
    std::printf("CAS edge: %.3f %.3f (input %.3f %.3f)\n", sharpened[10 * W + 25], sharpened[10 * W + 26], dark, light);
    assert(sharpened[10 * W + 25] < dark - 0.01f && sharpened[10 * W + 26] > light + 0.01f);
    // Black and white pixels (the game's letterbox, UI text) stay finite and in range.
    for (u32 i = 56 * W; i < 64 * W; ++i) {
        assert(std::isfinite(sharpened[i]) && sharpened[i] >= 0.f && sharpened[i] <= 1.f);
    }
    std::printf("CAS black/white: %.3f %.3f\n", sharpened[60 * W + 10], sharpened[60 * W + 11]);
    std::puts("PASS: CAS sharpens edges, keeps flat areas");

    auto& smaa = *new HostPasses::SmaaPass;
    smaa.Create(device, instance.GetAllocator());
    smaa.Render(scheduler.CommandBuffer(), scheduler, *input_view, {W, H}, false);
    const auto smoothed = read_back(smaa.OutputImage());
    assert(std::abs(smoothed[10 * W + 2] - dark) < 0.01f && std::abs(smoothed[10 * W + 60] - light) < 0.01f);
    u32 blended = 0;
    for (u32 y = 4; y < 52; ++y) {
        for (u32 x = 0; x < W; ++x) {
            const float v = smoothed[y * W + x];
            blended += v > dark + 0.02f && v < light - 0.02f;
        }
    }
    std::printf("SMAA: %u blended pixels along the edge\n", blended);
    assert(blended > 20);
    std::puts("PASS: SMAA blends the stair-stepped edge, keeps flat areas");
    vmaDestroyBuffer(instance.GetAllocator(), staging, allocation);
}
