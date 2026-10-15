#include "video.h"

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "common.h"

namespace rover {

using namespace fpv;

namespace {

constexpr int32_t kColorFormatSurface = 0x7F000789;
constexpr int32_t kAvcProfileBaseline = 1;
constexpr int32_t kAvcLevel4 = 0x800;  // up to 1080p30 / 720p60
constexpr int32_t kBitrateModeCbr = 2;
constexpr int kFormatPrivate = 0x22;  // AIMAGE_FORMAT_PRIVATE
constexpr uint32_t kBufferFlagKeyFrame = 1;
constexpr uint32_t kBufferFlagCodecConfig = 2;
constexpr uint32_t kBufferFlagEos = 4;

bool startsWithSps(const uint8_t* d, size_t n) {
    size_t i = 0;
    if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) i = 4;
    else if (n >= 3 && d[0] == 0 && d[1] == 0 && d[2] == 1) i = 3;
    else return false;
    return i < n && (d[i] & 0x1F) == 7;
}

void onSessionNoop(void*, ACameraCaptureSession*) {}

struct CameraChoice {
    std::string id;
    bool back = false;
    bool color = false;   // a normal colour camera (not depth-only or monochrome)
    float fovDeg = 0;     // horizontal field of view at zoomMin
    float zoomMin = 1;    // < 1: a logical camera that reaches its ultra-wide by zooming out
    int maxFps = 0;
    int width = 0, height = 0;
    int recW = 0, recH = 0;  // phone recording: largest 16:9 output up to 1920x1080
    int32_t fpsLo = 0, fpsHi = 0;
    bool boottime = false;
    bool canFocus = false;
    std::string desc;
};

// Every camera id worth trying: the listed ones, then "0".."99", because some vendors leave
// the extra lenses off the list but still answer for them (the Mi A3's ultra-wide is "21").
std::vector<std::string> cameraIds(ACameraManager* mgr) {
    std::vector<std::string> out;
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(mgr, &ids) == ACAMERA_OK && ids) {
        for (int i = 0; i < ids->numCameras; ++i) out.emplace_back(ids->cameraIds[i]);
        ACameraManager_deleteCameraIdList(ids);
    }
    for (int i = 0; i < 100; ++i) {
        std::string id = std::to_string(i);
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

// Reads one camera's characteristics and picks the output size and fps range closest to
// what was asked for. False if the camera doesn't exist.
bool describeCamera(ACameraManager* mgr, const std::string& id, const VideoConfig& cfg, CameraChoice& out) {
    ACameraMetadata* ch = nullptr;
    if (ACameraManager_getCameraCharacteristics(mgr, id.c_str(), &ch) != ACAMERA_OK || !ch) return false;
    out = CameraChoice{};
    out.id = id;
    ACameraMetadata_const_entry e{};
    out.back = ACameraMetadata_getConstEntry(ch, ACAMERA_LENS_FACING, &e) == ACAMERA_OK &&
               e.data.u8[0] == ACAMERA_LENS_FACING_BACK;

    bool compatible = false, mono = false;
    if (ACameraMetadata_getConstEntry(ch, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &e) == ACAMERA_OK)
        for (uint32_t k = 0; k < e.count; ++k) {
            compatible |= e.data.u8[k] == ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE;
            mono |= e.data.u8[k] == ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MONOCHROME;
        }
    out.color = compatible && !mono;

    if (ACameraMetadata_getConstEntry(ch, ACAMERA_CONTROL_ZOOM_RATIO_RANGE, &e) == ACAMERA_OK && e.count >= 2 &&
        e.data.f[0] > 0.1f && e.data.f[0] < 1.0f)
        out.zoomMin = e.data.f[0];
    float focal = 0;
    if (ACameraMetadata_getConstEntry(ch, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &e) == ACAMERA_OK)
        for (uint32_t k = 0; k < e.count; ++k)
            if (e.data.f[k] > 0 && (focal == 0 || e.data.f[k] < focal)) focal = e.data.f[k];
    if (focal > 0 && ACameraMetadata_getConstEntry(ch, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE, &e) == ACAMERA_OK &&
        e.count >= 2)
        out.fovDeg = float(2 * std::atan(e.data.f[0] / (2 * focal * out.zoomMin)) * 180 / M_PI);

    // Output size: exact match, else the closest 16:9 size no wider than 1920.
    if (ACameraMetadata_getConstEntry(ch, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
        int bestDiff = 1 << 30;
        for (uint32_t k = 0; k + 3 < e.count; k += 4) {
            int32_t fmt = e.data.i32[k], w = e.data.i32[k + 1], h = e.data.i32[k + 2], input = e.data.i32[k + 3];
            if (fmt != kFormatPrivate || input) continue;
            if (w == cfg.width && h == cfg.height) {
                out.width = w;
                out.height = h;
                break;
            }
            if (w * 9 == h * 16 && w <= 1920 && std::abs(w - cfg.width) < bestDiff) {
                bestDiff = std::abs(w - cfg.width);
                out.width = w;
                out.height = h;
            }
        }
    }
    if (!out.width) {
        out.width = 1280;
        out.height = 720;
    }
    if (ACameraMetadata_getConstEntry(ch, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK)
        for (uint32_t k = 0; k + 3 < e.count; k += 4) {
            int32_t fmt = e.data.i32[k], w = e.data.i32[k + 1], h = e.data.i32[k + 2], input = e.data.i32[k + 3];
            if (fmt == kFormatPrivate && !input && w * 9 == h * 16 && w <= 1920 && w > out.recW) {
                out.recW = w;
                out.recH = h;
            }
        }
    if (!out.recW) {
        out.recW = out.width;
        out.recH = out.height;
    }

    // FPS: prefer a fixed [fps, fps] range so exposure can't stretch frame time.
    out.fpsLo = out.fpsHi = 30;
    if (ACameraMetadata_getConstEntry(ch, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &e) == ACAMERA_OK) {
        int32_t bLo = 0, bHi = 0;
        for (uint32_t k = 0; k + 1 < e.count; k += 2) {
            int32_t lo = e.data.i32[k], hi = e.data.i32[k + 1];
            out.maxFps = std::max(out.maxFps, int(hi));
            if (hi > cfg.fps) continue;
            if (hi > bHi || (hi == bHi && lo > bLo)) {
                bLo = lo;
                bHi = hi;
            }
        }
        if (bHi) {
            out.fpsLo = bLo;
            out.fpsHi = bHi;
        }
    }
    out.boottime = ACameraMetadata_getConstEntry(ch, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE, &e) == ACAMERA_OK &&
                   e.data.u8[0] == ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
    out.canFocus = ACameraMetadata_getConstEntry(ch, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &e) == ACAMERA_OK &&
                   e.count && e.data.f[0] > 0;
    ACameraMetadata_free(ch);

    std::ostringstream d;
    d << "cam " << id << " " << out.width << "x" << out.height << " @" << out.fpsLo << "-" << out.fpsHi;
    if (out.fovDeg > 0) d << ", " << int(std::lround(out.fovDeg)) << " deg";
    if (out.zoomMin < 1) d << " (zoom " << out.zoomMin << "x)";
    out.desc = d.str();
    return true;
}

// Cameras to try, best first. A camera id in the settings: just that one. Blank: every
// colour back camera, widest view first (the ultra-wide, if the phone lets apps use it).
std::vector<CameraChoice> chooseCameras(ACameraManager* mgr, const VideoConfig& cfg) {
    std::vector<CameraChoice> out;
    for (const std::string& id : cameraIds(mgr)) {
        CameraChoice c;
        if (!describeCamera(mgr, id, cfg, c)) continue;
        if (!cfg.cameraId.empty() ? id == cfg.cameraId : c.back && c.color) out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const CameraChoice& a, const CameraChoice& b) { return a.fovDeg > b.fovDeg; });
    return out;
}

}  // namespace

std::string VideoPipeline::listCameras() {
    ACameraManager* mgr = ACameraManager_create();
    VideoConfig cfg;
    const std::vector<CameraChoice> picked = chooseCameras(mgr, cfg);
    std::ostringstream s;
    for (const std::string& id : cameraIds(mgr)) {
        CameraChoice c;
        if (!describeCamera(mgr, id, cfg, c)) continue;
        s << id << ": " << (c.back ? "back" : "front/ext");
        if (c.fovDeg > 0) s << ", " << int(std::lround(c.fovDeg)) << " deg";
        if (c.zoomMin < 1) s << " (zoom " << c.zoomMin << "x)";
        s << ", max " << c.maxFps << "fps";
        if (!c.color) s << ", not a colour camera";
        if (!picked.empty() && picked.front().id == id) s << "  <- used when blank";
        s << "\n";
    }
    ACameraManager_delete(mgr);
    return s.str();
}

bool VideoPipeline::start(const VideoConfig& cfg, FrameSink sink) {
    stop();
    cfg_ = cfg;
    sink_ = std::move(sink);
    failed_ = false;
    curKbps_ = cfg.maxKbps;
    lastDecreaseUs_ = lastIncreaseUs_ = 0;
    csd_.clear();

    camMgr_ = ACameraManager_create();
    // Open the first camera that lets us: a vendor may describe its ultra-wide but refuse
    // to open it for apps other than its own.
    ACameraDevice_StateCallbacks devCb{this, onDeviceDisconnected, onDeviceError};
    CameraChoice cam;
    for (const CameraChoice& c : chooseCameras(camMgr_, cfg_)) {
        camera_status_t st = ACameraManager_openCamera(camMgr_, c.id.c_str(), &devCb, &camDev_);
        if (st == ACAMERA_OK) {
            cam = c;
            break;
        }
        LOGW("video: openCamera(%s) failed: %d, trying the next camera", c.id.c_str(), st);
        camDev_ = nullptr;
    }
    if (!camDev_) {
        LOGE("video: no camera could be opened (camera permission granted?)");
        stop();
        return false;
    }
    cfg_.width = cam.width;
    cfg_.height = cam.height;
    cfg_.fps = cam.fpsHi;  // the ultra-wide may top out below what was asked for
    timestampsBoottime_ = cam.boottime;
    camInfo_ = cam.desc;
    LOGI("video: %s, timestamps %s", cam.desc.c_str(), cam.boottime ? "boottime" : "monotonic");

    if (!openEncoder()) {
        stop();
        return false;
    }
    running_ = true;
    thread_ = std::thread(&VideoPipeline::outputLoop, this);

    // Camera into the encoder surface (and the recorder's, if one is set).
    fpsLo_ = cam.fpsLo;
    fpsHi_ = cam.fpsHi;
    zoomMin_ = cam.zoomMin;
    canFocus_ = cam.canFocus;
    recW_ = cam.recW;
    recH_ = cam.recH;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(camMu_);
        ok = startSessionLocked();
    }
    if (!ok) {
        stop();
        return false;
    }
    LOGI("video: streaming %dx%d, %d kbps", cfg_.width, cfg_.height, cfg_.maxKbps);
    return true;
}

bool VideoPipeline::openEncoder() {
    // First try with every low-latency option; some encoders reject unknown combos, so
    // fall back to a plain configuration.
    for (int attempt = 0; attempt < 2; ++attempt) {
        codec_ = AMediaCodec_createEncoderByType("video/avc");
        if (!codec_) {
            LOGE("video: no H.264 encoder");
            return false;
        }
        AMediaFormat* f = AMediaFormat_new();
        AMediaFormat_setString(f, "mime", "video/avc");
        AMediaFormat_setInt32(f, "width", cfg_.width);
        AMediaFormat_setInt32(f, "height", cfg_.height);
        AMediaFormat_setInt32(f, "color-format", kColorFormatSurface);
        AMediaFormat_setInt32(f, "bitrate", curKbps_ * 1000);
        AMediaFormat_setInt32(f, "bitrate-mode", kBitrateModeCbr);
        AMediaFormat_setInt32(f, "frame-rate", cfg_.fps);
        AMediaFormat_setInt32(f, "profile", kAvcProfileBaseline);  // no B-frames
        AMediaFormat_setInt32(f, "level", kAvcLevel4);  // a profile without a level is rejected
        AMediaFormat_setInt32(f, "i-frame-interval", attempt == 0 ? 10 : 1);
        if (attempt == 0) {
            // Refresh the picture a column at a time over half a second instead of periodic
            // IDR frames: constant frame sizes and self-healing after packet loss.
            AMediaFormat_setInt32(f, "intra-refresh-period", std::max(1, cfg_.fps / 2));
            AMediaFormat_setInt32(f, "priority", 0);  // realtime
            AMediaFormat_setInt32(f, "latency", 1);
            AMediaFormat_setInt32(f, "max-bframes", 0);
            AMediaFormat_setInt32(f, "prepend-sps-pps-to-idr-frames", 1);
            AMediaFormat_setInt32(f, "vendor.qti-ext-enc-low-latency.enable", 1);
        }
        media_status_t st = AMediaCodec_configure(codec_, f, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(f);
        if (st == AMEDIA_OK) {
            LOGI("video: encoder configured (%s)", attempt == 0 ? "low-latency" : "basic");
            break;
        }
        LOGW("video: encoder configure attempt %d failed: %d", attempt, st);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    if (!codec_) return false;
    if (AMediaCodec_createInputSurface(codec_, &window_) != AMEDIA_OK || !window_) {
        LOGE("video: createInputSurface failed");
        return false;
    }
    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        LOGE("video: encoder start failed");
        return false;
    }
    return true;
}

VideoPipeline::~VideoPipeline() {
    stop();
    if (recWindow_) ANativeWindow_release(recWindow_);
}

bool VideoPipeline::startSessionLocked() {
    ACaptureSessionOutputContainer_create(&outputs_);
    ACaptureSessionOutput_create(window_, &sessOut_);
    ACaptureSessionOutputContainer_add(outputs_, sessOut_);
    ACameraOutputTarget_create(window_, &target_);
    if (recWindow_) {
        ACaptureSessionOutput_create(recWindow_, &recOut_);
        ACaptureSessionOutputContainer_add(outputs_, recOut_);
        ACameraOutputTarget_create(recWindow_, &recTarget_);
    }
    ACameraDevice_createCaptureRequest(camDev_, TEMPLATE_RECORD, &request_);
    ACaptureRequest_addTarget(request_, target_);
    if (recTarget_ && recOn_) ACaptureRequest_addTarget(request_, recTarget_);

    int32_t range[2] = {fpsLo_, fpsHi_};
    ACaptureRequest_setEntry_i32(request_, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, range);
    if (zoomMin_ < 1) ACaptureRequest_setEntry_float(request_, ACAMERA_CONTROL_ZOOM_RATIO, 1, &zoomMin_);
    // Electronic stabilisation buffers frames: off. (Optical stabilisation is free, left on.)
    uint8_t v = ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
    ACaptureRequest_setEntry_u8(request_, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, 1, &v);
    v = ACAMERA_NOISE_REDUCTION_MODE_FAST;
    ACaptureRequest_setEntry_u8(request_, ACAMERA_NOISE_REDUCTION_MODE, 1, &v);
    v = ACAMERA_EDGE_MODE_FAST;
    ACaptureRequest_setEntry_u8(request_, ACAMERA_EDGE_MODE, 1, &v);
    if (canFocus_ && cfg_.focusInfinity) {
        v = ACAMERA_CONTROL_AF_MODE_OFF;
        ACaptureRequest_setEntry_u8(request_, ACAMERA_CONTROL_AF_MODE, 1, &v);
        float inf = 0.0f;
        ACaptureRequest_setEntry_float(request_, ACAMERA_LENS_FOCUS_DISTANCE, 1, &inf);
    } else if (canFocus_) {
        v = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
        ACaptureRequest_setEntry_u8(request_, ACAMERA_CONTROL_AF_MODE, 1, &v);
    }

    ACameraCaptureSession_stateCallbacks sessCb{this, onSessionNoop, onSessionNoop, onSessionNoop};
    camera_status_t st = ACameraDevice_createCaptureSession(camDev_, outputs_, &sessCb, &session_);
    if (st != ACAMERA_OK) {
        LOGE("video: createCaptureSession failed: %d", st);
        session_ = nullptr;
        return false;
    }
    st = ACameraCaptureSession_setRepeatingRequest(session_, nullptr, 1, &request_, nullptr);
    if (st != ACAMERA_OK) {
        LOGE("video: setRepeatingRequest failed: %d", st);
        return false;
    }
    return true;
}

void VideoPipeline::closeSessionLocked() {
    if (session_) {
        ACameraCaptureSession_stopRepeating(session_);
        ACameraCaptureSession_close(session_);
        session_ = nullptr;
    }
    if (request_) { ACaptureRequest_free(request_); request_ = nullptr; }
    if (target_) { ACameraOutputTarget_free(target_); target_ = nullptr; }
    if (recTarget_) { ACameraOutputTarget_free(recTarget_); recTarget_ = nullptr; }
    if (outputs_) { ACaptureSessionOutputContainer_free(outputs_); outputs_ = nullptr; }
    if (sessOut_) { ACaptureSessionOutput_free(sessOut_); sessOut_ = nullptr; }
    if (recOut_) { ACaptureSessionOutput_free(recOut_); recOut_ = nullptr; }
}

bool VideoPipeline::setRecordSurface(ANativeWindow* w) {
    std::lock_guard<std::mutex> lk(camMu_);
    if (w) ANativeWindow_acquire(w);
    recOn_ = false;
    const bool live = session_ != nullptr;
    if (live) closeSessionLocked();
    if (recWindow_) ANativeWindow_release(recWindow_);
    recWindow_ = w;
    LOGI("video: record output %s", w ? "added" : "removed");
    if (live && !startSessionLocked()) {
        failed_ = true;  // the engine restarts the pipeline (without the record output if it's gone)
        return false;
    }
    return true;
}

bool VideoPipeline::setRecording(bool on) {
    std::lock_guard<std::mutex> lk(camMu_);
    recOn_ = on && recWindow_;
    if (!session_ || !request_ || !recTarget_) return !on || recOn_;
    if (recOn_) ACaptureRequest_addTarget(request_, recTarget_);
    else ACaptureRequest_removeTarget(request_, recTarget_);
    return ACameraCaptureSession_setRepeatingRequest(session_, nullptr, 1, &request_, nullptr) == ACAMERA_OK;
}

void VideoPipeline::recordSize(int& w, int& h, int& fps) const {
    w = recW_;
    h = recH_;
    fps = fpsHi_;
}

void VideoPipeline::stop() {
    running_ = false;
    {
        std::lock_guard<std::mutex> lk(camMu_);
        closeSessionLocked();
        if (camDev_) {
            ACameraDevice_close(camDev_);
            camDev_ = nullptr;
        }
    }
    if (thread_.joinable()) thread_.join();
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    if (window_) { ANativeWindow_release(window_); window_ = nullptr; }
    if (camMgr_) { ACameraManager_delete(camMgr_); camMgr_ = nullptr; }
    fps_ = 0;
}

void VideoPipeline::onDeviceDisconnected(void* ctx, ACameraDevice*) {
    LOGE("video: camera disconnected");
    static_cast<VideoPipeline*>(ctx)->failed_ = true;
}

void VideoPipeline::onDeviceError(void* ctx, ACameraDevice*, int err) {
    LOGE("video: camera error %d", err);
    static_cast<VideoPipeline*>(ctx)->failed_ = true;
}

uint64_t VideoPipeline::toMonoUs(int64_t ptsUs) const {
    // Surface input keeps the camera sensor timestamp. Map it onto CLOCK_MONOTONIC; if the
    // declared timestamp source is wrong, pick whichever clock puts it in the recent past.
    const int64_t mono = int64_t(monoUs());
    const int64_t boot = int64_t(boottimeUs());
    const int64_t viaMono = ptsUs;
    const int64_t viaBoot = ptsUs - (boot - mono);
    auto plausible = [&](int64_t t) { return t <= mono && mono - t < 1000000; };
    int64_t first = timestampsBoottime_ ? viaBoot : viaMono;
    int64_t second = timestampsBoottime_ ? viaMono : viaBoot;
    if (plausible(first)) return uint64_t(first);
    if (plausible(second)) return uint64_t(second);
    return uint64_t(mono);
}

void VideoPipeline::outputLoop() {
    raiseThreadPriority(-16);
    while (running_) {
        AMediaCodecBufferInfo info{};
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 20000);
        if (idx < 0) continue;  // try again / format changed / buffers changed
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getOutputBuffer(codec_, size_t(idx), &cap);
        if (buf && info.size > 0) {
            const uint8_t* d = buf + info.offset;
            const size_t n = size_t(info.size);
            if (info.flags & kBufferFlagCodecConfig) {
                csd_.assign(d, d + n);
            } else {
                const bool key = (info.flags & kBufferFlagKeyFrame) != 0;
                const uint64_t captureUs = toMonoUs(info.presentationTimeUs);
                if (key && !csd_.empty() && !startsWithSps(d, n)) {
                    frameBuf_.assign(csd_.begin(), csd_.end());
                    frameBuf_.insert(frameBuf_.end(), d, d + n);
                    sink_(frameBuf_.data(), frameBuf_.size(), true, captureUs);
                } else {
                    sink_(d, n, key, captureUs);
                }
                ++fpsCount_;
                uint64_t now = monoUs();
                if (!fpsWindowUs_) fpsWindowUs_ = now;
                if (now - fpsWindowUs_ >= 1000000) {
                    fps_ = float(fpsCount_ * 1e6 / double(now - fpsWindowUs_));
                    fpsCount_ = 0;
                    fpsWindowUs_ = now;
                }
            }
        }
        AMediaCodec_releaseOutputBuffer(codec_, size_t(idx), false);
        if (info.flags & kBufferFlagEos) break;
    }
}

void VideoPipeline::setBitrate(int kbps) {
    if (!codec_) return;
    AMediaFormat* p = AMediaFormat_new();
    AMediaFormat_setInt32(p, "video-bitrate", kbps * 1000);
    AMediaCodec_setParameters(codec_, p);
    AMediaFormat_delete(p);
    curKbps_ = kbps;
}

void VideoPipeline::requestKeyframe() {
    if (!codec_ || !running_) return;
    AMediaFormat* p = AMediaFormat_new();
    AMediaFormat_setInt32(p, "request-sync", 0);
    AMediaCodec_setParameters(codec_, p);
    AMediaFormat_delete(p);
}

void VideoPipeline::onFeedback(const net::VideoFeedback& f) {
    if (!running_) return;
    if (f.requestKeyframe) requestKeyframe();

    // Delay-based rate control: back off hard when the network starts queueing (the
    // latency killer), creep back up slowly once it is clean.
    std::lock_guard<std::mutex> lk(abrMu_);
    const uint64_t now = monoUs();
    const float loss = f.lossPermille / 10.0f;
    const int q = f.queueDelayMs;
    const int cur = curKbps_;
    int next = cur;
    const bool canDecrease = now - lastDecreaseUs_ > 400000;
    if ((q > 80 || loss > 5) && canDecrease) {
        next = cur * 7 / 10;
        lastDecreaseUs_ = now;
    } else if ((q > 40 || loss > 2) && canDecrease) {
        next = cur * 85 / 100;
        lastDecreaseUs_ = now;
    } else if (q < 20 && loss < 1 && now - lastDecreaseUs_ > 3000000 && now - lastIncreaseUs_ > 500000) {
        next = cur * 105 / 100 + 50;
        lastIncreaseUs_ = now;
    }
    next = std::clamp(next, cfg_.minKbps, cfg_.maxKbps);
    if (next != cur) setBitrate(next);
}

std::string VideoPipeline::info() const { return camInfo_; }

}  // namespace rover
