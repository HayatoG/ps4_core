// SPDX-License-Identifier: GPL-2.0-or-later

#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <vector>

#include "video_core/texture_cache/barrier_merge.h"

using VideoCore::MergeAdjacentBarriers;

static vk::ImageMemoryBarrier2 MakeBarrier(std::uint32_t mip, std::uint32_t layer,
                                           vk::ImageLayout old_layout = vk::ImageLayout::eGeneral) {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    barrier.oldLayout = old_layout;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vk::Image{reinterpret_cast<VkImage>(std::uintptr_t{1})};
    barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, mip, 1, layer, 1};
    return barrier;
}

int main() {
    assert(!VideoCore::IsBarrierMergeEnabled(nullptr));
    assert(!VideoCore::IsBarrierMergeEnabled("0"));
    assert(!VideoCore::IsBarrierMergeEnabled("true"));
    assert(!VideoCore::IsBarrierMergeEnabled("10"));
    assert(!VideoCore::IsBarrierMergeEnabled("1 "));
    assert(!VideoCore::IsBarrierMergeEnabled(""));
    assert(VideoCore::IsBarrierMergeEnabled("1"));

    std::vector<vk::ImageMemoryBarrier2> empty;
    MergeAdjacentBarriers(empty, 0);
    assert(empty.empty());

    std::vector<vk::ImageMemoryBarrier2> barriers{MakeBarrier(9, 9)};
    for (std::uint32_t mip = 0; mip < 6; ++mip) {
        for (std::uint32_t layer = 0; layer < 1536; ++layer) {
            barriers.push_back(MakeBarrier(mip, layer));
        }
    }
    MergeAdjacentBarriers(barriers, 1);
    assert(barriers.size() == 2);
    assert(barriers[0].subresourceRange.baseMipLevel == 9);
    assert(barriers[1].subresourceRange.baseMipLevel == 0);
    assert(barriers[1].subresourceRange.levelCount == 6);
    assert(barriers[1].subresourceRange.baseArrayLayer == 0);
    assert(barriers[1].subresourceRange.layerCount == 1536);

    const auto expect_separate = [](auto mutate) {
        auto a = MakeBarrier(0, 0);
        auto b = MakeBarrier(0, 1);
        mutate(a, b);
        std::vector<vk::ImageMemoryBarrier2> pair{a, b};
        MergeAdjacentBarriers(pair, 0);
        assert(pair.size() == 2);
    };
    expect_separate([](auto&, auto& b) { b.image = vk::Image{reinterpret_cast<VkImage>(std::uintptr_t{2})}; });
    expect_separate([](auto&, auto& b) { b.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth; });
    expect_separate([](auto&, auto& b) { b.srcStageMask = vk::PipelineStageFlagBits2::eTransfer; });
    expect_separate([](auto&, auto& b) { b.srcAccessMask = vk::AccessFlagBits2::eTransferWrite; });
    expect_separate([](auto&, auto& b) { b.dstStageMask = vk::PipelineStageFlagBits2::eTransfer; });
    expect_separate([](auto&, auto& b) { b.dstAccessMask = vk::AccessFlagBits2::eTransferRead; });
    expect_separate([](auto&, auto& b) { b.oldLayout = vk::ImageLayout::eTransferDstOptimal; });
    expect_separate([](auto&, auto& b) { b.newLayout = vk::ImageLayout::eGeneral; });
    expect_separate([](auto&, auto& b) { b.srcQueueFamilyIndex = 3; });
    expect_separate([](auto&, auto& b) { b.dstQueueFamilyIndex = 4; });

    std::vector<vk::ImageMemoryBarrier2> noncontiguous{MakeBarrier(0, 0), MakeBarrier(0, 2)};
    MergeAdjacentBarriers(noncontiguous, 0);
    assert(noncontiguous.size() == 2);
    // A different prior layout in the middle must keep its own dependency and range.
    std::vector<vk::ImageMemoryBarrier2> mixed{
        MakeBarrier(0, 0), MakeBarrier(0, 1, vk::ImageLayout::eTransferDstOptimal),
        MakeBarrier(0, 2), MakeBarrier(0, 3),
    };
    MergeAdjacentBarriers(mixed, 0);
    assert(mixed.size() == 3);
    assert(mixed[1].oldLayout == vk::ImageLayout::eTransferDstOptimal);
    assert(mixed[1].subresourceRange.baseArrayLayer == 1);
    assert(mixed[1].subresourceRange.layerCount == 1);
    assert(mixed[2].subresourceRange.baseArrayLayer == 2);
    assert(mixed[2].subresourceRange.layerCount == 2);

    // Disjoint mip ranges cannot be collapsed across a missing mip.
    std::vector<vk::ImageMemoryBarrier2> mip_gap{MakeBarrier(0, 0), MakeBarrier(2, 0)};
    MergeAdjacentBarriers(mip_gap, 0);
    assert(mip_gap.size() == 2);

}
