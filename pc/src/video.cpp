#include "video.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include "util.h"

namespace pc {

using namespace fpv;

namespace {
constexpr size_t kMaxPending = 4;          // frames being reassembled at once
constexpr size_t kMaxQueue = 6;            // decoder backlog before we flush and resync
constexpr uint64_t kKeyReqIntervalUs = 300000;
constexpr uint64_t kDelayBucketUs = 5000000;

AVPixelFormat pickFormat(AVCodecContext*, const AVPixelFormat* fmts) {
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_D3D11) return *p;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}
}  // namespace

VideoReceiver::VideoReceiver(bool hwDecode) : wantHw_(hwDecode) {}

VideoReceiver::~VideoReceiver() {
    stop();
    if (out_) av_frame_free(&out_);
}

void VideoReceiver::start() {
    running_ = true;
    thread_ = std::thread(&VideoReceiver::decodeLoop, this);
}

void VideoReceiver::stop() {
    {
        std::lock_guard<std::mutex> lk(qMu_);
        if (!running_) return;
        running_ = false;
    }
    qCv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void VideoReceiver::onChunk(const net::VideoHeader& h, const uint8_t* data, size_t len, uint64_t recvUs) {
    if (anyDelivered_ && int32_t(h.frameId - lastDelivered_) <= 0) return;  // older than what we showed
    if (len > net::kMaxVideoChunk) return;
    if (h.chunkIdx + 1u < h.chunkCount && len != net::kMaxVideoChunk) return;  // only the last chunk is short
    intervalBytes_ += len;

    auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.frameId == h.frameId; });
    if (it == pending_.end()) {
        if (pending_.size() >= kMaxPending) {
            auto oldest = std::min_element(pending_.begin(), pending_.end(), [](const Pending& a, const Pending& b) {
                return int32_t(a.frameId - b.frameId) < 0;
            });
            pending_.erase(oldest);
        }
        Pending p;
        p.frameId = h.frameId;
        p.count = h.chunkCount;
        p.flags = h.flags;
        p.captureUs = h.captureUs;
        p.phoneLatUs = h.phoneLatUs;
        p.have.assign(h.chunkCount, false);
        p.data.resize(size_t(h.chunkCount) * net::kMaxVideoChunk + AV_INPUT_BUFFER_PADDING_SIZE);
        pending_.push_back(std::move(p));
        it = pending_.end() - 1;
    }
    Pending& p = *it;
    if (p.count != h.chunkCount || p.have[h.chunkIdx]) return;
    p.have[h.chunkIdx] = true;
    ++p.got;
    std::memcpy(p.data.data() + size_t(h.chunkIdx) * net::kMaxVideoChunk, data, len);
    if (h.chunkIdx + 1u == h.chunkCount) p.size = size_t(h.chunkIdx) * net::kMaxVideoChunk + len;
    p.lastRecvUs = recvUs;

    if (p.got == p.count) {
        uint32_t id = p.frameId;
        completeFrame(p);
        // Anything older than a completed frame can never be shown: abandon it.
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [&](const Pending& q) { return int32_t(q.frameId - id) <= 0; }),
                       pending_.end());
    }
}

void VideoReceiver::requestKeyframe(uint64_t nowUs) {
    if (nowUs - lastKeyReqUs_ < kKeyReqIntervalUs) return;
    lastKeyReqUs_ = nowUs;
    keyReq_ = true;
    std::lock_guard<std::mutex> lk(statMu_);
    ++stats_.keyframeRequests;
}

bool VideoReceiver::takeKeyframeRequest() { return keyReq_.exchange(false); }

void VideoReceiver::completeFrame(Pending& p) {
    const uint64_t now = p.lastRecvUs;
    const uint32_t gap = anyDelivered_ ? p.frameId - lastDelivered_ - 1 : 0;
    lastDelivered_ = p.frameId;
    anyDelivered_ = true;
    const bool key = (p.flags & net::kVidKeyframe) != 0;

    if (gap > 0) {
        std::lock_guard<std::mutex> lk(statMu_);
        stats_.framesLost += gap;
    }
    // A lost frame breaks the reference chain. Intra-refresh heals it within ~0.5 s anyway,
    // but a keyframe heals it immediately.
    if (gap > 0) requestKeyframe(now);
    if (waitingForKeyframe_ && !key) {
        requestKeyframe(now);
        return;
    }
    if (key) waitingForKeyframe_ = false;

    // One-way delay above the recent minimum = time spent queued in the network.
    int64_t d = int64_t(p.lastRecvUs) - int64_t(p.captureUs);
    if (now - bucketStartUs_ > kDelayBucketUs) {
        minDelayPrev_ = minDelayCur_;
        minDelayCur_ = INT64_MAX;
        bucketStartUs_ = now;
    }
    minDelayCur_ = std::min(minDelayCur_, d);
    int64_t q = (d - std::min(minDelayCur_, minDelayPrev_)) / 1000;
    maxQueueDelayMs_ = std::max<uint16_t>(maxQueueDelayMs_, uint16_t(std::min<int64_t>(q, 65535)));

    Assembled a;
    a.size = p.size;
    a.keyframe = key;
    a.data = std::move(p.data);
    std::memset(a.data.data() + a.size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    a.timing.captureUs = p.captureUs;
    a.timing.phoneLatUs = p.phoneLatUs;
    a.timing.lastRecvUs = p.lastRecvUs;
    {
        std::lock_guard<std::mutex> lk(qMu_);
        if (queue_.size() >= kMaxQueue) {
            // Decoder can't keep up: skip ahead and resync on a keyframe.
            queue_.clear();
            waitingForKeyframe_ = !key;
        }
        if (!waitingForKeyframe_) queue_.push_back(std::move(a));
    }
    qCv_.notify_one();

    std::lock_guard<std::mutex> lk(statMu_);
    stats_.haveStream = true;
    stats_.queueDelayMs = uint16_t(std::min<int64_t>(q < 0 ? 0 : q, 65535));
    ++winFrames_;
    winBytes_ += uint32_t(p.size);
    if (statWindowUs_ == 0) statWindowUs_ = now;
    if (now - statWindowUs_ >= 1000000) {
        double s = double(now - statWindowUs_) / 1e6;
        stats_.fps = float(winFrames_ / s);
        stats_.kbps = float(winBytes_ * 8.0 / 1000.0 / s);
        winFrames_ = winBytes_ = 0;
        statWindowUs_ = now;
    }
}

net::VideoFeedback VideoReceiver::makeFeedback(uint64_t nowUs, float lossPct) {
    net::VideoFeedback f;
    f.lossPermille = uint16_t(std::min(1000.0f, lossPct * 10.0f));
    f.queueDelayMs = maxQueueDelayMs_;
    maxQueueDelayMs_ = 0;
    uint64_t elapsed = intervalStartUs_ ? nowUs - intervalStartUs_ : 0;
    f.recvKbps = elapsed ? uint32_t(intervalBytes_ * 8000 / elapsed) : 0;
    intervalBytes_ = 0;
    intervalStartUs_ = nowUs;
    return f;
}

AVFrame* VideoReceiver::takeFrame(FrameTiming& timing) {
    std::lock_guard<std::mutex> lk(outMu_);
    AVFrame* f = out_;
    out_ = nullptr;
    timing = outTiming_;
    return f;
}

VideoStats VideoReceiver::stats() {
    std::lock_guard<std::mutex> lk(statMu_);
    return stats_;
}

void VideoReceiver::decodeLoop() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;  // output every frame immediately
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->thread_count = 1;  // frame threading would add a frame of delay per thread

    AVBufferRef* hwDev = nullptr;
    bool hw = false;
    if (wantHw_ && av_hwdevice_ctx_create(&hwDev, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) == 0) {
        ctx->hw_device_ctx = av_buffer_ref(hwDev);
        ctx->get_format = pickFormat;
        // The phone signals plain Baseline, which FFmpeg won't hand to the GPU by default;
        // its stream uses no Baseline-only tools, so the GPU decodes it fine.
        ctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
        hw = true;
    }
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        logf("video: cannot open H.264 decoder");
        avcodec_free_context(&ctx);
        av_buffer_unref(&hwDev);
        return;
    }
    logf("video: decoder ready (%s)", hw ? "D3D11VA" : "software");
    {
        std::lock_guard<std::mutex> lk(statMu_);
        stats_.hw = hw;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    for (;;) {
        Assembled a;
        {
            std::unique_lock<std::mutex> lk(qMu_);
            qCv_.wait(lk, [&] { return !running_ || !queue_.empty(); });
            if (!running_) break;
            a = std::move(queue_.front());
            queue_.pop_front();
        }
        const uint64_t t0 = nowUs();
        pkt->data = a.data.data();
        pkt->size = int(a.size);
        pkt->flags = a.keyframe ? AV_PKT_FLAG_KEY : 0;
        if (avcodec_send_packet(ctx, pkt) < 0) {
            keyReq_ = true;
            continue;
        }
        while (avcodec_receive_frame(ctx, frame) == 0) {
            AVFrame* out = av_frame_alloc();
            if (frame->format == AV_PIX_FMT_D3D11) {
                // Copy to system memory as NV12 for SDL. ~1 ms at 720p.
                if (av_hwframe_transfer_data(out, frame, 0) < 0) {
                    av_frame_free(&out);
                    av_frame_unref(frame);
                    continue;
                }
                av_frame_copy_props(out, frame);
            } else {
                av_frame_move_ref(out, frame);
            }
            av_frame_unref(frame);

            const uint64_t t1 = nowUs();
            {
                std::lock_guard<std::mutex> lk(statMu_);
                stats_.decodeMs = stats_.decodeMs * 0.9f + float(t1 - t0) / 1000.0f * 0.1f;
            }
            {
                std::lock_guard<std::mutex> lk(outMu_);
                if (out_) av_frame_free(&out_);  // renderer didn't take the previous one: drop it
                out_ = out;
                outTiming_ = a.timing;
                outTiming_.decodedUs = t1;
            }
            if (onFrameReady) onFrameReady();
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    av_buffer_unref(&hwDev);
}

}  // namespace pc
