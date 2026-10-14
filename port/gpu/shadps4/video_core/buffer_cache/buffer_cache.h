// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "tlg_copy.h"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // tlg: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 30;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (tlg: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// tlg: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);
    /// tlg: a CPU read fault on pages with no GPU-modified data: lift the read protection on the
    /// faulting thread instead of a round trip through the GPU command thread.
    void ReleaseReadProtection(VAddr device_addr, u64 size);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

public:
    /// tlg: TLG_WRITE_MERGE=1 (precise readbacks): a guest memcpy/memset into memory with
    /// GPU-modified pages writes through the backing view without a fault or a GPU wait, and the
    /// exact byte range is uploaded before the GPU next uses or downloads it. Bytes the GPU
    /// wrote elsewhere on those pages stay intact. False: the caller writes normally.
    bool CpuWriteMerge(VAddr dst, const void* src, int fill, u64 size);

    /// tlg: TLG_EAGER_READBACK=1 (precise readbacks): pages the guest reads back after GPU
    /// writes are learnt (NoteReadback). At every fence they are copied to host memory if the
    /// GPU wrote them since, and the batch is submitted (EagerCopyAtFence). A later CPU read
    /// fault on such a page is served on the faulting thread (TryEagerRead): it waits only for
    /// that copy, not for the GPU command thread nor for a full GPU idle.
    bool TryEagerRead(VAddr addr, u64 size);
    void NoteReadback(VAddr addr, u64 size);
    void EagerCopyAtFence();

private:
    /// Uploads the merged guest writes inside [device_addr, device_addr + size) of `arena`.
    void FlushMergedWrites(const Buffer* arena, VAddr device_addr, u64 size);
    std::mutex merge_mutex;
    std::map<VAddr, VAddr> merged_writes; ///< start -> end, disjoint
    std::atomic<u64> merged_count{0};

    struct EagerPage {
        u64 offset;   ///< in eager_buffer
        u64 tick;     ///< submission holding the last copy
        bool dirty;   ///< GPU wrote it since the last copy
        bool valid;   ///< the copy at `tick` is current
        u32 idle;     ///< fences since the guest last read it
    };
    static bool EagerEnabled();
    void NoteGpuWriteForEager(VAddr addr, u64 size); ///< GPU-side threads, before the write
    void MarkEagerStale(VAddr addr, u64 size);       ///< any thread: CPU bytes the copy lacks
    void ApplyEagerSubtractsLocked();                ///< GPU-side threads, eager_mutex held
    std::mutex eager_mutex;
    std::map<VAddr, EagerPage> eager_pages; ///< by 4 KiB page address
    std::vector<VAddr> eager_free_offsets;
    u64 eager_next_offset = 0;
    std::atomic<u32> eager_count{0};
    std::vector<VAddr> eager_subtracts; ///< pages served on guest threads: leave gpu_modified_ranges
    std::unique_ptr<Buffer> eager_buffer;

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// tlg: batched small copies on the copy threads (TlgCopy::QueueCopy).
    static void RunGuestCopy(const TlgCopy::Item& item);
    void SmallGuestCopy(const TlgCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // tlg: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;

    u32 arena_memory_type_index{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
