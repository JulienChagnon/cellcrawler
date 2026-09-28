#pragma once
#include <android/log.h>
#include <time.h>

#include <cstdint>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "rover", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "rover", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "rover", __VA_ARGS__)

namespace rover {

// The phone's reference clock for everything sent to the PC.
inline uint64_t monoUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ULL + uint64_t(ts.tv_nsec) / 1000ULL;
}

inline uint64_t boottimeUs() {
    timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return uint64_t(ts.tv_sec) * 1000000ULL + uint64_t(ts.tv_nsec) / 1000ULL;
}

// Best effort: nudge the calling thread's priority up (ignored if not permitted).
void raiseThreadPriority(int nice);

}  // namespace rover
