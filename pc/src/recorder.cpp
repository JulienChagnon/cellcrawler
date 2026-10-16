#include "recorder.h"

#include <cstring>
#include <ctime>
#include <filesystem>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "util.h"

namespace pc {

namespace {
constexpr size_t kMaxQueued = 3;  // ~0.1 s at 30 fps; beyond that frames are dropped

AVCodecContext* openEncoder(int w, int h, bool globalHeader) {
    // Quality over latency here: this is a file, not the live link.
    const char* names[] = {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf", "libopenh264"};
    for (const char* name : names) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) continue;
        AVCodecContext* enc = avcodec_alloc_context3(codec);
        enc->width = w;
        enc->height = h;
        enc->time_base = {1, 1000};  // ms: frames arrive whenever the window redraws
        enc->framerate = {30, 1};
        enc->bit_rate = int64_t(w) * h * 30 / 8;  // ~8 Mbit/s at 720p
        enc->gop_size = 60;
        enc->max_b_frames = 0;
        enc->pix_fmt = AV_PIX_FMT_YUV420P;
        const void* fmts = nullptr;
        int nfmts = 0;
        if (avcodec_get_supported_config(enc, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &fmts, &nfmts) >= 0 && fmts) {
            const AVPixelFormat* f = static_cast<const AVPixelFormat*>(fmts);
            enc->pix_fmt = f[0];
            for (int i = 0; i < nfmts; ++i)
                if (f[i] == AV_PIX_FMT_NV12 || f[i] == AV_PIX_FMT_YUV420P) {
                    enc->pix_fmt = f[i];
                    break;
                }
        }
        if (globalHeader) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set(enc->priv_data, "preset", "p4", 0);
        av_opt_set(enc->priv_data, "hw_encoding", "1", 0);
        if (avcodec_open2(enc, codec, nullptr) == 0) {
            logf("record: encoder %s %dx%d", codec->name, w, h);
            return enc;
        }
        avcodec_free_context(&enc);
    }
    return nullptr;
}
}  // namespace

bool ScreenRecorder::start(const std::string& dir, int width, int height) {
    stop();
    width_ = width & ~1;
    height_ = height & ~1;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);

    const std::time_t now = std::time(nullptr);
    char name[64];
    std::strftime(name, sizeof name, "fpv-%Y%m%d-%H%M%S.mp4", std::localtime(&now));
    path_ = (std::filesystem::u8path(dir) / name).u8string();

    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "mp4", path_.c_str()) < 0 || !fmt) {
        logf("record: cannot create %s", path_.c_str());
        return false;
    }
    AVCodecContext* enc = openEncoder(width_, height_, (fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0);
    if (!enc) {
        logf("record: no usable H.264 encoder");
        avformat_free_context(fmt);
        return false;
    }
    AVStream* st = avformat_new_stream(fmt, nullptr);
    avcodec_parameters_from_context(st->codecpar, enc);
    st->time_base = enc->time_base;
    if (avio_open(&fmt->pb, path_.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(fmt, nullptr) < 0) {
        logf("record: cannot write %s", path_.c_str());
        if (fmt->pb) avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        avcodec_free_context(&enc);
        return false;
    }
    enc_ = enc;
    fmt_ = fmt;
    stream_ = st;
    firstUs_ = 0;
    lastPts_ = -1;
    dropped_ = 0;
    stopping_ = false;
    active_ = true;
    thread_ = std::thread(&ScreenRecorder::run, this);
    logf("record: PC recording to %s", path_.c_str());
    return true;
}

void ScreenRecorder::addFrame(const uint8_t* bgra, int pitch, int width, int height, uint64_t timeUs) {
    if (!active_) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.size() >= kMaxQueued) {
        ++dropped_;
        return;
    }
    Frame f;
    if (!spare_.empty()) {
        f.pixels.swap(spare_.back());
        spare_.pop_back();
    }
    f.pixels.resize(size_t(pitch) * height);
    std::memcpy(f.pixels.data(), bgra, f.pixels.size());
    f.pitch = pitch;
    f.width = width;
    f.height = height;
    f.timeUs = timeUs;
    queue_.push_back(std::move(f));
    cv_.notify_one();
}

void ScreenRecorder::stop() {
    if (!active_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
    active_ = false;
    logf("record: PC recording saved to %s%s", path_.c_str(),
         dropped_ ? (" (" + std::to_string(dropped_) + " frames skipped)").c_str() : "");
}

void ScreenRecorder::run() {
    auto* enc = static_cast<AVCodecContext*>(enc_);
    auto* fmt = static_cast<AVFormatContext*>(fmt_);
    auto* st = static_cast<AVStream*>(stream_);
    AVFrame* yuv = av_frame_alloc();
    yuv->format = enc->pix_fmt;
    yuv->width = enc->width;
    yuv->height = enc->height;
    av_frame_get_buffer(yuv, 0);
    AVPacket* pkt = av_packet_alloc();
    SwsContext* sws = nullptr;

    auto drain = [&] {
        while (avcodec_receive_packet(enc, pkt) == 0) {
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
        }
    };

    for (;;) {
        Frame f;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) break;  // stopping, all written
            f = std::move(queue_.front());
            queue_.pop_front();
        }
        sws = sws_getCachedContext(sws, f.width, f.height, AV_PIX_FMT_BGRA, enc->width, enc->height, enc->pix_fmt,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws && av_frame_make_writable(yuv) >= 0) {
            const uint8_t* src[1] = {f.pixels.data()};
            const int srcStride[1] = {f.pitch};
            sws_scale(sws, src, srcStride, 0, f.height, yuv->data, yuv->linesize);
            if (!firstUs_) firstUs_ = f.timeUs;
            int64_t pts = int64_t((f.timeUs - firstUs_) / 1000);
            if (pts <= lastPts_) pts = lastPts_ + 1;  // strictly increasing
            lastPts_ = pts;
            yuv->pts = pts;
            if (avcodec_send_frame(enc, yuv) >= 0) drain();
        }
        std::lock_guard<std::mutex> lk(mu_);
        spare_.push_back(std::move(f.pixels));
    }

    avcodec_send_frame(enc, nullptr);
    drain();
    av_write_trailer(fmt);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    avcodec_free_context(&enc);
    sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&yuv);
    enc_ = fmt_ = stream_ = nullptr;
}

}  // namespace pc
