// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <memory>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Vulkan {

// tlg: bounded compiler queue. Stop drains accepted work before Vulkan resources are released.
class AsyncCompiler {
public:
    struct TraceHooks {
        std::function<uint64_t(bool,size_t)> enqueued;
        std::function<uint64_t(uint64_t)> begin;
        std::function<void(uint64_t,uint64_t)> end;
    };
    explicit AsyncCompiler(unsigned count, size_t capacity = 64,
                           std::function<void(unsigned)> initialize = {}, TraceHooks trace_hooks = {})
        : capacity{capacity}, trace{std::move(trace_hooks)} {
        for (unsigned i = 0; i < count; ++i) {
            workers.emplace_back([this, i, initialize] {
                worker_index = i;
                if (initialize) {
                    initialize(i);
                }
                for (;;) {
                    std::function<void()> task;
                    uint64_t operation = 0;
                    {
                        std::unique_lock lock{mutex};
                        cv.wait(lock, [&] { return stopping || !tasks.empty() || !urgent.empty(); });
                        if (tasks.empty() && urgent.empty()) {
                            return;
                        }
                        auto& queue = urgent.empty() ? tasks : urgent;
                        auto next = std::move(queue.front());
                        queue.pop_front();
                        queue_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - next.enqueued).count();
                        operation = next.operation;
                        task = std::move(next.function);
                    }
                    uint64_t parent = 0;
                    if (operation && trace.begin) { try { parent = trace.begin(operation); } catch (...) {} }
                    task();
                    if (operation && trace.end) { try { trace.end(operation,parent); } catch (...) {} }
                }
            });
        }
    }
    ~AsyncCompiler() { Stop(); }

    bool Submit(std::function<void()> task, bool priority = false) {
        {
            std::scoped_lock lock{mutex};
            if (stopping || tasks.size() + urgent.size() >= capacity) {
                return false;
            }
            uint64_t operation = 0;
            if (trace.enqueued) { try { operation = trace.enqueued(priority,tasks.size()+urgent.size()); } catch (...) {} }
            (priority ? urgent : tasks).push_back({std::move(task), std::chrono::steady_clock::now(),operation});
        }
        cv.notify_one();
        return true;
    }
    bool HasCapacity() {
        std::scoped_lock lock{mutex};
        return !stopping && tasks.size() + urgent.size() < capacity;
    }
    static unsigned WorkerIndex() { return worker_index; }
    size_t Depth() {
        std::scoped_lock lock{mutex};
        return tasks.size() + urgent.size();
    }
    uint64_t QueueWaitNs() { return queue_wait_ns.exchange(0); }
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
        }
        cv.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    size_t capacity;
    std::mutex mutex;
    std::condition_variable cv;
    struct Task {
        std::function<void()> function;
        std::chrono::steady_clock::time_point enqueued;
        uint64_t operation = 0;
    };
    std::deque<Task> tasks, urgent;
    static inline thread_local unsigned worker_index = 0;
    std::atomic<uint64_t> queue_wait_ns{};
    bool stopping = false;
    TraceHooks trace;
    std::vector<std::thread> workers;
};

// tlg: one job per cache entry; failures are terminal until the next session.
template <typename T>
class AsyncJob {
public:
    template <typename F>
    bool Start(AsyncCompiler& compiler, F&& function, bool priority = false) {
        if (started) {
            return false;
        }
        auto task = std::make_shared<std::packaged_task<T()>>(std::forward<F>(function));
        auto completion = task->get_future();
        if (!compiler.Submit([task] { (*task)(); }, priority)) {
            return false;
        }
        future = std::move(completion);
        started = true;
        return true;
    }
    bool Pending() const { return future.valid(); }
    bool Failed() const { return failed; }
    const std::string& Error() const { return error; }
    void Wait() const {
        if (future.valid()) {
            future.wait();
        }
    }
    /// True when the job has finished (or there is none) within `timeout`.
    template <typename Rep, typename Period>
    bool WaitFor(std::chrono::duration<Rep, Period> timeout) const {
        return !future.valid() || future.wait_for(timeout) == std::future_status::ready;
    }
    std::optional<T> Poll() {
        if (!future.valid() || future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return std::nullopt;
        }
        try {
            return future.get();
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Unknown compiler error";
        }
        failed = true;
        return std::nullopt;
    }

private:
    std::future<T> future;
    bool started = false;
    bool failed = false;
    std::string error;
};

// tlg: IR retains its pools until emission finishes, then returns them for another translation.
template <typename T>
class CompilerPool {
public:
    explicit CompilerPool(size_t capacity) : capacity{capacity} {}
    std::pair<std::unique_ptr<T>, bool> Acquire() {
        std::scoped_lock lock{mutex};
        if (available.empty()) {
            return {std::make_unique<T>(), false};
        }
        auto pool = std::move(available.back());
        available.pop_back();
        return {std::move(pool), true};
    }
    void Recycle(std::unique_ptr<T> pool) noexcept {
        try {
            pool->ReleaseContents();
            std::scoped_lock lock{mutex};
            if (available.size() < capacity) {
                available.push_back(std::move(pool));
            }
        } catch (...) {
            // Reuse is optional; discard the pool if resetting or retaining it fails.
        }
    }
    size_t Retained() {
        std::scoped_lock lock{mutex};
        return available.size();
    }
private:
    size_t capacity;
    std::mutex mutex;
    std::vector<std::unique_ptr<T>> available;
};

} // namespace Vulkan
