#include "audio_loopback.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <winrt/base.h>

#include <atomic>
#include <cstdio>
#include <thread>

namespace raidcast {
namespace {

std::int64_t Now100ns() {
    static const std::int64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    // Split to avoid overflowing int64 on a long-uptime machine.
    return (t.QuadPart / freq) * 10'000'000 + (t.QuadPart % freq) * 10'000'000 / freq;
}

// ActivateAudioInterfaceAsync is asynchronous even though we have nothing to do
// while it runs, so this exists purely to signal an event.
//
// IAgileObject is not optional: from an MTA thread, ActivateAudioInterfaceAsync
// fails the whole call with E_ILLEGAL_METHOD_CALL (0x8000000E) if the handler
// cannot be marshalled freely.
class ActivationHandler : public IActivateAudioInterfaceCompletionHandler,
                          public IAgileObject {
public:
    explicit ActivationHandler(HANDLE done) : done_(done) {}

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done_);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

private:
    HANDLE             done_;
    std::atomic<ULONG> refs_{1};
};

}  // namespace

struct AudioLoopback::Impl {
    winrt::com_ptr<IAudioClient>        client;
    winrt::com_ptr<IAudioCaptureClient> capture;
    HANDLE                              buffer_ready = nullptr;
    HANDLE                              stop         = nullptr;
    std::thread                         thread;
    ChunkCallback                       on_chunk;
    std::atomic<std::uint64_t>          frames{0};
    std::atomic<bool>                   running{false};
};

AudioLoopback::AudioLoopback() : impl_(std::make_unique<Impl>()) {}
AudioLoopback::~AudioLoopback() { Stop(); }

std::uint64_t AudioLoopback::frames_captured() const {
    return impl_ ? impl_->frames.load(std::memory_order_relaxed) : 0;
}

bool AudioLoopback::Start(std::uint32_t pid, ChunkCallback on_chunk, std::string* error) {
    char detail[256] = {};
    auto fail = [&](const char* what) {
        if (error) *error = detail[0] ? std::string(what) + " " + detail : what;
        Stop();
        return false;
    };
    auto note = [&](HRESULT hr) {
        std::snprintf(detail, sizeof(detail), "(hr=0x%08lX)", static_cast<unsigned long>(hr));
    };

    impl_->on_chunk = std::move(on_chunk);

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType                        = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv{};
    pv.vt             = VT_BLOB;
    pv.blob.cbSize    = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    winrt::handle done{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!done) return fail("CreateEvent failed");

    auto* handler = new ActivationHandler(done.get());
    winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                             __uuidof(IAudioClient), &pv, handler, op.put());
    handler->Release();
    if (FAILED(hr)) {
        note(hr);
        return fail("ActivateAudioInterfaceAsync failed");
    }

    if (WaitForSingleObject(done.get(), 3000) != WAIT_OBJECT_0)
        return fail("timed out activating process loopback");

    HRESULT activate_hr = S_OK;
    winrt::com_ptr<IUnknown> unknown;
    if (FAILED(op->GetActivateResult(&activate_hr, unknown.put())) || FAILED(activate_hr)) {
        note(activate_hr);
        return fail("process loopback refused");
    }

    impl_->client = unknown.as<IAudioClient>();

    // The virtual loopback device has no mix format to query; we state one. Opus
    // is natively 48 kHz, so choosing it here means nothing ever resamples.
    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels       = static_cast<WORD>(kAudioChannels);
    fmt.nSamplesPerSec  = kAudioRate;
    fmt.wBitsPerSample  = 32;
    fmt.nBlockAlign     = static_cast<WORD>(kAudioChannels * 4);
    fmt.nAvgBytesPerSec = kAudioRate * fmt.nBlockAlign;

    constexpr REFERENCE_TIME kBuffer = 200'000;  // 20 ms
    if (const HRESULT hr_init = impl_->client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBuffer, 0,
            &fmt, nullptr);
        FAILED(hr_init)) {
        note(hr_init);
        return fail("IAudioClient::Initialize failed");
    }

    impl_->buffer_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->stop         = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->buffer_ready || !impl_->stop) return fail("event creation failed");

    if (FAILED(impl_->client->SetEventHandle(impl_->buffer_ready)))
        return fail("SetEventHandle failed");
    if (FAILED(impl_->client->GetService(__uuidof(IAudioCaptureClient),
                                         impl_->capture.put_void())))
        return fail("GetService(IAudioCaptureClient) failed");
    if (FAILED(impl_->client->Start())) return fail("IAudioClient::Start failed");

    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([this] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        HANDLE waits[] = {impl_->stop, impl_->buffer_ready};

        while (impl_->running.load(std::memory_order_acquire)) {
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (w == WAIT_OBJECT_0) break;          // stop
            if (w != WAIT_OBJECT_0 + 1) continue;   // timeout or error

            // The exit check belongs inside this loop too: if the callback
            // (encode + send) falls behind, WASAPI always has another buffer
            // ready and we would never return to the outer wait — making Stop()
            // hang forever on the join.
            while (impl_->running.load(std::memory_order_acquire)) {
                BYTE*  data    = nullptr;
                UINT32 frames  = 0;
                DWORD  flags   = 0;
                UINT64 pos     = 0, qpc = 0;

                const HRESULT hr =
                    impl_->capture->GetBuffer(&data, &frames, &flags, &pos, &qpc);
                if (hr != S_OK || frames == 0) break;

                AudioChunk chunk;
                chunk.samples   = reinterpret_cast<const float*>(data);
                chunk.frames    = frames;
                chunk.silent    = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                // WASAPI reports a device position; use our own clock so audio
                // and video share one timebase with no correlation step.
                chunk.qpc_100ns = Now100ns();

                impl_->frames.fetch_add(frames, std::memory_order_relaxed);
                if (impl_->on_chunk) impl_->on_chunk(chunk);

                impl_->capture->ReleaseBuffer(frames);
            }
        }
    });

    return true;
}

void AudioLoopback::Stop() {
    if (!impl_) return;

    impl_->running.store(false, std::memory_order_release);
    if (impl_->stop) SetEvent(impl_->stop);
    if (impl_->thread.joinable()) impl_->thread.join();

    if (impl_->client) impl_->client->Stop();
    impl_->capture = nullptr;
    impl_->client  = nullptr;

    if (impl_->buffer_ready) {
        CloseHandle(impl_->buffer_ready);
        impl_->buffer_ready = nullptr;
    }
    if (impl_->stop) {
        CloseHandle(impl_->stop);
        impl_->stop = nullptr;
    }
}

}  // namespace raidcast
