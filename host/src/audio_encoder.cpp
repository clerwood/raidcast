#include "audio_encoder.h"

#include "audio_loopback.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <vector>

namespace raidcast {
namespace {

std::string AvErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

}  // namespace

struct AudioEncoder::Impl {
    AVCodecContext*    ctx   = nullptr;
    AVFrame*           frame = nullptr;
    AVPacket*          pkt   = nullptr;
    std::vector<float> pending;      // interleaved, awaiting a full Opus frame
    std::int64_t       pending_pts = 0;
    bool               have_pts    = false;
};

AudioEncoder::AudioEncoder() : impl_(std::make_unique<Impl>()) {}
AudioEncoder::~AudioEncoder() { Close(); }

bool AudioEncoder::Open(std::uint32_t bitrate_bps, std::string* error) {
    auto fail = [&](const std::string& what) {
        if (error) *error = what;
        Close();
        return false;
    };

    const AVCodec* codec = avcodec_find_encoder_by_name("libopus");
    if (!codec) return fail("libopus encoder not present in this FFmpeg build");

    impl_->ctx = avcodec_alloc_context3(codec);
    if (!impl_->ctx) return fail("avcodec_alloc_context3 failed");

    AVCodecContext* c = impl_->ctx;
    c->sample_rate = kAudioRate;
    c->sample_fmt  = AV_SAMPLE_FMT_FLT;   // matches WASAPI's interleaved float
    c->bit_rate    = bitrate_bps;
    c->time_base   = AVRational{1, 1'000'000};
    av_channel_layout_default(&c->ch_layout, static_cast<int>(kAudioChannels));

    // 10 ms rather than the 20 ms default: audio is the viewer's master clock,
    // so its buffering shows up directly in the latency budget.
    av_opt_set(c->priv_data, "frame_duration", "10", 0);
    av_opt_set(c->priv_data, "application", "audio", 0);

    if (int err = avcodec_open2(c, codec, nullptr); err < 0)
        return fail("avcodec_open2(libopus): " + AvErr(err));

    impl_->frame = av_frame_alloc();
    impl_->pkt   = av_packet_alloc();
    if (!impl_->frame || !impl_->pkt) return fail("frame/packet allocation failed");

    impl_->frame->format      = c->sample_fmt;
    impl_->frame->sample_rate = c->sample_rate;
    impl_->frame->nb_samples  = c->frame_size;
    av_channel_layout_copy(&impl_->frame->ch_layout, &c->ch_layout);
    if (int err = av_frame_get_buffer(impl_->frame, 0); err < 0)
        return fail("av_frame_get_buffer: " + AvErr(err));

    return true;
}

bool AudioEncoder::Submit(const float* interleaved, std::uint32_t frames, std::int64_t pts_us,
                          const PacketSink& sink, std::string* error) {
    if (!impl_->ctx) {
        if (error) *error = "audio encoder not open";
        return false;
    }

    const std::size_t block = static_cast<std::size_t>(impl_->ctx->frame_size) * kAudioChannels;

    if (!impl_->have_pts) {
        impl_->pending_pts = pts_us;
        impl_->have_pts    = true;
    }
    impl_->pending.insert(impl_->pending.end(), interleaved,
                          interleaved + static_cast<std::size_t>(frames) * kAudioChannels);

    while (impl_->pending.size() >= block) {
        if (int err = av_frame_make_writable(impl_->frame); err < 0) {
            if (error) *error = "av_frame_make_writable: " + AvErr(err);
            return false;
        }
        std::copy(impl_->pending.begin(), impl_->pending.begin() + block,
                  reinterpret_cast<float*>(impl_->frame->data[0]));
        impl_->pending.erase(impl_->pending.begin(),
                             impl_->pending.begin() + static_cast<std::ptrdiff_t>(block));

        impl_->frame->pts = impl_->pending_pts;
        impl_->pending_pts += impl_->ctx->frame_size * 1'000'000LL / kAudioRate;

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
                EncodedAudio a;
                a.data   = impl_->pkt->data;
                a.size   = static_cast<std::size_t>(impl_->pkt->size);
                a.pts_us = impl_->pkt->pts;
                sink(a);
            }
            av_packet_unref(impl_->pkt);
        }
    }
    return true;
}

void AudioEncoder::Close() {
    if (!impl_) return;
    if (impl_->pkt) av_packet_free(&impl_->pkt);
    if (impl_->frame) av_frame_free(&impl_->frame);
    if (impl_->ctx) avcodec_free_context(&impl_->ctx);
    impl_->pending.clear();
    impl_->have_pts = false;
}

}  // namespace raidcast
