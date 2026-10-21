// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_perf_trace.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#ifndef TLG_PERF_BUILD_ID
#define TLG_PERF_BUILD_ID "standalone-test"
#endif
namespace TlgPerf {
namespace {
constexpr unsigned Threads = 128, Capacity = 4096, History = 65536;
using Clock = std::chrono::steady_clock;
struct Ring {
    std::array<Event, Capacity> events{};
    std::atomic<std::uint64_t> head{0}, tail{0};
};
struct State {
    Options options;
    std::array<Ring, Threads> rings;
    std::atomic<unsigned> registered{0};
    std::atomic<std::uint64_t> dropped{0}, frame{0}, last_frame{0}, trigger{0};
    std::atomic<std::uint64_t> dropped_ring{0}, dropped_unregistered{0}, dropped_reentrant{0};
    std::atomic<bool> quit{false};
    std::thread consumer;
    Clock::time_point origin = Clock::now();
    std::uint64_t epoch = 0, salt = 0;
};
std::atomic<State *> current{nullptr};
std::atomic<std::uint64_t> active{0}, operations{1}, epochs{0};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<State *>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
thread_local std::uint64_t current_scope = 0;
thread_local struct Local {
    std::uint64_t epoch = 0;
    unsigned slot = Threads;
    unsigned role = 0;
    bool busy = false;
} local;
std::uint64_t Now(State &s) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - s.origin).count();
}
// Count readers before reading the pointer: Stop can safely retire it after clearing it.
struct Access {
    State *state;
    Access() : state(nullptr) {
        active.fetch_add(1);
        state = current.load();
    }
    ~Access() { active.fetch_sub(1); }
};
bool Register(State &s, unsigned role) {
    if (local.epoch == s.epoch)
        return local.slot < Threads;
    local.epoch = s.epoch;
    local.role = role;
    local.slot = s.registered.fetch_add(1);
    return local.slot < Threads;
}
bool Push(State &s, Event e) {
    if (local.busy) {
        s.dropped.fetch_add(1);
        s.dropped_reentrant.fetch_add(1);
        return false;
    }
    local.busy = true;
    if (!Register(s, 0)) {
        s.dropped.fetch_add(1);
        s.dropped_unregistered.fetch_add(1);
        local.busy = false;
        return false;
    }
    auto &r = s.rings[local.slot];
    auto h = r.head.load(std::memory_order_relaxed);
    if (h - r.tail.load(std::memory_order_acquire) >= Capacity) {
        s.dropped.fetch_add(1);
        s.dropped_ring.fetch_add(1);
        local.busy = false;
        return false;
    }
    e.ts_ns = Now(s);
    e.frame = s.frame.load(std::memory_order_relaxed);
    e.thread = static_cast<std::uint16_t>(local.slot + 1);
    e.role = local.role;
    r.events[h % Capacity] = e;
    r.head.store(h + 1, std::memory_order_release);
    local.busy = false;
    return true;
}
void Consume(State &s) {
    std::deque<Event> history;
    std::ofstream out;
    std::uint64_t bytes = 0, until = 0, start = 0, last_trigger = 0, first = 0, latest = 0;
    unsigned captures = 0;
    bool io_error = false, limited = false, limit_notice = false;
    std::ofstream frames(s.options.folder / "frames.csv");
    frames << "frame,ts_ns,interval_ns\n";
    std::uint64_t frame_bytes = 23;
    bytes += frame_bytes;
    bool frames_truncated = false;
    auto write = [&](const std::string &text) {
        if (!out || io_error)
            return;
        // Reserve room for the terminal status even when event data hits the limit.
        if (bytes + text.size() + 512 > s.options.max_bytes) {
            limited = true;
            return;
        }
        out << text;
        bytes += text.size();
        if (!out)
            io_error = true;
    };
    auto event = [&](const Event &e) {
        char b[400];
        int n = std::snprintf(
            b, sizeof(b),
            "{\"ts_ns\":%llu,\"frame\":%llu,\"op\":%llu,\"object\":%llu,\"generation\":%llu,\"a\":%"
            "llu,\"b\":%llu,\"kind\":%u,\"thread\":%u,\"role\":%u}\n",
            (unsigned long long)e.ts_ns, (unsigned long long)e.frame,
            (unsigned long long)e.operation, (unsigned long long)e.object,
            (unsigned long long)e.generation, (unsigned long long)e.a, (unsigned long long)e.b,
            (unsigned)e.kind, (unsigned)e.thread, e.role);
        if (n > 0 && n < (int)sizeof(b))
            write(std::string(b, n));
    };
    auto close = [&] {
        if (!out.is_open())
            return;
        std::string footer =
            "{\"capture_end\":true,\"dropped_total\":" + std::to_string(s.dropped.load()) +
            ",\"dropped_ring\":" + std::to_string(s.dropped_ring.load()) +
            ",\"dropped_unregistered\":" + std::to_string(s.dropped_unregistered.load()) +
            ",\"dropped_reentrant\":" + std::to_string(s.dropped_reentrant.load()) +
            ",\"first_ns\":" + std::to_string(first) + ",\"last_ns\":" + std::to_string(latest) +
            ",\"truncated\":" + (limited ? "true" : "false") +
            ",\"shutdown\":" + (s.quit.load() ? "true" : "false") + "}\n";
        if (bytes + footer.size() <= s.options.max_bytes) {
            out << footer;
            bytes += footer.size();
        }
        out.close();
    };
    try {
        while (true) {
            // Stop retires producers before setting quit. Only exit after a pass
            // that observes quit before draining, so every accepted event is read.
            const bool stopping = s.quit.load();
            std::vector<Event> batch;
            for (unsigned i = 0; i < std::min(Threads, s.registered.load()); ++i) {
                auto &r = s.rings[i];
                auto t = r.tail.load(std::memory_order_relaxed);
                auto h = r.head.load(std::memory_order_acquire);
                while (t < h) {
                    batch.push_back(r.events[t % Capacity]);
                    ++t;
                }
                r.tail.store(t, std::memory_order_release);
            }
#ifdef TLG_PERF_TESTS
            if (s.options.after_drain)
                s.options.after_drain();
#endif
            std::stable_sort(batch.begin(), batch.end(),
                             [](auto &a, auto &b) { return a.ts_ns < b.ts_ns; });
            auto now = Now(s);
            auto trigger = s.trigger.exchange(0);
            auto last = s.last_frame.load();
            if (last && now >= last && now - last >= s.options.freeze_ns && last != last_trigger) {
                trigger = now;
                last_trigger = last;
            }
            if (!s.options.detailed)
                trigger = 0;
            if (trigger && !out.is_open() && captures < s.options.max_captures && !limited &&
                !io_error) {
                start = now;
                until = now + s.options.post_ns;
                auto path = s.options.folder / ("capture-" + std::to_string(s.epoch) + "-" +
                                                std::to_string(++captures) + ".jsonl");
                // Start() creates a unique session directory: never overwrite another session.
                out.open(path, std::ios::out);
                if (!out) {
                    io_error = true;
                    std::fprintf(stderr, "Perf trace: capture write failed; gameplay continues\n");
                } else {
                    write("{\"session\":1,\"build\":\"" TLG_PERF_BUILD_ID
                          "\",\"schema\":1,\"event_bytes\":64,\"pre_requested_ns\":" +
                          std::to_string(s.options.pre_ns) +
                          ",\"trigger_ns\":" + std::to_string(trigger) + "}\n");
                    first = history.empty() ? (batch.empty() ? now : batch.front().ts_ns)
                                            : history.front().ts_ns;
                    for (auto &e : history)
                        event(e);
                }
            } else if (trigger && !out.is_open() && !limit_notice) {
                std::fprintf(stderr, "Perf trace: capture limit reached or write unavailable; no "
                                     "further capture promised\n");
                limit_notice = true;
            } else if (trigger && out.is_open())
                until = std::min(start + s.options.pre_ns + s.options.post_ns,
                                 std::max(until, now + s.options.post_ns));
            for (auto &e : batch) {
                if (e.kind == Kind::Frame && frames && !frames_truncated) {
                    auto row = std::to_string(e.frame) + "," + std::to_string(e.ts_ns) + "," +
                               std::to_string(e.a) + "\n";
                    if (frame_bytes + row.size() > 16 * 1024 * 1024 ||
                        bytes + row.size() + 512 > s.options.max_bytes) {
                        frames_truncated = true;
                        std::fprintf(stderr, "Perf trace: frame series size limit reached\n");
                    } else {
                        frames << row;
                        frame_bytes += row.size();
                        bytes += row.size();
                    }
                }
                if (out.is_open())
                    event(e);
                history.push_back(e);
                latest = e.ts_ns;
            }
            if (!batch.empty()) {
                // A crash right after a hitch is the case worth diagnosing: keep what was read.
                frames.flush();
                if (out.is_open())
                    out.flush();
            }
            while (!history.empty() &&
                   (history.size() > History || now - history.front().ts_ns > s.options.pre_ns))
                history.pop_front();
            if (out.is_open() && (now >= until || limited || io_error || stopping))
                close();
            if (stopping) {
                if (frames)
                    frames << "# end,dropped_total=" << s.dropped.load()
                           << ",dropped_ring=" << s.dropped_ring.load()
                           << ",dropped_unregistered=" << s.dropped_unregistered.load()
                           << ",dropped_reentrant=" << s.dropped_reentrant.load()
                           << ",truncated=" << (frames_truncated ? 1 : 0) << "\n";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(s.options.poll_ms));
        }
    } catch (...) {
        io_error = true;
        close();
        std::fprintf(stderr, "Perf trace: consumer stopped; gameplay continues\n");
    }
}
} // namespace
bool Start(const Options &options) noexcept {
    if (current.load())
        return false;
    try {
        if (!options.max_captures || options.max_bytes < 2048 || !options.poll_ms)
            return false;
        std::filesystem::create_directories(options.folder);
        // Validate E/S on the control thread, never in a fault or GPU producer.
        auto s = std::make_unique<State>();
        s->options = options;
        s->epoch = epochs.fetch_add(1) + 1;
        // Random and kept in memory only: Hash() IDs cannot be mapped back to addresses.
        std::random_device random;
        s->salt = (static_cast<std::uint64_t>(random()) << 32) ^ random();
        // Unique directory, atomically allocated, to protect all earlier captures. Named by
        // wall-clock seconds so sessions sort by date; never by the salt.
        const auto wall = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
        auto base = options.folder;
        for (unsigned attempt = 0;; ++attempt) {
            s->options.folder =
                base / ("session-" + std::to_string(wall) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(s->options.folder))
                break;
        }
        auto probe = s->options.folder / ".write-test";
        {
            std::ofstream f(probe);
            if (!f)
                return false;
        }
        std::filesystem::remove(probe);
        s->consumer = std::thread([ptr = s.get()] {
            try {
                Consume(*ptr);
            } catch (...) {
                std::fprintf(stderr, "Perf trace: consumer unavailable; gameplay continues\n");
            }
        });
        current.store(s.release());
        detail::frames.store(true);
        detail::detailed.store(options.detailed);
        return true;
    } catch (...) {
        return false;
    }
}
void Stop() noexcept {
    detail::detailed.store(false);
    detail::frames.store(false);
    auto *s = current.exchange(nullptr);
    if (!s)
        return;
    while (active.load())
        std::this_thread::yield();
    s->quit.store(true);
#ifdef TLG_PERF_TESTS
    if (s->options.on_shutdown)
        s->options.on_shutdown();
#endif
    if (s->consumer.joinable())
        s->consumer.join();
    delete s;
}
void InitFromEnvironment() noexcept {
    const char *flag = std::getenv("TLG_PERF_TRACE");
    const char *frames = std::getenv("TLG_PERF_FRAMES");
    const bool full = flag && flag[0] == '1';
    if ((!full && (!frames || frames[0] != '1')) || current.load())
        return;
    try {
        Options o;
        o.detailed = full;
        const char *user = std::getenv("TLG_GPU_USER_DIR");
        o.folder = (user && *user ? std::filesystem::path(user) : std::filesystem::current_path()) /
                   "perf-traces";
        if (Start(o)) {
            std::atexit(Stop);
            RegisterThread(1);
            std::printf("Perf trace: %s (GPU user data)\n",
                        full ? "bounded captures enabled" : "frame series only");
        } else
            std::fprintf(stderr, "Perf trace: initialization failed; gameplay continues\n");
    } catch (...) {
    }
}
bool RegisterThread(std::uint32_t role) noexcept {
    Access a;
    return a.state && Register(*a.state, role);
}
void Emit(Event e) noexcept {
    if (!Enabled())
        return;
    Access a;
    if (a.state)
        Push(*a.state, e);
}
bool Mark(std::uint32_t id) noexcept {
    if (!Enabled())
        return false;
    Access a;
    if (!a.state)
        return false;
    Event e;
    e.kind = Kind::Marker;
    e.a = id;
    bool accepted = Push(*a.state, e);
    if (accepted)
        a.state->trigger.store(Now(*a.state) + 1);
    return accepted;
}
void FramePresented(std::uint64_t id, std::uint64_t ns) noexcept {
    if (!current.load(std::memory_order_relaxed))
        return;
    Access a;
    if (!a.state)
        return;
    auto &s = *a.state;
    s.frame.store(id);
    s.last_frame.store(Now(s) + 1);
    Event e;
    e.kind = Kind::Frame;
    e.a = ns;
    Push(s, e);
    if (s.options.detailed && ns >= s.options.freeze_ns)
        s.trigger.store(Now(s) + 1);
}
std::uint64_t Enqueue(std::uint64_t queue, bool priority, std::uint64_t depth) noexcept {
    if (!Enabled())
        return 0;
    auto op = NextOperation();
    Event e;
    e.kind = Kind::QueueEnqueue;
    e.operation = op;
    e.object = queue;
    e.a = depth;
    e.b = current_scope;
    e.generation = priority;
    Emit(e);
    return op;
}
std::uint64_t QueueBegin(std::uint64_t op, std::uint64_t queue) noexcept {
    auto parent = current_scope;
    current_scope = op;
    Event e;
    e.kind = Kind::QueueBegin;
    e.operation = op;
    e.object = queue;
    Emit(e);
    return parent;
}
void QueueEnd(std::uint64_t op, std::uint64_t parent, std::uint64_t queue) noexcept {
    Event e;
    e.kind = Kind::QueueEnd;
    e.operation = op;
    e.object = queue;
    Emit(e);
    current_scope = parent;
}
std::uint64_t NextOperation() noexcept {
    return operations.fetch_add(1, std::memory_order_relaxed);
}
std::uint64_t Hash(std::uint64_t x) noexcept {
    Access a;
    if (!a.state)
        return 0;
    x ^= a.state->salt;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
void Span::Begin(Kind begin) noexcept {
    operation_ = NextOperation();
    parent_ = current_scope;
    current_scope = operation_;
    Event e;
    e.kind = begin;
    e.operation = operation_;
    e.object = object_;
    e.generation = generation_;
    e.b = parent_;
    Emit(e);
}
void Span::End() noexcept {
    Event e;
    e.kind = end_;
    e.operation = operation_;
    e.object = object_;
    e.generation = generation_;
    e.b = parent_;
    Emit(e);
    if (current_scope == operation_)
        current_scope = parent_;
}
} // namespace TlgPerf
