// capture-probe — M1 spike.
//
// Answers the one risk RaidCast accepted rather than designed out (DESIGN.md D3):
// can Windows Graphics Capture feed us a WoW window fast enough, and with low
// enough delivery latency, to build the rest of the pipeline on?
//
// Reports capture rate, frame interval percentiles, and compositor-to-delivery
// latency. Also samples the pixels, because the classic WGC failure mode is a
// perfectly healthy stream of entirely black frames.
//
//   capture-probe --list
//   capture-probe --process Wow.exe --seconds 20
//   capture-probe --hwnd 0x00010A3C --no-preview

#include "capture_wgc.h"
#include "convert_nv12.h"

#include <winrt/base.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace raidcast;

namespace {

struct Options {
    bool         list       = false;
    bool         preview    = true;
    int          seconds    = 15;
    int          delay      = 0;  // grace period to focus the target first
    std::string  dump;        // write one converted NV12 frame here, then continue
    std::wstring process    = L"Wow.exe";
    HWND         hwnd       = nullptr;
};

std::wstring Widen(const char* s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

Options ParseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") o.list = true;
        else if (a == "--no-preview") o.preview = false;
        else if (a == "--seconds" && i + 1 < argc) o.seconds = std::atoi(argv[++i]);
        else if (a == "--delay" && i + 1 < argc) o.delay = std::atoi(argv[++i]);
        else if (a == "--dump-nv12" && i + 1 < argc) o.dump = argv[++i];
        else if (a == "--process" && i + 1 < argc) o.process = Widen(argv[++i]);
        else if (a == "--hwnd" && i + 1 < argc)
            o.hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(
                std::strtoull(argv[++i], nullptr, 0)));
    }
    return o;
}

double Percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    auto idx = static_cast<std::size_t>(p * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

// ---------------------------------------------------------------------------

struct Shared {
    std::mutex                         mu;   // guards the immediate context too
    winrt::com_ptr<ID3D11Texture2D>    latest;
    winrt::com_ptr<ID3D11Texture2D>    staging;
    std::vector<double>                interval_ms;
    std::vector<double>                latency_ms;
    std::int64_t                       prev_content = 0;
    std::uint32_t                      w = 0, h = 0;
    std::uint64_t                      resized = 0;
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE || msg == WM_DESTROY ||
        (msg == WM_KEYDOWN && wp == VK_ESCAPE)) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = ParseArgs(argc, argv);
    // Window titles are UTF-8 narrow strings and the stats header is not ASCII;
    // without this the console renders both as mojibake.
    SetConsoleOutputCP(CP_UTF8);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // --- pick a target ------------------------------------------------------
    HWND target = opt.hwnd;
    if (opt.list || target == nullptr) {
        auto cands = EnumerateCaptureCandidates(opt.list ? L"" : opt.process);
        if (opt.list) {
            std::printf("%-12s %-8s %-11s %s\n", "HWND", "PID", "SIZE", "EXE / TITLE");
            for (const auto& c : cands) {
                std::printf("0x%08llX   %-8u %5ux%-5u %s  |  %s\n",
                            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(c.hwnd)),
                            c.pid, c.width, c.height,
                            Narrow(c.exe).c_str(), Narrow(c.title).c_str());
            }
            return 0;
        }
        if (cands.empty()) {
            std::fprintf(stderr,
                         "No visible window found for %s.\n"
                         "Run with --list to see candidates, or --process NAME.\n",
                         Narrow(opt.process).c_str());
            return 1;
        }
        // Largest client area wins — WoW's main window, not a tooltip.
        target = std::max_element(cands.begin(), cands.end(),
                                  [](const WindowInfo& a, const WindowInfo& b) {
                                      return a.width * a.height < b.width * b.height;
                                  })->hwnd;
    }

    // --- D3D11 --------------------------------------------------------------
    winrt::com_ptr<ID3D11Device>        device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL                   fl{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, device.put(), &fl, context.put()))) {
        std::fprintf(stderr, "D3D11CreateDevice failed\n");
        return 1;
    }

    Shared sh;

    // --- preview window -----------------------------------------------------
    HWND                             preview_hwnd = nullptr;
    winrt::com_ptr<IDXGISwapChain1>  swap;
    RECT src{};
    GetClientRect(target, &src);
    const UINT cap_w = std::max<LONG>(src.right - src.left, 1);
    const UINT cap_h = std::max<LONG>(src.bottom - src.top, 1);

    if (opt.preview) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"RaidCastCaptureProbe";
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);

        const double scale = std::min(1.0, std::min(1280.0 / cap_w, 720.0 / cap_h));
        RECT want{0, 0, static_cast<LONG>(cap_w * scale), static_cast<LONG>(cap_h * scale)};
        AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);

        preview_hwnd = CreateWindowExW(0, wc.lpszClassName, L"RaidCast capture probe",
                                       WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                       want.right - want.left, want.bottom - want.top,
                                       nullptr, nullptr, wc.hInstance, nullptr);
        ShowWindow(preview_hwnd, SW_SHOW);

        auto dxgi_dev = device.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter> adapter;
        dxgi_dev->GetAdapter(adapter.put());
        winrt::com_ptr<IDXGIFactory2> factory;
        adapter->GetParent(winrt::guid_of<IDXGIFactory2>(), factory.put_void());

        // Backbuffer matches the capture so CopyResource is legal; DXGI stretches
        // it down to the window.
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width       = cap_w;
        scd.Height      = cap_h;
        scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc  = {1, 0};
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.Scaling     = DXGI_SCALING_STRETCH;
        scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        factory->CreateSwapChainForHwnd(device.get(), preview_hwnd, &scd, nullptr, nullptr,
                                        swap.put());
    }

    // --- start capture ------------------------------------------------------
    WindowCapture capture;
    std::string   err;

    auto on_frame = [&](const CaptureFrame& f) {
        std::lock_guard<std::mutex> lock(sh.mu);

        if (sh.w != f.width || sh.h != f.height) {
            sh.w = f.width;
            sh.h = f.height;
            sh.latest = nullptr;
            ++sh.resized;
        }
        if (!sh.latest) {
            D3D11_TEXTURE2D_DESC d{};
            f.texture->GetDesc(&d);
            d.Usage          = D3D11_USAGE_DEFAULT;
            d.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = 0;
            d.MiscFlags      = 0;
            device->CreateTexture2D(&d, nullptr, sh.latest.put());
        }
        if (sh.latest) context->CopyResource(sh.latest.get(), f.texture);

        if (sh.prev_content != 0) {
            sh.interval_ms.push_back((f.content_time_100ns - sh.prev_content) / 10000.0);
        }
        sh.prev_content = f.content_time_100ns;
        sh.latency_ms.push_back((f.arrived_time_100ns - f.content_time_100ns) / 10000.0);
    };

    // WGC only delivers frames the game actually presents, and WoW honours its
    // Max Background FPS when it is not foreground. Measuring the foreground rate
    // therefore means handing focus back to WoW before sampling starts.
    if (opt.delay > 0) {
        std::printf("Focus the target window now — starting in");
        for (int i = opt.delay; i > 0; --i) {
            std::printf(" %d", i);
            std::fflush(stdout);
            Sleep(1000);
        }
        std::printf(" go\n");
    }

    if (!capture.Start(target, device.get(), on_frame, &err)) {
        std::fprintf(stderr, "capture failed: %s\n", err.c_str());
        return 1;
    }

    std::printf("Capturing HWND 0x%llX  %ux%u  for %ds%s\n\n",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(target)),
                cap_w, cap_h, opt.seconds, opt.preview ? "  (Esc closes preview)" : "");
    // NOTE on the Δts columns: they are (arrived - SystemRelativeTime). They come
    // out consistently NEGATIVE by ~5 ms, which means SystemRelativeTime is not
    // simply "when the frame was produced" in QPC-now's epoch — most likely it is
    // a target present time. Treat the value as a stability indicator, not as
    // capture latency. Real glass-to-glass needs an on-screen clock and a camera.
    std::printf("%-6s %6s %8s %8s %8s %8s %8s %7s\n",
                "t", "fps", "int p50", "int p99", "int max", "Δts p50", "Δts p99", "pixels");

    // --- run ----------------------------------------------------------------
    const auto start = std::chrono::steady_clock::now();
    auto       next  = start + std::chrono::seconds(1);
    bool       quit  = false;
    bool       dumped = false;

    while (!quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (capture.closed()) {
            std::printf("\ntarget window closed — capture is terminal (fail closed)\n");
            break;
        }

        {
            std::lock_guard<std::mutex> lock(sh.mu);

            if (!opt.dump.empty() && !dumped && sh.latest && sh.w && sh.h) {
                std::string cerr_msg;
                Bgra2Nv12 conv;
                if (conv.Init(device.get(), sh.w, sh.h, &cerr_msg) &&
                    conv.Convert(context.get(), sh.latest.get()) &&
                    conv.DumpNv12(context.get(), opt.dump.c_str(), &cerr_msg)) {
                    std::printf("wrote %s (%ux%u NV12, BT.709 full range)\n",
                                opt.dump.c_str(), conv.width(), conv.height());
                } else {
                    std::fprintf(stderr, "nv12 dump failed: %s\n", cerr_msg.c_str());
                }
                dumped = true;
            }

            if (swap && sh.latest) {
                winrt::com_ptr<ID3D11Texture2D> back;
                if (SUCCEEDED(swap->GetBuffer(0, winrt::guid_of<ID3D11Texture2D>(),
                                              back.put_void()))) {
                    D3D11_TEXTURE2D_DESC bd{}, ld{};
                    back->GetDesc(&bd);
                    sh.latest->GetDesc(&ld);
                    if (bd.Width == ld.Width && bd.Height == ld.Height) {
                        context->CopyResource(back.get(), sh.latest.get());
                    }
                }
                swap->Present(1, 0);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            next += std::chrono::seconds(1);

            double mean_pixel = -1.0;
            std::vector<double> iv, lat;
            {
                std::lock_guard<std::mutex> lock(sh.mu);
                iv.swap(sh.interval_ms);
                lat.swap(sh.latency_ms);

                // Is this actually a picture, or a very healthy stream of black?
                if (sh.latest && sh.w >= 64 && sh.h >= 64) {
                    if (!sh.staging) {
                        D3D11_TEXTURE2D_DESC d{};
                        d.Width = d.Height = 64;
                        d.MipLevels = d.ArraySize = 1;
                        d.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
                        d.SampleDesc     = {1, 0};
                        d.Usage          = D3D11_USAGE_STAGING;
                        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        device->CreateTexture2D(&d, nullptr, sh.staging.put());
                    }
                    if (sh.staging) {
                        D3D11_BOX box{};
                        box.left   = sh.w / 2 - 32; box.right  = box.left + 64;
                        box.top    = sh.h / 2 - 32; box.bottom = box.top + 64;
                        box.front  = 0;             box.back   = 1;
                        context->CopySubresourceRegion(sh.staging.get(), 0, 0, 0, 0,
                                                       sh.latest.get(), 0, &box);
                        D3D11_MAPPED_SUBRESOURCE m{};
                        if (SUCCEEDED(context->Map(sh.staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
                            std::uint64_t sum = 0;
                            for (UINT y = 0; y < 64; ++y) {
                                const auto* row = static_cast<const std::uint8_t*>(m.pData) + y * m.RowPitch;
                                for (UINT x = 0; x < 64; ++x)
                                    sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
                            }
                            context->Unmap(sh.staging.get(), 0);
                            mean_pixel = static_cast<double>(sum) / (64.0 * 64.0 * 3.0);
                        }
                    }
                }
            }

            auto  ivc  = iv;
            const auto secs = std::chrono::duration<double>(now - start).count();
            std::printf("%5.0fs %6zu %8.2f %8.2f %8.2f %8.2f %8.2f %7s\n",
                        secs, iv.size(),
                        Percentile(ivc, 0.50), Percentile(ivc, 0.99),
                        iv.empty() ? 0.0 : *std::max_element(iv.begin(), iv.end()),
                        Percentile(lat, 0.50), Percentile(lat, 0.99),
                        mean_pixel < 0 ? "n/a" : (mean_pixel < 1.0 ? "BLACK" : "ok"));
        }

        if (opt.seconds > 0 && now - start >= std::chrono::seconds(opt.seconds)) break;
        if (!swap) Sleep(1);
    }

    capture.Stop();
    std::printf("\ntotal frames: %llu   pool resizes: %llu\n",
                static_cast<unsigned long long>(capture.frames()),
                static_cast<unsigned long long>(sh.resized));
    return 0;
}
