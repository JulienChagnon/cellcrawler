// Camera -> hardware H.264 encoder, zero-copy: the NDK camera renders straight into the
// encoder's input Surface. Encoded frames are handed to a sink (the network layer).
//
// Latency settings: no B-frames, CBR, intra-refresh instead of periodic IDR (no bitrate
// spikes), realtime priority / low-latency hints, video stabilisation off, fixed focus.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fpv/net_proto.h"

struct ACameraManager;
struct ACameraDevice;
struct ACameraCaptureSession;
struct ACaptureRequest;
struct ACaptureSessionOutputContainer;
struct ACaptureSessionOutput;
struct ACameraOutputTarget;
struct AMediaCodec;
struct ANativeWindow;

namespace rover {

struct VideoConfig {
    int width = 1280;
    int height = 720;
    int fps = 60;
    int maxKbps = 4000;
    int minKbps = 500;
    std::string cameraId;  // empty = widest back camera the phone lets apps open
    bool focusInfinity = true;
};

class VideoPipeline {
public:
    // data is Annex-B H.264 for one frame (SPS/PPS prepended on keyframes).
    using FrameSink = std::function<void(const uint8_t* data, size_t len, bool keyframe, uint64_t captureMonoUs)>;

    ~VideoPipeline();
    bool start(const VideoConfig& cfg, FrameSink sink);
    void stop();

    // Phone-side recording: a second camera output into the recorder's Surface (a Java
    // MediaRecorder). Setting or clearing the surface rebuilds the capture session (a short
    // gap in the live video); recording on/off only changes the repeating request. Both
    // survive a camera restart. The surface's size must come from recordSize().
    bool setRecordSurface(ANativeWindow* w);  // takes its own reference; nullptr removes it
    bool setRecording(bool on);
    void recordSize(int& w, int& h, int& fps) const;
    bool running() const { return running_; }
    bool failed() const { return failed_; }

    void onFeedback(const fpv::net::VideoFeedback& f);  // bitrate adaptation + keyframe requests
    void requestKeyframe();

    uint32_t bitrate() const { return curKbps_ * 1000; }
    float fps() const { return fps_; }
    std::string info() const;

    // "id: facing, focal length, sizes" for every camera, for the settings screen.
    static std::string listCameras();

private:
    bool startSessionLocked();  // camMu_ held
    void closeSessionLocked();
    bool openEncoder();
    bool openCamera();
    void outputLoop();
    void setBitrate(int kbps);
    uint64_t toMonoUs(int64_t ptsUs) const;

    static void onDeviceDisconnected(void* ctx, ACameraDevice* dev);
    static void onDeviceError(void* ctx, ACameraDevice* dev, int err);

    VideoConfig cfg_;
    FrameSink sink_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::thread thread_;

    AMediaCodec* codec_ = nullptr;
    ANativeWindow* window_ = nullptr;
    ACameraManager* camMgr_ = nullptr;
    ACameraDevice* camDev_ = nullptr;
    ACaptureSessionOutputContainer* outputs_ = nullptr;
    ACaptureSessionOutput* sessOut_ = nullptr;
    ACameraOutputTarget* target_ = nullptr;
    ACaptureRequest* request_ = nullptr;
    ACameraCaptureSession* session_ = nullptr;
    bool timestampsBoottime_ = false;
    std::string camInfo_;

    // Capture session state, rebuilt when the record surface changes (camMu_).
    std::mutex camMu_;
    int32_t fpsLo_ = 30, fpsHi_ = 30;
    float zoomMin_ = 1;
    bool canFocus_ = false;
    ANativeWindow* recWindow_ = nullptr;
    ACaptureSessionOutput* recOut_ = nullptr;
    ACameraOutputTarget* recTarget_ = nullptr;
    bool recOn_ = false;
    int recW_ = 0, recH_ = 0;

    std::vector<uint8_t> csd_;  // SPS/PPS
    std::vector<uint8_t> frameBuf_;

    // Adaptive bitrate
    std::mutex abrMu_;
    std::atomic<int> curKbps_{0};
    uint64_t lastDecreaseUs_ = 0, lastIncreaseUs_ = 0;

    // Stats
    std::atomic<float> fps_{0};
    uint64_t fpsWindowUs_ = 0;
    uint32_t fpsCount_ = 0;
};

}  // namespace rover
