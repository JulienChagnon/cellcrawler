// Video path: chunk reassembly -> H.264 decode (D3D11VA or software) -> newest-frame slot.
//
// Latency rules: no jitter buffer, frames are decoded the moment their last chunk lands,
// incomplete frames are abandoned as soon as a newer frame completes, and only the newest
// decoded frame is ever shown.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "fpv/net_proto.h"

struct AVFrame;

namespace pc {

struct FrameTiming {
    uint64_t captureUs = 0;   // phone clock
    uint32_t phoneLatUs = 0;  // capture -> sent, measured on the phone
    uint64_t lastRecvUs = 0;  // PC clock, last chunk received
    uint64_t decodedUs = 0;   // PC clock
};

struct VideoStats {
    float fps = 0;
    float kbps = 0;
    float decodeMs = 0;
    uint32_t framesLost = 0;  // total incomplete/skipped frames
    uint32_t keyframeRequests = 0;
    bool hw = false;
    bool haveStream = false;
    uint16_t queueDelayMs = 0;
};

class VideoReceiver {
public:
    explicit VideoReceiver(bool hwDecode);
    ~VideoReceiver();

    void start();
    void stop();

    // Network thread.
    void onChunk(const fpv::net::VideoHeader& h, const uint8_t* data, size_t len, uint64_t recvUs);
    bool takeKeyframeRequest();
    fpv::net::VideoFeedback makeFeedback(uint64_t nowUs, float lossPct);

    // Render thread. Returns the newest decoded frame (caller frees with av_frame_free),
    // or nullptr if nothing new since the last call.
    AVFrame* takeFrame(FrameTiming& timing);
    VideoStats stats();

    // Invoked on the decoder thread whenever a new frame is ready.
    std::function<void()> onFrameReady;

private:
    struct Pending {
        uint32_t frameId = 0;
        uint16_t count = 0, got = 0;
        uint8_t flags = 0;
        uint64_t captureUs = 0;
        uint32_t phoneLatUs = 0;
        uint64_t lastRecvUs = 0;
        size_t size = 0;
        std::vector<bool> have;
        std::vector<uint8_t> data;
    };
    struct Assembled {
        std::vector<uint8_t> data;  // padded for the decoder
        size_t size = 0;
        bool keyframe = false;
        FrameTiming timing;
    };

    void decodeLoop();
    void requestKeyframe(uint64_t nowUs);
    void completeFrame(Pending& p);

    const bool wantHw_;

    // Reassembly (network thread only, except the stats below)
    std::vector<Pending> pending_;
    bool anyDelivered_ = false;
    uint32_t lastDelivered_ = 0;
    uint64_t lastKeyReqUs_ = 0;
    std::atomic<bool> keyReq_{false};
    bool waitingForKeyframe_ = true;
    // queue delay estimator: min of (recv - capture) over ~10 s, in two 5 s buckets
    int64_t minDelayCur_ = INT64_MAX, minDelayPrev_ = INT64_MAX;
    uint64_t bucketStartUs_ = 0;
    int64_t lastDelay_ = 0;
    uint16_t maxQueueDelayMs_ = 0;

    // Decoder hand-off
    std::mutex qMu_;
    std::condition_variable qCv_;
    std::deque<Assembled> queue_;
    std::thread thread_;
    bool running_ = false;

    // Output slot
    std::mutex outMu_;
    AVFrame* out_ = nullptr;
    FrameTiming outTiming_;

    // Stats
    std::mutex statMu_;
    VideoStats stats_;
    uint64_t statWindowUs_ = 0;
    uint32_t winFrames_ = 0, winBytes_ = 0;
    uint64_t intervalBytes_ = 0, intervalStartUs_ = 0;
};

}  // namespace pc
