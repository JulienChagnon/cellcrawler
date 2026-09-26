#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace pc {

inline uint64_t nowUs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

inline void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[%10.3f] ", double(nowUs()) / 1e6);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    va_end(ap);
}

}  // namespace pc
