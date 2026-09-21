#include "audio_player.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace raidcast {
namespace {

constexpr std::uint32_t kSrcRate     = 48000;
constexpr std::uint32_t kSrcChannels = 2;

// Enough to ride out jitter, small enough not to dominate the latency budget.
constexpr std::uint32_t kTargetMs = 40;
constexpr std::uint32_t kMaxMs    = 160;

std::string AvErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

}  // namespace

struct AudioPlayer::Impl {
    // decode
    AVCodecContext* ctx   = nullptr;
    AVPacket*       pkt   = nullptr;
    AVFrame*        frame = nullptr;
    SwrContext*     swr   = nullptr;

    // render
    winrt::com_ptr<IAudioClient>        client;
    winrt::com_ptr<IAudioRenderClient>  render;
    HANDLE                              ready = nullptr;
    HANDLE                              stop  = nullptr;
    std::thread                         thread;
    std::atomic<bool>                   running{false};

    std::uint32_t dst_rate     = 48000;
    std::uint32_t dst_channels = 2;

    // What the resampler is currently configured to accept. Set from the first
    // decoded frame rather than assumed: FFmpeg's native Opus decoder emits
    // planar float (fltp) while libopus emits interleaved (flt), and guessing
    // wrong produces output that measures as silence rather than failing.
    AVSampleFormat  swr_in_fmt  = AV_SAMPLE_FMT_NONE;
    int             swr_in_rate = 0;
    AVChannelLayout swr_in_layout{};

    mutable std::mutex mu;
    std::vector<float> queue;  // interleaved, device format
    std::atomic<std::uint64_t> underruns{0};
    std::atomic<int>           peak_milli{0};  // peak |sample| * 1000
    std::atomic<float>         gain{1.0f};
    std::atomic<int>           out_peak_milli{0};
    std::atomic<std::uint64_t> trimmed{0};

    std::size_t QueuedFrames() const { return queue.size() / dst_channels; }
};

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}
AudioPlayer::~AudioPlayer() { Close(); }

std::uint32_t AudioPlayer::queued_ms() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return static_cast<std::uint32_t>(impl_->QueuedFrames() * 1000 / impl_->dst_rate);
}
// (Re)builds the resampler when the decoder's actual output does not match what
// it was last configured for. Called per frame; the comparison is cheap and the
// rebuild happens once.
static bool EnsureResampler(AudioPlayer::Impl* impl, const AVFrame* frame,
                            std::string* error);

void AudioPlayer::SetGain(float gain) {
    impl_->gain.store(std::clamp(gain, 0.0f, 4.0f), std::memory_order_relaxed);
}

float AudioPlayer::TakeOutputPeakDbfs() {
    const int milli = impl_->out_peak_milli.exchange(0, std::memory_order_relaxed);
    if (milli <= 0) return -120.0f;
    return 20.0f * std::log10(static_cast<float>(milli) / 1000.0f);
}

float AudioPlayer::TakePeakDbfs() {
    const int milli = impl_->peak_milli.exchange(0, std::memory_order_relaxed);
    if (milli <= 0) return -120.0f;
    return 20.0f * std::log10(static_cast<float>(milli) / 1000.0f);
}

std::uint64_t AudioPlayer::underruns() const {
    return impl_->underruns.load(std::memory_order_relaxed);
}
std::uint64_t AudioPlayer::trimmed() const {
    return impl_->trimmed.load(std::memory_order_relaxed);
}

bool AudioPlayer::Open(std::string* error) {
    auto fail = [&](const std::string& what) {
        if (error) *error = what;
        Close();
        return false;
    };

    // --- decoder ------------------------------------------------------------
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) return fail("no Opus decoder in this FFmpeg build");

    impl_->ctx = avcodec_alloc_context3(codec);
    if (!impl_->ctx) return fail("avcodec_alloc_context3 failed");
    impl_->ctx->sample_rate = kSrcRate;
    av_channel_layout_default(&impl_->ctx->ch_layout, static_cast<int>(kSrcChannels));

    if (int err = avcodec_open2(impl_->ctx, codec, nullptr); err < 0)
        return fail("avcodec_open2(opus): " + AvErr(err));

    impl_->pkt   = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    if (!impl_->pkt || !impl_->frame) return fail("packet/frame allocation failed");

    // --- render endpoint ----------------------------------------------------
    winrt::com_ptr<IMMDeviceEnumerator> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), devices.put_void())))
        return fail("MMDeviceEnumerator unavailable");

    winrt::com_ptr<IMMDevice> device;
    if (FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, device.put())))
        return fail("no default audio output device");

    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                impl_->client.put_void())))
        return fail("IAudioClient activation failed");

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(impl_->client->GetMixFormat(&mix)) || mix == nullptr)
        return fail("GetMixFormat failed");

    impl_->dst_rate     = mix->nSamplesPerSec;
    impl_->dst_channels = mix->nChannels;

    constexpr REFERENCE_TIME kBuffer = 400'000;  // 40 ms
    const HRESULT init_hr = impl_->client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBuffer, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(init_hr)) return fail("IAudioClient::Initialize failed");

    impl_->ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->stop  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->ready || !impl_->stop) return fail("event creation failed");

    if (FAILED(impl_->client->SetEventHandle(impl_->ready)))
        return fail("SetEventHandle failed");
    if (FAILED(impl_->client->GetService(__uuidof(IAudioRenderClient),
                                         impl_->render.put_void())))
        return fail("GetService(IAudioRenderClient) failed");

    UINT32 buffer_frames = 0;
    impl_->client->GetBufferSize(&buffer_frames);
    if (FAILED(impl_->client->Start())) return fail("IAudioClient::Start failed");

    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([this, buffer_frames] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        HANDLE waits[] = {impl_->stop, impl_->ready};

        while (impl_->running.load(std::memory_order_acquire)) {
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (w == WAIT_OBJECT_0) break;
            if (w != WAIT_OBJECT_0 + 1) continue;

            UINT32 padding = 0;
            if (FAILED(impl_->client->GetCurrentPadding(&padding))) continue;
            const UINT32 want = buffer_frames - padding;
            if (want == 0) continue;

            BYTE* out = nullptr;
            if (FAILED(impl_->render->GetBuffer(want, &out))) continue;

            UINT32 written = 0;
            {
                std::lock_guard<std::mutex> lock(impl_->mu);
                const std::size_t have = impl_->QueuedFrames();
                written = static_cast<UINT32>(std::min<std::size_t>(have, want));
                if (written > 0) {
                    const std::size_t n = static_cast<std::size_t>(written) * impl_->dst_channels;
                    const float g  = impl_->gain.load(std::memory_order_relaxed);
                    auto*       dst = reinterpret_cast<float*>(out);
                    float out_peak = 0.0f;
                    for (std::size_t i = 0; i < n; ++i) {
                        // Clamp rather than let a boosted sample wrap into noise.
                        dst[i]   = std::clamp(impl_->queue[i] * g, -1.0f, 1.0f);
                        out_peak = std::max(out_peak, std::fabs(dst[i]));
                    }
                    const int om = static_cast<int>(out_peak * 1000.0f);
                    int prev_om = impl_->out_peak_milli.load(std::memory_order_relaxed);
                    while (om > prev_om && !impl_->out_peak_milli.compare_exchange_weak(
                                               prev_om, om, std::memory_order_relaxed)) {
                    }
                    impl_->queue.erase(impl_->queue.begin(),
                                       impl_->queue.begin() + static_cast<std::ptrdiff_t>(n));
                }
            }
            if (written < want) {
                // Starved: emit silence rather than repeating or stalling.
                std::fill_n(reinterpret_cast<float*>(out) +
                                static_cast<std::size_t>(written) * impl_->dst_channels,
                            static_cast<std::size_t>(want - written) * impl_->dst_channels, 0.0f);
                impl_->underruns.fetch_add(1, std::memory_order_relaxed);
            }
            impl_->render->ReleaseBuffer(want, 0);
        }
    });

    return true;
}

bool AudioPlayer::Push(const std::uint8_t* data, std::size_t len, std::string* error) {
    if (!impl_->ctx) {
        if (error) *error = "audio player not open";
        return false;
    }

    av_packet_unref(impl_->pkt);
    impl_->pkt->data = const_cast<std::uint8_t*>(data);
    impl_->pkt->size = static_cast<int>(len);

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

        if (const char* dbg = std::getenv("RAIDCAST_AUDIO_DEBUG"); dbg && *dbg) {
            static bool once = false;
            if (!once) {
                once = true;
                float p0 = 0.0f, p1 = 0.0f;
                const auto* c0 = reinterpret_cast<const float*>(impl_->frame->data[0]);
                const auto* c1 = reinterpret_cast<const float*>(impl_->frame->data[1]);
                for (int i = 0; i < impl_->frame->nb_samples; ++i) {
                    if (c0) p0 = std::max(p0, std::fabs(c0[i]));
                    if (c1) p1 = std::max(p1, std::fabs(c1[i]));
                }
                std::fprintf(stderr,
                             "[audio] decoder=%s fmt=%s ch=%d rate=%d nb=%d "
                             "linesize0=%d data1=%s plane0_peak=%.4f plane1_peak=%.4f\n",
                             impl_->ctx->codec->name,
                             av_get_sample_fmt_name(
                                 static_cast<AVSampleFormat>(impl_->frame->format)),
                             impl_->frame->ch_layout.nb_channels, impl_->frame->sample_rate,
                             impl_->frame->nb_samples, impl_->frame->linesize[0],
                             c1 ? "set" : "null", p0, p1);
            }
        }

        if (!EnsureResampler(impl_.get(), impl_->frame, error)) return false;

        const int max_out = swr_get_out_samples(impl_->swr, impl_->frame->nb_samples);
        std::vector<float> converted(static_cast<std::size_t>(max_out) * impl_->dst_channels);
        auto* out_ptr = reinterpret_cast<std::uint8_t*>(converted.data());

        const int got = swr_convert(impl_->swr, &out_ptr, max_out,
                                    const_cast<const std::uint8_t**>(impl_->frame->data),
                                    impl_->frame->nb_samples);
        if (got <= 0) continue;
        converted.resize(static_cast<std::size_t>(got) * impl_->dst_channels);

        float peak = 0.0f;
        for (const float v : converted) peak = std::max(peak, std::fabs(v));
        const int milli = static_cast<int>(peak * 1000.0f);
        int prev = impl_->peak_milli.load(std::memory_order_relaxed);
        while (milli > prev && !impl_->peak_milli.compare_exchange_weak(
                                   prev, milli, std::memory_order_relaxed)) {
        }

        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->queue.insert(impl_->queue.end(), converted.begin(), converted.end());

        // Clock drift over a long session shows up here as a queue that only
        // grows. Trim from the front: a discontinuity is less bad than an
        // audio delay that widens all night.
        const std::size_t max_frames =
            static_cast<std::size_t>(kMaxMs) * impl_->dst_rate / 1000;
        if (impl_->QueuedFrames() > max_frames) {
            const std::size_t keep =
                static_cast<std::size_t>(kTargetMs) * impl_->dst_rate / 1000;
            const std::size_t drop = (impl_->QueuedFrames() - keep) * impl_->dst_channels;
            impl_->queue.erase(impl_->queue.begin(),
                               impl_->queue.begin() + static_cast<std::ptrdiff_t>(drop));
            impl_->trimmed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return true;
}

static bool EnsureResampler(AudioPlayer::Impl* impl, const AVFrame* frame,
                            std::string* error) {
    const auto fmt  = static_cast<AVSampleFormat>(frame->format);
    const int  rate = frame->sample_rate;

    if (impl->swr && impl->swr_in_fmt == fmt && impl->swr_in_rate == rate &&
        av_channel_layout_compare(&impl->swr_in_layout, &frame->ch_layout) == 0)
        return true;

    if (impl->swr) swr_free(&impl->swr);

    AVChannelLayout dst_layout;
    av_channel_layout_default(&dst_layout, static_cast<int>(impl->dst_channels));

    if (swr_alloc_set_opts2(&impl->swr, &dst_layout, AV_SAMPLE_FMT_FLT,
                            static_cast<int>(impl->dst_rate), &frame->ch_layout, fmt, rate, 0,
                            nullptr) < 0 ||
        swr_init(impl->swr) < 0) {
        if (error) *error = "could not configure the audio resampler";
        return false;
    }

    impl->swr_in_fmt  = fmt;
    impl->swr_in_rate = rate;
    av_channel_layout_copy(&impl->swr_in_layout, &frame->ch_layout);
    return true;
}

void AudioPlayer::Close() {
    if (!impl_) return;

    impl_->running.store(false, std::memory_order_release);
    if (impl_->stop) SetEvent(impl_->stop);
    if (impl_->thread.joinable()) impl_->thread.join();

    if (impl_->client) impl_->client->Stop();
    impl_->render = nullptr;
    impl_->client = nullptr;

    if (impl_->ready) { CloseHandle(impl_->ready); impl_->ready = nullptr; }
    if (impl_->stop)  { CloseHandle(impl_->stop);  impl_->stop  = nullptr; }

    if (impl_->swr) swr_free(&impl_->swr);
    av_channel_layout_uninit(&impl_->swr_in_layout);
    if (impl_->frame) av_frame_free(&impl_->frame);
    if (impl_->pkt) av_packet_free(&impl_->pkt);
    if (impl_->ctx) avcodec_free_context(&impl_->ctx);
}

}  // namespace raidcast
