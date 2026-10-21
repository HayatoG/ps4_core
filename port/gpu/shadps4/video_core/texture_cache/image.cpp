// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/amdgpu/tlg_amd_1d.h"
#include "tlg_toggles.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include "common/assert.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/barrier_merge.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

namespace {
// tlg: RX 580 / Polaris crash (amdvlk64.dll+0xebbbb, issues #31/#49/#81/#99/#103). Two early
// experiments, both switchable and neither shown to be the cause (the crash stayed with both):
//  * TLG_EXPLICIT_RANGE (default on, 0 = old behaviour): full-image barriers carry the real
//    level and layer counts instead of VK_REMAINING_MIP_LEVELS / VK_REMAINING_ARRAY_LAYERS.
//    Identical by the spec (base 0).
//  * TLG_BARRIER_SPLIT=1 (default off): one barrier per mip level instead of one for all;
//    =2: one barrier per mip level and per layer.
bool ExplicitBarrierRange() {
    static const bool on = [] {
        const char* env = std::getenv("TLG_EXPLICIT_RANGE");
        const bool enabled = !(env && env[0] == '0');
        if (enabled) {
            std::printf("GPU: explicit barrier ranges (TLG_EXPLICIT_RANGE=0 keeps VK_REMAINING_*)\n");
        }
        return enabled;
    }();
    return on;
}

// 0 = one barrier for the whole image, 1 = one per mip level, 2 = one per mip level and layer.
int BarrierSplitMode() {
    static const int mode = [] {
        const char* env = std::getenv("TLG_BARRIER_SPLIT");
        const int value = (env && env[0] >= '1' && env[0] <= '2') ? env[0] - '0' : 0;
        if (value != 0) {
            std::printf("GPU: split full-image barriers, mode %d (TLG_BARRIER_SPLIT)\n", value);
        }
        return value;
    }();
    return mode;
}

// Opt-in for the RX 580 investigation (#232/#278). Use the same strict switch as the launcher.
bool MergeBarriersEnabled() {
    static const bool enabled = [] {
        const bool on = IsBarrierMergeEnabled(std::getenv("TLG_BARRIER_MERGE"));
        if (on) {
            std::printf("GPU: merging adjacent subresource barriers (TLG_BARRIER_MERGE=1)\n");
        }
        return on;
    }();
    return enabled;
}

/// tlg: how many times the driver has refused this format/type/flags/usage combination. The
/// first, tenth, hundredth... are logged, so the magnitude stays visible without one line per
/// streamed image.
std::mutex unsupported_formats_mutex;
std::unordered_map<u64, u64> unsupported_formats;

u64 UnsupportedFormatCount(u64 key) {
    std::scoped_lock lock{unsupported_formats_mutex};
    return ++unsupported_formats[key];
}

bool IsPowerOfTen(u64 n) {
    for (u64 p = 1; p <= n; p *= 10) {
        if (p == n) {
            return true;
        }
    }
    return false;
}
} // namespace

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance& instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (info.props.is_bc_decoded) {
        // tlg: TLG_BC_TINY: a texture only ever filled by uploads and sampled.
        return usage;
    }
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used.
            usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        // tlg: BP09, 1D images are 2D of height 1 on the host.
        return AmdGpu::All1DAs2D() ? vk::ImageType::e2D : vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

UniqueImage::~UniqueImage() {
    if (image) {
        TLG_IMG_TRACE("destroy image %p\n", (void*)VkImage(image));
        vmaDestroyImage(allocator, image, allocation);
    }
}

void UniqueImage::Destroy() {
    if (image) {
        TLG_IMG_TRACE("destroy image %p\n", (void*)VkImage(image));
        vmaDestroyImage(allocator, image, allocation);
        image = vk::Image{};
        allocation = {};
    }
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VmaAllocationInfo alloc_info{};
    VkResult result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image,
                                     &allocation, &alloc_info);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        // tlg: see UniqueBuffer::Create: past the budget, the driver moves memory out instead of
        // failing (shadPS4-Infamous-FixBuild e656365e).
        LOG_WARNING(Render_Vulkan, "Allocating an image went over the memory budget, allocating "
                                   "it past the budget");
        VmaAllocationCreateInfo over_budget_ci = alloc_ci;
        over_budget_ci.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = vmaCreateImage(allocator, &image_ci_unsafe, &over_budget_ci, &unsafe_image,
                                &allocation, &alloc_info);
    }
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating image with error {}",
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
    size_bytes = alloc_info.size;
}

Image::Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime_,
             Common::SlotVector<ImageView>& slot_image_views_, const ImageInfo& info_)
    : guest_begin{info_.guest_address}, guest_end{info_.guest_address + info_.guest_size},
      info{info_}, runtime{&runtime_}, slot_image_views{&slot_image_views_} {
    TlgStats::Timer timer{TlgStats::t_image_create};
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }

    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance.Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && !info.props.is_bc_decoded && instance.IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    usage_flags = ImageUsageFlags(instance, info);
    // tlg: TLG_1D_NO_STORAGE / TLG_1D_NO_MUTABLE (video_core/amdgpu/tlg_amd_1d.h).
    const bool host_1d = !info.props.is_host_2d && ConvertImageType(info.type) == vk::ImageType::e1D;
    if (host_1d && AmdGpu::img_1d_no_storage.load(std::memory_order_relaxed)) {
        usage_flags &= ~vk::ImageUsageFlagBits::eStorage;
    }
    if (host_1d && AmdGpu::img_1d_no_mutable.load(std::memory_order_relaxed)) {
        flags &= ~(vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage);
    }
    format_features = FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance.GetSupportedFormat(info.pixel_format, format_features);
    vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = supported_format,
        .type = (info.props.is_host_2d ? vk::ImageType::e2D : ConvertImageType(info.type)),
        .tiling = tiling,
        .usage = usage_flags,
        .flags = flags,
    };
    auto image_format_properties =
        instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
    // tlg: storage usage is speculative (see ImageUsageFlags). Drivers that refuse it for a
    // format (AMD's Windows driver for BC6H) get the image without it: creating the unsupported
    // combination anyway loses the device a few frames later.
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported &&
        (usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        format_info.usage = usage_flags & ~vk::ImageUsageFlagBits::eStorage;
        image_format_properties = instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
        if (image_format_properties.result == vk::Result::eSuccess) {
            usage_flags = format_info.usage;
            format_features = FormatFeatureFlags(usage_flags);
        } else {
            format_info.usage = usage_flags;
        }
    }
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported) {
        // tlg: the game streams these all session long (BC 1D on NVIDIA: 178 refused creations in
        // 5 minutes, every one at a different address), and the image is created and sampled
        // correctly anyway, so one line per creation buries the real errors in a player's log.
        // One line per distinct format/type/flags/usage at 1, 10, 100... occurrences.
        const u64 key = (static_cast<u64>(static_cast<u32>(supported_format)) << 32) ^
                        (static_cast<u64>(static_cast<u32>(format_info.type)) << 24) ^
                        (static_cast<u64>(static_cast<u32>(
                             static_cast<vk::ImageCreateFlags::MaskType>(format_info.flags)))
                         << 12) ^
                        static_cast<u64>(
                            static_cast<vk::ImageUsageFlags::MaskType>(format_info.usage));
        if (const u64 count = UnsupportedFormatCount(key); IsPowerOfTen(count)) {
            LOG_ERROR(Render_Vulkan,
                      "image format {} type {} is not supported (flags {}, usage {}); {} so far, "
                      "logged at 1, 10, 100...",
                      vk::to_string(supported_format), vk::to_string(format_info.type),
                      vk::to_string(format_info.flags), vk::to_string(format_info.usage), count);
        }
        // tlg: TLG_BC1D_LOG=1 (PROPOSTA_BC1D_NVIDIA.diff), diagnostics only: one line per image
        // created although the driver reports its format/type unsupported (BC 1D on NVIDIA), to
        // tell streaming churn from one hot image and to match against flicker moments.
        static const bool bc1d_log = [] {
            const char* e = std::getenv("TLG_BC1D_LOG");
            return e && *e == '1';
        }();
        if (bc1d_log) {
            static std::atomic<u32> created{0};
            LOG_WARNING(Render_Vulkan,
                        "tlg: unsupported image #{} {} {} {}x{} layers {} mips {} tile {} at "
                        "{:#x}+{:#x} tick {}",
                        created.fetch_add(1) + 1, vk::to_string(supported_format),
                        vk::to_string(format_info.type), info.size.width, info.size.height,
                        info.resources.layers, info.resources.levels, AmdGpu::NameOf(info.tile_mode),
                        info.guest_address, info.guest_size, runtime->GetScheduler().CurrentTick());
        }
    }
    supported_samples = image_format_properties.result == vk::Result::eSuccess
                            ? image_format_properties.value.imageFormatProperties.sampleCounts
                            : vk::SampleCountFlagBits::e1;

    const vk::ImageCreateInfo image_ci = {
        .flags = flags,
        .imageType = (info.props.is_host_2d ? vk::ImageType::e2D : ConvertImageType(info.type)),
        .format = supported_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = LiverpoolToVK::NumSamples(info.num_samples, supported_samples),
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    backing = &backing_images.emplace_back();
    backing->num_samples = info.num_samples;
    backing->image = UniqueImage{instance.GetDevice(), instance.GetAllocator()};
    // tlg: every height-1 image is traced, whatever its Vulkan type: the crash image was a
    // height-1 image that the old 1D-only creation trace never showed.
    const bool trace_thin = host_1d || (!info.props.is_host_2d && info.size.height == 1);
    if (trace_thin) {
        TLG_IMG_TRACE("create thin image %ux%u layers %u mips %u %s vk type %s guest type %u "
                      "tile %u flags %s usage %s at %#llx\n",
                      info.size.width, info.size.height, u32(info.resources.layers),
                      u32(info.resources.levels), vk::to_string(supported_format).c_str(),
                      vk::to_string(image_ci.imageType).c_str(), u32(info.type),
                      u32(info.tile_mode), vk::to_string(flags).c_str(),
                      vk::to_string(usage_flags).c_str(), (unsigned long long)info.guest_address);
    }
    backing->image.Create(image_ci);
    if (trace_thin) {
        TLG_IMG_TRACE("created thin image %p\n", (void*)VkImage(backing->image.image));
    }

    Vulkan::SetObjectName(instance.GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        runtime->SetBackingSamples(this, info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    if (backing->last_view < view_infos.size() && view_infos[backing->last_view] == view_info) {
        return (*slot_image_views)[backing->image_view_ids[backing->last_view]];
    }
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        backing->last_view = static_cast<u32>(std::distance(view_infos.begin(), it));
        const auto view_id = backing->image_view_ids[backing->last_view];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->insert(runtime->GetInstance(), view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

void Image::GetBarriers(Barriers& barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                        vk::PipelineStageFlags2 dst_stage,
                        std::optional<SubresourceRange> subres_range) {
    auto& last_state = backing->state;
    auto& subresource_states = backing->subresource_states;

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != info.resources);
    const bool partially_transited = !subresource_states.empty();

    constexpr auto memo_write_flags = vk::AccessFlagBits2::eTransferWrite |
                                      vk::AccessFlagBits2::eShaderWrite |
                                      vk::AccessFlagBits2::eMemoryWrite;
    auto& memo = backing->partial_memo;
    const auto first_new_barrier = barriers.size();
    if (needs_partial_transition && partially_transited && memo.valid &&
        memo.range == *subres_range && memo.layout == dst_layout && memo.access == dst_mask &&
        memo.stage == dst_stage) {
        return;
    }
    memo.valid = false;
    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(info.resources.levels * info.resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
        }

        // In case of partial transition, we need to change the specified subresources only.
        // Otherwise all subresources need to be set to the same state so we can use a full
        // resource transition for the next time.
        const auto mips =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.level,
                                           subres_range->base.level + subres_range->extent.levels)
                : std::views::iota(0u, info.resources.levels);
        const auto layers =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.layer,
                                           subres_range->base.layer + subres_range->extent.layers)
                : std::views::iota(0u, info.resources.layers);

        for (u32 mip : mips) {
            for (u32 layer : layers) {
                // Optional coalescing below folds equal neighbouring transitions after this loop.
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    barriers.emplace_back(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = state.layout,
                        .newLayout = dst_layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetImage(),
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
        } else if (!(dst_mask & memo_write_flags)) {
            memo = {true, *subres_range, dst_layout, dst_mask, dst_stage};
        }
    } else { // Full resource transition
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask && !is_write) {
            return;
        }

        // tlg: see ExplicitBarrierRange / BarrierSplitMode above.
        const u32 total_levels = static_cast<u32>(info.resources.levels);
        const u32 total_layers = static_cast<u32>(info.resources.layers);
        const bool explicit_range = ExplicitBarrierRange() && total_levels > 0 && total_layers > 0;
        const u32 level_count = explicit_range ? total_levels : VK_REMAINING_MIP_LEVELS;
        const u32 layer_count = explicit_range ? total_layers : VK_REMAINING_ARRAY_LAYERS;
        const int split_mode = explicit_range ? BarrierSplitMode() : 0;
        const bool split_mips = split_mode >= 1 && total_levels > 1;
        const bool split_layers = split_mode >= 2 && total_layers > 1;
        const u32 mip_barriers = split_mips ? total_levels : 1;
        const u32 layer_barriers = split_layers ? total_layers : 1;
        for (u32 level = 0; level < mip_barriers; ++level) {
            for (u32 layer = 0; layer < layer_barriers; ++layer) {
                barriers.emplace_back(vk::ImageMemoryBarrier2{
                    .srcStageMask = last_state.pl_stage,
                    .srcAccessMask = last_state.access_mask,
                    .dstStageMask = dst_stage,
                    .dstAccessMask = dst_mask,
                    .oldLayout = last_state.layout,
                    .newLayout = dst_layout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = GetImage(),
                    .subresourceRange{
                        .aspectMask = aspect_mask,
                        .baseMipLevel = split_mips ? level : 0,
                        .levelCount = split_mips ? 1 : level_count,
                        .baseArrayLayer = split_layers ? layer : 0,
                        .layerCount = split_layers ? 1 : layer_count,
                    },
                });
            }
        }
    }

    // Merge only barriers emitted by this call, once, using the shared two-pass helper.
    if (barriers.size() >= first_new_barrier + 2 && MergeBarriersEnabled()) {
        MergeAdjacentBarriers(barriers, first_new_barrier);
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;
}

} // namespace VideoCore
