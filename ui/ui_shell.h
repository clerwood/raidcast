// Minimal window + Dear ImGui plumbing shared by the host panel and the viewer
// overlay.
//
// ImGui on D3D11 because we already have a device for capture and encode, it
// needs no runtime install, and this is a status panel rather than an
// application (.local/DESIGN.md D14).

#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <winrt/base.h>

#include <cstdint>
#include <string>

namespace raidcast {

// A plain top-level window with a message pump.
class AppWindow {
public:
    bool Create(const wchar_t* title, int width, int height, std::string* error = nullptr);
    void Destroy();

    // Drains pending messages. Returns false once the user has closed it.
    bool Pump();

    HWND hwnd() const { return hwnd_; }
    void Size(std::uint32_t* w, std::uint32_t* h) const;

private:
    HWND hwnd_ = nullptr;
};

// A swapchain sized to the window, for UI that is not drawing video.
class UiSwapchain {
public:
    bool Init(ID3D11Device* device, HWND hwnd, std::string* error = nullptr);
    bool ResizeIfNeeded(std::uint32_t w, std::uint32_t h);
    ID3D11RenderTargetView* rtv() const { return rtv_.get(); }
    void Present();

private:
    winrt::com_ptr<ID3D11Device>           device_;
    winrt::com_ptr<IDXGISwapChain1>        swap_;
    winrt::com_ptr<ID3D11RenderTargetView> rtv_;
    std::uint32_t w_ = 0, h_ = 0;
};

class ImGuiShell {
public:
    bool Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx,
              std::string* error = nullptr);
    void Shutdown();

    void NewFrame();
    void RenderTo(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv);

private:
    bool ready_ = false;
};

}  // namespace raidcast
