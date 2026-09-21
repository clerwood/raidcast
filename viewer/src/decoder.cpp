#include "decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
}

#include <cstdio>

namespace raidcast {
namespace {

std::string AvErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

AVPixelFormat PickFormat(AVCodecContext*, const AVPixelFormat* fmts) {
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_D3D11) return *p;
    return fmts[0];  // software fallback rather than failing outright
}

}  // namespace

struct Decoder::Impl {
    AVBufferRef*    device_ref = nullptr;
    AVCodecContext* ctx        = nullptr;
    AVPacket*       pkt        = nullptr;
    AVFrame*        frame      = nullptr;
    AVFrame*        sw_frame   = nullptr;
};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() { Close(); }

bool Decoder::Open(ID3D11Device* device, bool hevc, std::string* error) {
    auto fail = [&](const std::string& what) {
        if (error) *error = what;
        Close();
        return false;
    };

    const AVCodec* codec =
        avcodec_find_decoder(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (!codec) return fail("no HEVC/H.264 decoder in this build");

    impl_->device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!impl_->device_ref) return fail("av_hwdevice_ctx_alloc failed");

    auto* dev_ctx = reinterpret_cast<AVHWDeviceContext*>(impl_->device_ref->data);
    auto* d3d     = static_cast<AVD3D11VADeviceContext*>(dev_ctx->hwctx);
    device->AddRef();  // libav takes ownership
    d3d->device = device;

    if (int err = av_hwdevice_ctx_init(impl_->device_ref); err < 0)
        return fail("av_hwdevice_ctx_init: " + AvErr(err));

    impl_->ctx = avcodec_alloc_context3(codec);
    if (!impl_->ctx) return fail("avcodec_alloc_context3 failed");

    impl_->ctx->hw_device_ctx = av_buffer_ref(impl_->device_ref);
    impl_->ctx->get_format    = PickFormat;
    impl_->ctx->thread_count  = 1;   // low latency beats throughput here
    impl_->ctx->flags        |= AV_CODEC_FLAG_LOW_DELAY;

    if (int err = avcodec_open2(impl_->ctx, codec, nullptr); err < 0)
        return fail("avcodec_open2: " + AvErr(err));

    impl_->pkt      = av_packet_alloc();
    impl_->frame    = av_frame_alloc();
    impl_->sw_frame = av_frame_alloc();
    if (!impl_->pkt || !impl_->frame || !impl_->sw_frame)
        return fail("packet/frame allocation failed");

    return true;
}

bool Decoder::Decode(const std::uint8_t* data, std::size_t len, std::int64_t pts_us,
                     const FrameSink& sink, std::string* error) {
    if (!impl_->ctx) {
        if (error) *error = "decoder not open";
        return false;
    }

    av_packet_unref(impl_->pkt);
    impl_->pkt->data = const_cast<std::uint8_t*>(data);
    impl_->pkt->size = static_cast<int>(len);
    impl_->pkt->pts  = pts_us;

    if (int err = avcodec_send_packet(impl_->ctx, impl_->pkt); err < 0) {
        if (error) *error = "avcodec_send_packet: " + AvErr(err);
        return false;
    }

    for (;;) {
        const int err = avcodec_receive_frame(impl_->ctx, impl_->frame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) break;
        if (err < 0) {
            if (error) *error = "avcodec_receive_frame: " + AvErr(err);
            return false;
        }
        if (sink) {
            DecodedFrame f;
            f.width  = static_cast<std::uint32_t>(impl_->frame->width);
            f.height = static_cast<std::uint32_t>(impl_->frame->height);
            f.pts_us = impl_->frame->pts;
            if (impl_->frame->format == AV_PIX_FMT_D3D11) {
                f.texture = reinterpret_cast<ID3D11Texture2D*>(impl_->frame->data[0]);
                f.slice   = static_cast<std::uint32_t>(
                    reinterpret_cast<std::intptr_t>(impl_->frame->data[1]));
            }
            sink(f);
        }
    }
    return true;
}

bool Decoder::DumpLastNv12(const char* path, std::string* error) {
    auto fail = [&](const std::string& what) {
        if (error) *error = what;
        return false;
    };
    if (!impl_->frame || impl_->frame->width == 0) return fail("no frame decoded yet");

    AVFrame* src = impl_->frame;
    if (src->format == AV_PIX_FMT_D3D11) {
        av_frame_unref(impl_->sw_frame);
        impl_->sw_frame->format = AV_PIX_FMT_NV12;
        if (int err = av_hwframe_transfer_data(impl_->sw_frame, src, 0); err < 0)
            return fail("av_hwframe_transfer_data: " + AvErr(err));
        src = impl_->sw_frame;
    }

    FILE* f = std::fopen(path, "wb");
    if (!f) return fail("cannot open output file");

    for (int y = 0; y < src->height; ++y)
        std::fwrite(src->data[0] + static_cast<std::ptrdiff_t>(y) * src->linesize[0], 1,
                    static_cast<std::size_t>(src->width), f);
    for (int y = 0; y < src->height / 2; ++y)
        std::fwrite(src->data[1] + static_cast<std::ptrdiff_t>(y) * src->linesize[1], 1,
                    static_cast<std::size_t>(src->width), f);

    std::fclose(f);
    return true;
}

void Decoder::Close() {
    if (!impl_) return;
    if (impl_->sw_frame) av_frame_free(&impl_->sw_frame);
    if (impl_->frame) av_frame_free(&impl_->frame);
    if (impl_->pkt) av_packet_free(&impl_->pkt);
    if (impl_->ctx) avcodec_free_context(&impl_->ctx);
    if (impl_->device_ref) av_buffer_unref(&impl_->device_ref);
}

}  // namespace raidcast
