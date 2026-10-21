// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
namespace TlgPerf {
enum class Kind : std::uint16_t {
    Frame = 1,
    Marker,
    PackageOpen,
    PipelineRequest,
    CompileBegin,
    CompileEnd,
    WaitBegin,
    WaitEnd,
    ResourceCreate,
    ResourceEvict,
    ResourceUpload,
    FaultBegin,
    FaultEnd,
    ShaderLoad,
    ShaderTranslate,
    MemoryPressure,
    CaptureStatus,
    PipelineFeedback,
    QueueEnqueue,
    QueueBegin,
    QueueEnd,
    ResourceRead,
    ReadbackApply,
    FaultContext
};
// Numeric only. Never put a guest pointer, file path, or game data into an event: data addresses
// go through Hash(). Code offsets inside the game executable (as in the fault profile log) are
// allowed, since they name a code site, not memory contents.
struct Event {
    std::uint64_t ts_ns = 0, frame = 0, operation = 0, object = 0, generation = 0, a = 0, b = 0;
    Kind kind = Kind::CaptureStatus;
    std::uint16_t thread = 0;
    std::uint32_t role = 0;
};
static_assert(sizeof(Event) == 64);
struct Options {
    std::filesystem::path folder;
    std::uint64_t pre_ns = 5'000'000'000, post_ns = 2'000'000'000, freeze_ns = 100'000'000;
    std::uint64_t max_bytes = 128 * 1024 * 1024;
    unsigned max_captures = 10, poll_ms = 5;
    bool detailed = true; // false: only the common frame series for overhead comparisons
#ifdef TLG_PERF_TESTS
    void (*after_drain)() noexcept = nullptr;
    void (*on_shutdown)() noexcept = nullptr;
#endif
};
// Vulkan creation feedback flags: VALID=1, APPLICATION_CACHE_HIT=2, BASE_ACCELERATION=4.
// Return 0 unknown, 1 application cache hit, 2 valid without application cache hit.
constexpr unsigned CacheFeedback(unsigned flags) noexcept {
    return !(flags & 1) ? 0 : (flags & 2) ? 1 : 2;
}
namespace detail {
inline std::atomic<bool> detailed{false}, frames{false};
}
// Inline so a disabled hook costs one relaxed load, not a call into another translation unit.
inline bool Enabled() noexcept { return detail::detailed.load(std::memory_order_relaxed); }
inline bool FramesEnabled() noexcept { return detail::frames.load(std::memory_order_relaxed); }
bool Start(const Options &) noexcept; // control thread only; Start/Stop must be serialized
void InitFromEnvironment() noexcept;
void Stop() noexcept;
bool RegisterThread(std::uint32_t role = 0) noexcept;
void Emit(Event) noexcept;
bool Mark(std::uint32_t marker_id) noexcept;
void FramePresented(std::uint64_t frame_id, std::uint64_t interval_ns) noexcept;
std::uint64_t NextOperation() noexcept;
std::uint64_t Enqueue(std::uint64_t queue, bool priority, std::uint64_t depth) noexcept;
std::uint64_t QueueBegin(std::uint64_t operation, std::uint64_t queue) noexcept;
void QueueEnd(std::uint64_t operation, std::uint64_t parent, std::uint64_t queue) noexcept;
// Per-session salted ID. The random salt never leaves memory, so a capture alone cannot be
// mapped back to an address.
std::uint64_t Hash(std::uint64_t value) noexcept;
constexpr std::uint64_t Label(const char *s) noexcept {
    std::uint64_t x = 14695981039346656037ULL;
    for (; *s; ++s) {
        x ^= static_cast<unsigned char>(*s);
        x *= 1099511628211ULL;
    }
    return x;
} // stable non-personal label, e.g. file basename
class Span {
    Kind end_;
    std::uint64_t operation_ = 0, object_ = 0, generation_ = 0, parent_ = 0;
    void Begin(Kind begin) noexcept;
    void End() noexcept;

  public:
    Span(Kind begin, Kind end, std::uint64_t object = 0, std::uint64_t generation = 0) noexcept
        : end_(end), object_(object), generation_(generation) {
        if (Enabled())
            Begin(begin);
    }
    ~Span() {
        if (operation_)
            End();
    }
    std::uint64_t Id() const noexcept { return operation_; }
    Span(const Span &) = delete;
    Span &operator=(const Span &) = delete;
};
} // namespace TlgPerf
