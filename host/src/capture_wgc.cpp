#include "capture_wgc.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <dwmapi.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace winrt_ns  = winrt;
namespace wgc       = winrt::Windows::Graphics::Capture;
namespace wgdx      = winrt::Windows::Graphics::DirectX;
namespace wgdx11    = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace raidcast {
namespace {

std::wstring ExeNameOf(std::uint32_t pid) {
    winrt::handle proc{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!proc) return {};

    wchar_t  path[MAX_PATH] = {};
    DWORD    len            = MAX_PATH;
    if (!QueryFullProcessImageNameW(proc.get(), 0, path, &len)) return {};

    std::wstring full(path, len);
    const auto slash = full.find_last_of(L'\\');
    return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

bool IEqual(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return towlower(x) == towlower(y);
           });
}

bool IsCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
           cloaked;
}

struct EnumCtx {
    const std::wstring*      filter;
    std::vector<WindowInfo>* out;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lparam);

    if (!IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;
    if (IsCloaked(hwnd)) return TRUE;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    const LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return TRUE;

    WindowInfo info;
    info.hwnd   = hwnd;
    info.width  = static_cast<std::uint32_t>(w);
    info.height = static_cast<std::uint32_t>(h);
    GetWindowThreadProcessId(hwnd, reinterpret_cast<DWORD*>(&info.pid));
    info.exe = ExeNameOf(info.pid);

    if (!ctx->filter->empty() && !IEqual(info.exe, *ctx->filter)) return TRUE;

    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);
    info.title = title;

    ctx->out->push_back(std::move(info));
    return TRUE;
}

wgdx11::IDirect3DDevice WrapDevice(ID3D11Device* device) {
    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::check_hresult(device->QueryInterface(winrt::guid_of<IDXGIDevice>(), dxgi.put_void()));

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(
        CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    return inspectable.as<wgdx11::IDirect3DDevice>();
}

winrt::com_ptr<ID3D11Texture2D> TextureOf(const wgdx11::IDirect3DSurface& surface) {
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void()));
    return tex;
}

std::int64_t Now100ns() {
    static const std::int64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    // Split the conversion: at the usual 10 MHz QPC frequency, t * 10'000'000
    // overflows int64 after roughly 25 hours of uptime, which silently poisons
    // every latency measurement on any machine that has been on for a day.
    return (t.QuadPart / freq) * 10'000'000 + (t.QuadPart % freq) * 10'000'000 / freq;
}

}  // namespace

std::vector<WindowInfo> EnumerateCaptureCandidates(const std::wstring& exe_filter) {
    std::vector<WindowInfo> out;
    EnumCtx ctx{&exe_filter, &out};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

struct WindowCapture::Impl {
    wgc::GraphicsCaptureItem            item{nullptr};
    wgc::Direct3D11CaptureFramePool     pool{nullptr};
    wgc::GraphicsCaptureSession         session{nullptr};
    wgdx11::IDirect3DDevice             device{nullptr};

    winrt::event_token frame_token{};
    winrt::event_token closed_token{};

    FrameCallback         on_frame;
    std::atomic<bool>     closed{false};
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> discarded{0};
    winrt::Windows::Graphics::SizeInt32 last_size{};

    // Kept so Restart() can rebuild the session against the same window. The
    // HWND is fixed for the object's lifetime — see the header.
    HWND                         target{nullptr};
    winrt::com_ptr<ID3D11Device> d3d;
};

WindowCapture::WindowCapture() : impl_(std::make_unique<Impl>()) {}

WindowCapture::~WindowCapture() { Stop(); }

bool WindowCapture::Start(HWND target,
                          ID3D11Device* device,
                          FrameCallback on_frame,
                          std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };

    if (target == nullptr || !IsWindow(target)) return fail("target window does not exist");
    if (device == nullptr) return fail("null D3D11 device");

    try {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                                     ::IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForWindow(
            target, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));

        impl_->item     = item;
        impl_->device   = WrapDevice(device);
        impl_->on_frame = std::move(on_frame);
        impl_->last_size = item.Size();
        impl_->target   = target;
        impl_->d3d.copy_from(device);

        // Free-threaded: frames arrive on a WGC worker thread and we do not
        // need a DispatcherQueue on the calling thread.
        impl_->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl_->device,
            wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            impl_->last_size);

        impl_->session = impl_->pool.CreateCaptureSession(item);

        // Win11 only, and not fatal if the runtime refuses it.
        try {
            impl_->session.IsBorderRequired(false);
        } catch (const winrt::hresult_error&) {
        }

        impl_->frame_token = impl_->pool.FrameArrived(
            [this](const wgc::Direct3D11CaptureFramePool& pool, const winrt::Windows::Foundation::IInspectable&) {
                // C++/WinRT turns an escaping exception into a failed HRESULT
                // that WGC discards, so without this a single throw here is a
                // capture that silently stops: no error, no Closed event, no
                // frames. Catch it where it can still be counted.
                try {
                    auto frame = pool.TryGetNextFrame();
                    if (!frame) return;

                    const auto size = frame.ContentSize();
                    if (size.Width != impl_->last_size.Width ||
                        size.Height != impl_->last_size.Height) {
                        impl_->discarded.fetch_add(1, std::memory_order_relaxed);

                        // A minimized window reports a degenerate size.
                        // Recreating the pool at 0x0 throws; recreating it at
                        // 1x1 wrecks the pool for when the window comes back.
                        // Neither is worth doing — wait for a real size.
                        if (size.Width <= 0 || size.Height <= 0) return;

                        // The frame belongs to the pool being replaced, so it
                        // has to go first. Recreating underneath a live frame
                        // is what wedges the pool into never raising
                        // FrameArrived again.
                        frame.Close();

                        pool.Recreate(impl_->device,
                                      wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                      2, size);
                        // Committed only now: if Recreate threw, last_size must
                        // still describe the pool that actually exists, or the
                        // comparison above never fires again.
                        impl_->last_size = size;
                        return;  // this frame's texture was the old size
                    }

                    auto tex = TextureOf(frame.Surface());

                    CaptureFrame cf;
                    cf.texture     = tex.get();
                    cf.width       = static_cast<std::uint32_t>(size.Width);
                    cf.height      = static_cast<std::uint32_t>(size.Height);
                    cf.content_time_100ns = frame.SystemRelativeTime().count();
                    cf.arrived_time_100ns = Now100ns();

                    impl_->frames.fetch_add(1, std::memory_order_relaxed);
                    if (impl_->on_frame) impl_->on_frame(cf);
                } catch (const winrt::hresult_error&) {
                    impl_->discarded.fetch_add(1, std::memory_order_relaxed);
                }
            });

        // FAIL CLOSED: the window went away. We do not rebind to anything.
        impl_->closed_token = item.Closed(
            [this](const wgc::GraphicsCaptureItem&, const winrt::Windows::Foundation::IInspectable&) {
                impl_->closed.store(true, std::memory_order_release);
            });

        impl_->session.StartCapture();
        return true;
    } catch (const winrt::hresult_error& e) {
        if (error) *error = winrt::to_string(e.message());
        return false;
    }
}

void WindowCapture::Stop() {
    if (!impl_) return;
    try {
        if (impl_->pool && impl_->frame_token) impl_->pool.FrameArrived(impl_->frame_token);
        if (impl_->item && impl_->closed_token) impl_->item.Closed(impl_->closed_token);
        if (impl_->session) impl_->session.Close();
        if (impl_->pool) impl_->pool.Close();
    } catch (const winrt::hresult_error&) {
    }
    impl_->session = nullptr;
    impl_->pool    = nullptr;
    impl_->item    = nullptr;
}

bool WindowCapture::Restart(std::string* error) {
    if (!impl_) {
        if (error) *error = "capture was never started";
        return false;
    }
    // FAIL CLOSED, still: a destroyed window is terminal, and Restart() is not
    // a way around that.
    if (impl_->closed.load(std::memory_order_acquire)) {
        if (error) *error = "the target window has closed";
        return false;
    }
    if (impl_->target == nullptr || !IsWindow(impl_->target)) {
        if (error) *error = "the target window no longer exists";
        return false;
    }

    HWND          target = impl_->target;
    auto          device = impl_->d3d;
    FrameCallback cb     = impl_->on_frame;

    Stop();
    return Start(target, device.get(), std::move(cb), error);
}

bool WindowCapture::closed() const {
    return impl_ && impl_->closed.load(std::memory_order_acquire);
}

std::uint64_t WindowCapture::frames() const {
    return impl_ ? impl_->frames.load(std::memory_order_relaxed) : 0;
}

std::uint64_t WindowCapture::frames_discarded() const {
    return impl_ ? impl_->discarded.load(std::memory_order_relaxed) : 0;
}

}  // namespace raidcast
