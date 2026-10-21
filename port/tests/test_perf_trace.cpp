// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_perf_trace.h"
#include "video_core/renderer_vulkan/vk_async_compiler.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition);           \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)
namespace {
std::atomic<bool> drained{false}, shutdown_requested{false};
void AfterDrain() noexcept {
    if (!drained.exchange(true))
        while (!shutdown_requested.load())
            std::this_thread::yield();
}
void OnShutdown() noexcept { shutdown_requested.store(true); }
void TestShutdown(const std::filesystem::path &folder) {
    TlgPerf::Options opt;
    opt.folder = folder;
    opt.after_drain = AfterDrain;
    opt.on_shutdown = OnShutdown;
    CHECK(TlgPerf::Start(opt));
    while (!drained.load())
        std::this_thread::yield();
    // Publish after the consumer has drained, then stop before its quit check.
    TlgPerf::FramePresented(1, 16'000'000);
    TlgPerf::FramePresented(2, 16'000'000);
    CHECK(TlgPerf::Mark(1));
    TlgPerf::Stop();
    unsigned rows = 0;
    for (auto &p : std::filesystem::recursive_directory_iterator(folder)) {
        if (p.path().filename() != "frames.csv")
            continue;
        std::ifstream f(p.path());
        std::string line;
        while (std::getline(f, line))
            rows += line.starts_with("1,") || line.starts_with("2,");
    }
    CHECK(rows == 2);
}
void TestRegistrationLoss(const std::filesystem::path &folder) {
    TlgPerf::Options opt;
    opt.folder = folder;
    opt.post_ns = opt.freeze_ns = 60'000'000'000;
    CHECK(TlgPerf::Start(opt));
    CHECK(TlgPerf::RegisterThread());
    CHECK(TlgPerf::Mark(1));
    for (unsigned i = 1; i < 128; ++i) {
        std::thread t([] { CHECK(TlgPerf::RegisterThread()); });
        t.join();
    }
    std::thread excess([] {
        for (unsigned i = 0; i < 3; ++i)
            TlgPerf::Emit({});
    });
    excess.join();
    TlgPerf::Stop();
    bool counted = false;
    for (auto &p : std::filesystem::recursive_directory_iterator(folder)) {
        if (p.path().extension() != ".jsonl")
            continue;
        std::ifstream f(p.path());
        std::string line;
        while (std::getline(f, line))
            counted |= line.find("\"dropped_total\":3,") != std::string::npos &&
                       line.find("\"dropped_unregistered\":3,") != std::string::npos;
    }
    CHECK(counted);
}
void TestOverflowReason(const std::filesystem::path &folder) {
    TlgPerf::Options opt;
    opt.folder = folder;
    opt.after_drain = AfterDrain;
    opt.on_shutdown = OnShutdown;
    CHECK(TlgPerf::Start(opt));
    while (!drained.load())
        std::this_thread::yield();
    CHECK(TlgPerf::Mark(1));
    for (unsigned i = 0; i < 10000; ++i)
        TlgPerf::Emit({});
    TlgPerf::Stop();
    bool classified = false, frames_classified = false;
    for (auto &p : std::filesystem::recursive_directory_iterator(folder)) {
        if (p.path().filename() == "frames.csv") {
            std::ifstream f(p.path());
            std::string line;
            while (std::getline(f, line)) {
                auto at = line.find("dropped_ring=");
                if (at == std::string::npos)
                    continue;
                auto lost = std::stoull(line.substr(at + 13));
                frames_classified |=
                    lost > 0 &&
                    line.find("dropped_total=" + std::to_string(lost) + ",") != std::string::npos &&
                    line.find("dropped_unregistered=0,") != std::string::npos &&
                    line.find("dropped_reentrant=0,") != std::string::npos;
            }
        }
        if (p.path().extension() != ".jsonl")
            continue;
        std::ifstream f(p.path());
        std::string line;
        while (std::getline(f, line)) {
            auto at = line.find("\"dropped_ring\":");
            if (at == std::string::npos)
                continue;
            auto lost = std::stoull(line.substr(at + 15));
            classified |=
                lost > 0 &&
                line.find("\"dropped_total\":" + std::to_string(lost) + ",") != std::string::npos &&
                line.find("\"dropped_unregistered\":0,") != std::string::npos &&
                line.find("\"dropped_reentrant\":0,") != std::string::npos;
        }
    }
    CHECK(classified && frames_classified);
}
void TestFramesOnly(const std::filesystem::path &folder) {
    TlgPerf::Options opt;
    opt.folder = folder;
    opt.detailed = false;
    CHECK(TlgPerf::Start(opt));
    CHECK(!TlgPerf::Start(opt));
    CHECK(!TlgPerf::Enabled());
    CHECK(TlgPerf::FramesEnabled());
    CHECK(!TlgPerf::Mark(1));
    TlgPerf::Emit({});
    TlgPerf::FramePresented(1, 200'000'000);
    TlgPerf::Stop();
    CHECK(!TlgPerf::FramesEnabled());
    unsigned frames = 0, captures = 0;
    for (auto &p : std::filesystem::recursive_directory_iterator(folder)) {
        captures += p.path().extension() == ".jsonl";
        if (p.path().filename() != "frames.csv")
            continue;
        std::ifstream f(p.path());
        std::string line;
        while (std::getline(f, line))
            frames += line.starts_with("1,");
    }
    CHECK(frames == 1 && captures == 0);
}
} // namespace
int main(int argc, char **argv) {

    using namespace TlgPerf;
    static_assert(sizeof(Event) <= 64);
    CHECK(CacheFeedback(0) == 0);
    CHECK(CacheFeedback(1) == 2);
    CHECK(CacheFeedback(3) == 1);
    CHECK(CacheFeedback(5) == 2);
    {
        std::atomic<unsigned> enqueued{0}, begun{0}, ended{0};
        Vulkan::AsyncCompiler::TraceHooks hooks;
        hooks.enqueued = [&](bool, size_t) { return std::uint64_t(++enqueued); };
        hooks.begin = [&](std::uint64_t op) {
            CHECK(op > 0);
            ++begun;
            return std::uint64_t(0);
        };
        hooks.end = [&](std::uint64_t op, std::uint64_t) {
            CHECK(op > 0);
            ++ended;
        };
        Vulkan::AsyncCompiler queue(1, 4, {}, hooks);
        CHECK(queue.Submit([] {}));
        queue.Stop();
        CHECK(enqueued == 1 && begun == 1 && ended == 1);
    }
    CHECK(!Enabled());
    CHECK(!Mark(1));
    auto folder = std::filesystem::temp_directory_path() /
                  ("iss-perf-test-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(std::filesystem::create_directory(folder));
    if (argc > 1) {
        if (std::string(argv[1]) == "shutdown")
            TestShutdown(folder);
        else if (std::string(argv[1]) == "frames")
            TestFramesOnly(folder);
        else if (std::string(argv[1]) == "overflow")
            TestOverflowReason(folder);
        else
            TestRegistrationLoss(folder);
        std::filesystem::remove_all(folder);
        return 0;
    }
    Options opt;
    opt.folder = folder;
    opt.poll_ms = 1;
    opt.post_ns = 10'000'000;
    opt.freeze_ns = 10'000'000;
    opt.max_captures = 2;
    CHECK(Start(opt));
    CHECK(RegisterThread(1));
    FramePresented(1, 16'000'000);
    {
        Span s{Kind::WaitBegin, Kind::WaitEnd, 10, 7};
    }
    CHECK(Mark(1));
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; i++)
        writers.emplace_back([] {
            RegisterThread(2);
            for (int j = 0; j < 10000; j++) {
                Event e;
                e.object = 42;
                e.kind = Kind::PackageOpen;
                Emit(e);
            }
        });
    for (auto &t : writers)
        t.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Stop();
    CHECK(!Enabled());
    unsigned headers = 0, events = 0, footers = 0, frame_files = 0;
    for (auto &p : std::filesystem::recursive_directory_iterator(folder)) {
        if (!p.is_regular_file())
            continue;
        frame_files += p.path().filename() == "frames.csv";
        std::ifstream f(p.path());
        std::string line;
        while (std::getline(f, line)) {
            headers += line.find("session") != std::string::npos;
            events += line.find("ts_ns") != std::string::npos;
            footers += line.find("capture_end") != std::string::npos;
        }
    }
    CHECK(frame_files == 1);
    CHECK(headers >= 1 && headers <= 2 && events > 0 && footers == headers);
    // A failed path must not stop rendering or throw from producer code.
    opt.folder = folder / "not-a-dir";
    {
        std::ofstream f(opt.folder);
        f << "x";
    }
    CHECK(!Start(opt));
    CHECK(!Enabled());
    std::filesystem::remove_all(folder);
}
