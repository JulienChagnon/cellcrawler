// Records what the fpv-pc window shows (video + HUD) to an MP4. The render thread only
// copies each finished frame into a queue; scaling, encoding and writing happen on the
// recorder's own thread, and frames are dropped rather than ever making the display wait.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pc {

class ScreenRecorder {
public:
    ~ScreenRecorder() { stop(); }

    // Starts a new file in dir (created if missing), frames scaled to width x height.
    // False (and logged) if no encoder or the file can't be created.
    bool start(const std::string& dir, int width, int height);
    // Queues one BGRA frame (any size: it's scaled). Never blocks.
    void addFrame(const uint8_t* bgra, int pitch, int width, int height, uint64_t timeUs);
    // Finishes the file. Blocks until what's queued is written.
    void stop();

    bool active() const { return active_; }
    const std::string& path() const { return path_; }

private:
    struct Frame {
        std::vector<uint8_t> pixels;
        int pitch = 0, width = 0, height = 0;
        uint64_t timeUs = 0;
    };
    void run();

    std::atomic<bool> active_{false};
    std::string path_;
    int width_ = 0, height_ = 0;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Frame> queue_;
    std::vector<std::vector<uint8_t>> spare_;  // recycled pixel buffers
    bool stopping_ = false;
    std::thread thread_;

    // Encoder state, recorder thread only (opaque FFmpeg pointers).
    void* enc_ = nullptr;
    void* fmt_ = nullptr;
    void* stream_ = nullptr;
    uint64_t firstUs_ = 0;
    int64_t lastPts_ = -1;
    uint32_t dropped_ = 0;
};

}  // namespace pc
