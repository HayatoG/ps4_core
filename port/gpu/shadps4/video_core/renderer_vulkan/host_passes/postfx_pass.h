// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: optional filters on the game's final image, run by the presenter before FSR 1 / the post
// process pass (the game's UI is part of that image): SMAA 1x anti-aliasing and AMD CAS
// sharpening. Their images are created on first use, so a disabled filter costs nothing.

#pragma once

#include <array>
#include <initializer_list>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Scheduler;
}

namespace Vulkan::HostPasses {

/// SMAA 1x (Jimenez et al., MIT): luma edge detection, blending weights, neighborhood blending.
/// Keeps the input's encoding: an sRGB view (linear values) gives linear output.
class SmaaPass {
public:
    void Create(vk::Device device, VmaAllocator allocator);

    /// `input` is in ShaderReadOnlyOptimal; returns a view of the same size in that layout.
    vk::ImageView Render(vk::CommandBuffer cmdbuf, Scheduler& scheduler, vk::ImageView input,
                         vk::Extent2D size, bool input_linear);
    /// The image behind Render's result (tests).
    vk::Image OutputImage() const {
        return output.image;
    }

private:
    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
    };
    void CreateTarget(Target& target, vk::Format format, vk::Extent2D size, const char* name);
    void UploadLookupTextures(vk::CommandBuffer cmdbuf, Scheduler& scheduler);
    void Draw(vk::CommandBuffer cmdbuf, u32 pass, Target& target, vk::Extent2D size,
              std::initializer_list<vk::DescriptorImageInfo> inputs, bool input_linear);

    vk::Device device{};
    VmaAllocator allocator{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniquePipelineLayout pipeline_layout{};
    std::array<vk::UniquePipeline, 3> pipelines{};
    vk::UniqueSampler linear_sampler{};
    vk::UniqueSampler point_sampler{};
    Target area, search;
    bool lookup_ready = false;
    Target edges, weights, output;
    vk::Extent2D cur_size{};
};

/// AMD FidelityFX CAS (MIT), sharpen only: an alternative to FSR 1's RCAS. A pixel shader:
/// the compute version (like fsr.comp) wrote nothing in the game's frames on the NVIDIA 595
/// driver although it passed postfx-test.
class CasPass {
public:
    void Create(vk::Device device, VmaAllocator allocator);

    /// `sharpness` 0..1; `input` in ShaderReadOnlyOptimal, output the same size and layout.
    vk::ImageView Render(vk::CommandBuffer cmdbuf, Scheduler& scheduler, vk::ImageView input,
                         vk::Extent2D size, float sharpness);
    vk::Image OutputImage() const {
        return output;
    }

private:
    vk::Device device{};
    VmaAllocator allocator{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipeline pipeline{};
    vk::UniqueSampler sampler{};
    VideoCore::UniqueImage output;
    vk::UniqueImageView output_view;
    vk::Extent2D cur_size{};
};

/// TLG_POSTFX_PROFILE=1: GPU time of each presenter filter (timestamps around SMAA, FSR 1/RCAS,
/// CAS and the post process pass), read without waiting a few frames later, printed every 5 s.
class PostFxTimer {
public:
    enum Stage : u32 { Smaa, FsrRcas, Cas, PostProcess, StageCount };

    void Create(vk::Device device, float timestamp_period_ns);
    bool Enabled() const {
        return static_cast<bool>(pool);
    }
    /// Starts this frame's slot (outside a render pass).
    void Begin(vk::CommandBuffer cmdbuf);
    /// The end of `stage` (skipped stages measure 0).
    void Stamp(vk::CommandBuffer cmdbuf, Stage stage);
    /// After the last stamp: reads an old slot, prints the averages.
    void End(bool smaa, bool cas, bool color);

private:
    static constexpr u32 Ring = 8;
    static constexpr u32 Stamps = StageCount + 1;
    vk::Device device{};
    vk::UniqueQueryPool pool{};
    double period_ns = 1.0;
    u32 slot = 0;
    std::array<bool, Ring> written{};
    std::array<double, StageCount> total_ms{};
    u32 frames = 0;
    double window_start = 0;
};

} // namespace Vulkan::HostPasses
