#include "ui_shell.h"

#include <algorithm>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace raidcast {
namespace {

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool AppWindow::Create(const wchar_t* title, int width, int height, std::string* error) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RaidCastWindow";
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    RECT want{0, 0, width, height};
    AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                            nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd_) {
        if (error) *error = "CreateWindowEx failed";
        return false;
    }
    ShowWindow(hwnd_, SW_SHOW);
    return true;
}

void AppWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool AppWindow::Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

void AppWindow::Size(std::uint32_t* w, std::uint32_t* h) const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    *w = static_cast<std::uint32_t>(std::max<LONG>(rc.right - rc.left, 1));
    *h = static_cast<std::uint32_t>(std::max<LONG>(rc.bottom - rc.top, 1));
}

bool UiSwapchain::Init(ID3D11Device* device, HWND hwnd, std::string* error) {
    device_.copy_from(device);

    auto dxgi_dev = device_.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(adapter.put());
    winrt::com_ptr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(winrt::guid_of<IDXGIFactory2>(), factory.put_void()))) {
        if (error) *error = "IDXGIFactory2 unavailable";
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    w_ = static_cast<std::uint32_t>(std::max<LONG>(rc.right - rc.left, 1));
    h_ = static_cast<std::uint32_t>(std::max<LONG>(rc.bottom - rc.top, 1));

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width       = w_;
    scd.Height      = h_;
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc  = {1, 0};
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    if (FAILED(factory->CreateSwapChainForHwnd(device, hwnd, &scd, nullptr, nullptr,
                                               swap_.put()))) {
        if (error) *error = "CreateSwapChainForHwnd failed";
        return false;
    }
    return ResizeIfNeeded(w_, h_);
}

bool UiSwapchain::ResizeIfNeeded(std::uint32_t w, std::uint32_t h) {
    if (!swap_) return false;
    if (rtv_ && w == w_ && h == h_) return true;

    rtv_ = nullptr;
    if (w != w_ || h != h_) {
        if (FAILED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return false;
        w_ = w;
        h_ = h;
    }
    winrt::com_ptr<ID3D11Texture2D> back;
    if (FAILED(swap_->GetBuffer(0, winrt::guid_of<ID3D11Texture2D>(), back.put_void())))
        return false;
    return SUCCEEDED(device_->CreateRenderTargetView(back.get(), nullptr, rtv_.put()));
}

void UiSwapchain::Reset(ID3D11DeviceContext* ctx) {
    if (ctx) {
        // A bound render target keeps a reference alive and the release silently
        // does nothing, so unbind and flush before dropping it.
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->Flush();
    }
    rtv_  = nullptr;
    swap_ = nullptr;
    w_ = h_ = 0;
}

void UiSwapchain::Present() {
    if (swap_) swap_->Present(1, 0);  // UI is not latency-critical; pace it to vblank
}

bool ImGuiShell::Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx,
                      std::string* error) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no layout file next to the exe
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(hwnd) || !ImGui_ImplDX11_Init(device, ctx)) {
        if (error) *error = "ImGui backend init failed";
        return false;
    }
    ready_ = true;
    return true;
}

void ImGuiShell::Shutdown() {
    if (!ready_) return;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ready_ = false;
}

void ImGuiShell::NewFrame() {
    if (!ready_) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiShell::RenderTo(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv) {
    if (!ready_) return;
    ImGui::Render();
    ID3D11RenderTargetView* rtvs[] = {rtv};
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace raidcast
