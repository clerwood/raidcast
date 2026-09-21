#include "encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

#include <winrt/base.h>

#include <array>
#include <climits>

namespace raidcast {
namespace {

// HEVC first across all three vendors, then H.264 as the compatibility floor.
// Deliberately no software encoder — see the header.
// TODO: hevc_qsv does not accept AV_PIX_FMT_D3D11 frames; an Intel-only host
// would need a QSV hwframes pool and a d3d11->qsv derive. No Intel GPU on hand
// to develop that against, so it is listed but will decline to open.
constexpr std::array<const char*, 6> kCandidates = {
    "hevc_nvenc", "hevc_amf", "hevc_qsv",
    "h264_nvenc", "h264_amf", "h264_qsv",
};

std::string AvErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

}  // namespace

struct Encoder::Impl {
    AVBufferRef*    device_ref = nullptr;
    AVBufferRef*    frames_ref = nullptr;
    AVCodecContext* ctx        = nullptr;
    AVFrame*        frame      = nullptr;
    AVPacket*       pkt        = nullptr;
    std::string     name;

    ID3D11Device*        device  = nullptr;   // borrowed
    ID3D11DeviceContext* context = nullptr;   // borrowed
    winrt::com_ptr<ID3D11Texture2D> scratch;  // used only when !direct
    ID3D11Texture2D* pool_tex   = nullptr;    // current frame's pool texture
    std::uint32_t    pool_slice = 0;
    bool             direct     = true;
};

bool Encoder::direct_write() const { return impl_ && impl_->direct; }

Encoder::Encoder() : impl_(std::make_unique<Impl>()) {}
Encoder::~Encoder() { Close(); }

const char* Encoder::codec_name() const {
    return impl_ && !impl_->name.empty() ? impl_->name.c_str() : "(none)";
}

bool Encoder::Open(ID3D11Device* device, ID3D11DeviceContext* context,
                   const EncoderConfig& cfg, std::string* error) {
    auto fail = [&](const std::string& what) {
        if (error) *error = what;
        Close();
        return false;
    };

    if (device == nullptr || context == nullptr || cfg.width == 0 || cfg.height == 0)
        return fail("invalid encoder configuration");
    impl_->device  = device;
    impl_->context = context;

    // --- hardware device: hand libav the same D3D11 device the capture uses ---
    impl_->device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!impl_->device_ref) return fail("av_hwdevice_ctx_alloc failed");

    auto* dev_ctx = reinterpret_cast<AVHWDeviceContext*>(impl_->device_ref->data);
    auto* d3d_ctx = static_cast<AVD3D11VADeviceContext*>(dev_ctx->hwctx);
    device->AddRef();  // libav takes ownership of this reference
    d3d_ctx->device = device;

    if (int err = av_hwdevice_ctx_init(impl_->device_ref); err < 0)
        return fail("av_hwdevice_ctx_init: " + AvErr(err));

    // --- frame pool ---------------------------------------------------------
    // Ideally the conversion shader writes straight into the pool, which needs
    // UAV bind flags. Measured on NVIDIA 596.36 (RTX 4070 Ti): an NV12 texture
    // *array* is rejected with E_INVALIDARG for every bind-flag combination
    // except D3D11_BIND_DECODER, while ArraySize == 1 accepts all of them. Since
    // libav's pool is an array, direct write is not available there — so try the
    // ideal case for drivers that do allow it, then fall back to DECODER plus a
    // scratch texture the shader can write and EndFrame copies in.
    constexpr UINT kPoolSize = 8;
    const std::array<UINT, 2> kBindFlags = {
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
        D3D11_BIND_DECODER,
    };

    // Ask D3D11 directly which flags an array of this size accepts, rather than
    // letting libav try and fail — a failed av_hwframe_ctx_init logs an alarming
    // "Could not create the texture" that is in fact a normal probe result.
    UINT chosen = 0;
    bool found   = false;
    for (UINT flags : kBindFlags) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width      = cfg.width;
        td.Height     = cfg.height;
        td.MipLevels  = 1;
        td.ArraySize  = kPoolSize;
        td.Format     = DXGI_FORMAT_NV12;
        td.SampleDesc = {1, 0};
        td.Usage      = D3D11_USAGE_DEFAULT;
        td.BindFlags  = flags;

        winrt::com_ptr<ID3D11Texture2D> probe;
        if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, probe.put()))) {
            chosen = flags;
            found  = true;
            break;
        }
    }
    if (!found) return fail("no usable NV12 frame pool configuration");

    impl_->frames_ref = av_hwframe_ctx_alloc(impl_->device_ref);
    if (!impl_->frames_ref) return fail("av_hwframe_ctx_alloc failed");

    auto* fr_ctx = reinterpret_cast<AVHWFramesContext*>(impl_->frames_ref->data);
    fr_ctx->format            = AV_PIX_FMT_D3D11;
    fr_ctx->sw_format         = AV_PIX_FMT_NV12;
    fr_ctx->width             = static_cast<int>(cfg.width);
    fr_ctx->height            = static_cast<int>(cfg.height);
    fr_ctx->initial_pool_size = kPoolSize;
    static_cast<AVD3D11VAFramesContext*>(fr_ctx->hwctx)->BindFlags = chosen;

    if (int err = av_hwframe_ctx_init(impl_->frames_ref); err < 0)
        return fail("av_hwframe_ctx_init: " + AvErr(err));

    impl_->direct = (chosen & D3D11_BIND_UNORDERED_ACCESS) != 0;

    if (!impl_->direct) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width      = cfg.width;
        d.Height     = cfg.height;
        d.MipLevels  = 1;
        d.ArraySize  = 1;
        d.Format     = DXGI_FORMAT_NV12;
        d.SampleDesc = {1, 0};
        d.Usage      = D3D11_USAGE_DEFAULT;
        d.BindFlags  = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, impl_->scratch.put())))
            return fail("no UAV-capable NV12 texture available at all");
    }

    // --- encoder ------------------------------------------------------------
    // Try to *open* each candidate, not merely find it. An encoder can be present
    // in the build and still refuse to initialise — most commonly when the FFmpeg
    // build targets a newer NVENC API than the installed driver provides. Finding
    // it and assuming it works turns a recoverable "fall back to the next vendor"
    // into a hard failure.
    bool        opened = false;
    std::string attempts;

    for (const char* name : kCandidates) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) continue;

        if (impl_->ctx) avcodec_free_context(&impl_->ctx);
        impl_->ctx = avcodec_alloc_context3(codec);
        if (!impl_->ctx) return fail("avcodec_alloc_context3 failed");

        AVCodecContext* c = impl_->ctx;
        c->width        = static_cast<int>(cfg.width);
        c->height       = static_cast<int>(cfg.height);
        c->pix_fmt      = AV_PIX_FMT_D3D11;
        c->sw_pix_fmt   = AV_PIX_FMT_NV12;
        c->time_base    = AVRational{1, 1'000'000};  // microseconds
        c->framerate    = AVRational{static_cast<int>(cfg.fps), 1};
        c->max_b_frames = 0;
        c->gop_size     = static_cast<int>(cfg.fps * cfg.idr_interval_s);
        c->bit_rate     = cfg.bitrate_bps;
        c->rc_max_rate  = cfg.bitrate_bps;
        c->rc_min_rate  = cfg.bitrate_bps;

        // Two frames' worth keeps CBR honest without letting the encoder bank
        // bits it would later spend as a latency spike.
        c->rc_buffer_size = static_cast<int>(cfg.bitrate_bps / cfg.fps * 2);

        // Full-range BT.709, signalled in the VUI so the viewer reproduces it.
        c->color_range     = AVCOL_RANGE_JPEG;
        c->colorspace      = AVCOL_SPC_BT709;
        c->color_primaries = AVCOL_PRI_BT709;
        c->color_trc       = AVCOL_TRC_BT709;
        c->flags          |= AV_CODEC_FLAG_LOW_DELAY;

        c->hw_frames_ctx = av_buffer_ref(impl_->frames_ref);
        if (!c->hw_frames_ctx) return fail("av_buffer_ref failed");

        // Options must be per-vendor. A shared list does NOT work: "preset" exists
        // on all three encoders with disjoint value sets, so nvenc's "p1" is not
        // ignored by AMF and QSV — it is rejected as an invalid value.
        auto opt = [&](const char* k, const char* v) { av_opt_set(c->priv_data, k, v, 0); };
        const std::string n = name;
        if (n.find("nvenc") != std::string::npos) {
            opt("preset", "p1");          // fastest
            opt("tune", "ull");           // ultra low latency
            opt("rc", "cbr");
            opt("zerolatency", "1");
            opt("delay", "0");
            opt("b_ref_mode", "disabled");
            if (cfg.intra_refresh) {
                opt("intra-refresh", "1");
                opt("int_ref_type", "vertical");
            }
        } else if (n.find("amf") != std::string::npos) {
            opt("usage", "ultralowlatency");
            opt("quality", "speed");
            opt("rc", "cbr");
        } else if (n.find("qsv") != std::string::npos) {
            opt("preset", "veryfast");
            opt("async_depth", "1");
            opt("low_delay_brc", "1");
        }

        const int err = avcodec_open2(c, codec, nullptr);
        if (err >= 0) {
            impl_->name = name;
            opened      = true;
            break;
        }
        attempts += std::string(attempts.empty() ? "" : "; ") + name + ": " + AvErr(err);
    }

    if (!opened) return fail("no usable hardware encoder (" + attempts + ")");

    impl_->frame = av_frame_alloc();
    impl_->pkt   = av_packet_alloc();
    if (!impl_->frame || !impl_->pkt) return fail("frame/packet allocation failed");

    return true;
}

bool Encoder::BeginFrame(ID3D11Texture2D** tex, std::uint32_t* slice, std::string* error) {
    if (!impl_->ctx) {
        if (error) *error = "encoder not open";
        return false;
    }

    av_frame_unref(impl_->frame);
    if (int err = av_hwframe_get_buffer(impl_->frames_ref, impl_->frame, 0); err < 0) {
        if (error) *error = "av_hwframe_get_buffer: " + AvErr(err);
        return false;
    }

    // For AV_PIX_FMT_D3D11, data[0] is the texture and data[1] is the array index.
    impl_->pool_tex   = reinterpret_cast<ID3D11Texture2D*>(impl_->frame->data[0]);
    impl_->pool_slice =
        static_cast<std::uint32_t>(reinterpret_cast<std::intptr_t>(impl_->frame->data[1]));

    if (impl_->direct) {
        *tex   = impl_->pool_tex;
        *slice = impl_->pool_slice;
    } else {
        *tex   = impl_->scratch.get();
        *slice = 0;
    }
    return true;
}

bool Encoder::EndFrame(std::int64_t pts_us, const PacketSink& sink, std::string* error) {
    if (!impl_->ctx) {
        if (error) *error = "encoder not open";
        return false;
    }

    if (!impl_->direct && impl_->pool_tex) {
        impl_->context->CopySubresourceRegion(impl_->pool_tex, impl_->pool_slice, 0, 0, 0,
                                              impl_->scratch.get(), 0, nullptr);
    }

    impl_->frame->pts = pts_us;
    if (int err = avcodec_send_frame(impl_->ctx, impl_->frame); err < 0) {
        if (error) *error = "avcodec_send_frame: " + AvErr(err);
        return false;
    }

    for (;;) {
        const int err = avcodec_receive_packet(impl_->ctx, impl_->pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) break;
        if (err < 0) {
            if (error) *error = "avcodec_receive_packet: " + AvErr(err);
            return false;
        }
        if (sink) {
            EncodedPacket p;
            p.data     = impl_->pkt->data;
            p.size     = static_cast<std::size_t>(impl_->pkt->size);
            p.pts_us   = impl_->pkt->pts;
            p.keyframe = (impl_->pkt->flags & AV_PKT_FLAG_KEY) != 0;
            sink(p);
        }
        av_packet_unref(impl_->pkt);
    }
    return true;
}

void Encoder::Close() {
    if (!impl_) return;
    if (impl_->pkt) av_packet_free(&impl_->pkt);
    if (impl_->frame) av_frame_free(&impl_->frame);
    if (impl_->ctx) avcodec_free_context(&impl_->ctx);
    if (impl_->frames_ref) av_buffer_unref(&impl_->frames_ref);
    if (impl_->device_ref) av_buffer_unref(&impl_->device_ref);
}

}  // namespace raidcast
