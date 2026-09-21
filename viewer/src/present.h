// Presents decoded NV12 frames to a window.
//
// Flip-model swapchain, FLIP_DISCARD, ALLOW_TEARING, presented immediately
// (.local/DESIGN.md §6). The viewer is reading an information display, so half a
// refresh of queueing is ~8 ms of the C3 budget spent on nothing. Tear instead.
//
// The backbuffer is allocated at the video's native size and DXGI stretches it
// to the window, which keeps the conversion shader trivial and costs nothing.

#pragma once

#include <d3d11.h>
#include <dxgi1_5.h>
#include <windows.h>
#include <winrt/base.h>

#include <cstdint>
#include <string>

namespace raidcast {

class Presenter {
public:
    bool Init(ID3D11Device* device, HWND hwnd, std::uint32_t width, std::uint32_t height,
              std::string* error = nullptr);

    // Converts a decoded frame into the backbuffer. Split from Swap() so the
    // stats overlay can draw on top before anything reaches the screen.
    // `nv12` is a decoder pool texture; `slice` selects the array element.
    bool Render(ID3D11DeviceContext* ctx, ID3D11Texture2D* nv12, std::uint32_t slice);

    // Backbuffer render target, for drawing UI over the video.
    ID3D11RenderTargetView* rtv() const { return back_rtv_.get(); }

    bool Swap();

    bool tearing_allowed() const { return tearing_; }

private:
    struct Views {
        winrt::com_ptr<ID3D11ShaderResourceView> y, uv;
    };

    winrt::com_ptr<ID3D11Device>              device_;
    winrt::com_ptr<IDXGISwapChain1>           swap_;
    winrt::com_ptr<ID3D11ComputeShader>       cs_;
    winrt::com_ptr<ID3D11UnorderedAccessView> back_uav_;
    winrt::com_ptr<ID3D11RenderTargetView>    back_rtv_;
    winrt::com_ptr<ID3D11Buffer>              cb_;
    // libav's D3D11VA decoder allocates its frame pool with BIND_DECODER only,
    // and an NV12 texture array cannot carry BIND_SHADER_RESOURCE as well (the
    // same driver constraint the encoder hits from the other direction). So the
    // decoded slice is copied into a texture we own that can be sampled.
    winrt::com_ptr<ID3D11Texture2D> scratch_;
    Views                           scratch_views_;
    std::uint32_t w_ = 0, h_ = 0;
    std::uint32_t cb_slice_ = 0xFFFFFFFF;
    bool          tearing_  = false;
};

}  // namespace raidcast
