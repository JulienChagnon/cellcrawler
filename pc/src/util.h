#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace pc {

inline uint64_t nowUs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

namespace detail {
inline std::mutex& logMutex() {
    static std::mutex m;
    return m;
}
inline FILE*& logFile() {
    static FILE* f = nullptr;
    return f;
}
}  // namespace detail

// Also writes every log line to path (the previous run's log is kept as <path>.prev).
inline void logToFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(detail::logMutex());
    const std::string prev = path + ".prev";
    std::remove(prev.c_str());
    std::rename(path.c_str(), prev.c_str());
    detail::logFile() = std::fopen(path.c_str(), "w");
}

inline void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(detail::logMutex());
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const double t = double(nowUs()) / 1e6;
    std::fprintf(stderr, "[%10.3f] ", t);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    if (FILE* f = detail::logFile()) {
        // Wall-clock time too, so a log can be matched to "it happened around 7:15".
        const std::time_t now = std::time(nullptr);
        char wall[16];
        std::strftime(wall, sizeof wall, "%H:%M:%S", std::localtime(&now));
        std::fprintf(f, "%s [%10.3f] ", wall, t);
        std::vfprintf(f, fmt, ap2);
        std::fputc('\n', f);
        std::fflush(f);
    }
    va_end(ap2);
    va_end(ap);
}

}  // namespace pc
