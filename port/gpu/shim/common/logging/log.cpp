// tlg: see log.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include "common/logging/log.h"

namespace Common::Log {
static Level threshold() {
    static const Level level = [] {
        const char* v = std::getenv("TLG_GPU_LOG");
        if (!v) return Level::warn;
        static const char* names[] = {"trace", "debug", "info", "warning", "error", "critical"};
        for (int i = 0; i < 6; ++i)
            if (!std::strcmp(v, names[i])) return Level(i);
        return Level::warn;
    }();
    return level;
}
// Detailed log mode (TLG_VERBOSE_LOG=1/2, set by the launcher as ISS_VERBOSE_LOG): every line gets
// the time since the first log call and the thread. Read once; the default format is unchanged.
static bool verbose() {
    static const bool on = [] {
        const char* v = std::getenv("TLG_VERBOSE_LOG");
        return v && std::atoi(v) > 0;
    }();
    return on;
}
static unsigned long thread_id() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentThreadId());
#else
    static thread_local const unsigned long tid = static_cast<unsigned long>(::syscall(SYS_gettid));
    return tid;
#endif
}
bool ShouldLog(Level level) { return level >= threshold(); }
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message) {
    static std::mutex mutex;
    static const char* names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical"};
    const char* base = std::strrchr(file, '/');
    if (verbose()) {
        static const auto start = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::scoped_lock lock{mutex};
        std::fprintf(stderr, "[%10.3f ms] [tid %lu] GPU [%.*s] <%s> %s:%d %s: %s\n", ms, thread_id(),
                     int(log_class.size()), log_class.data(), names[int(level)], base ? base + 1 : file,
                     line, function, message.c_str());
        return;
    }
    std::scoped_lock lock{mutex};
    std::fprintf(stderr, "GPU [%.*s] <%s> %s:%d %s: %s\n", int(log_class.size()), log_class.data(),
                 names[int(level)], base ? base + 1 : file, line, function, message.c_str());
}
} // namespace Common::Log
