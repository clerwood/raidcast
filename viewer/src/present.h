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
#include <map>
#include <string>
#include <utility>

namespace raidcast {

class Presenter {
public:
    bool Init(ID3D11Device* device, HWND hwnd, std::uint32_t width, std::uint32_t height,
              std::string* error = nullptr);

    // `nv12` is a decoder pool texture; `slice` selects the array element.
    bool Present(ID3D11DeviceContext* ctx, ID3D11Texture2D* nv12, std::uint32_t slice);

    bool tearing_allowed() const { return tearing_; }

private:
    struct Views {
        winrt::com_ptr<ID3D11ShaderResourceView> y, uv;
    };
    Views* PlaneViews(ID3D11Texture2D* nv12, std::uint32_t slice);

    winrt::com_ptr<ID3D11Device>              device_;
    winrt::com_ptr<IDXGISwapChain1>           swap_;
    winrt::com_ptr<ID3D11ComputeShader>       cs_;
    winrt::com_ptr<ID3D11UnorderedAccessView> back_uav_;
    winrt::com_ptr<ID3D11Buffer>              cb_;
    std::map<std::pair<ID3D11Texture2D*, std::uint32_t>, Views> views_;
    std::uint32_t w_ = 0, h_ = 0;
    std::uint32_t cb_slice_ = 0xFFFFFFFF;
    bool          tearing_  = false;
};

}  // namespace raidcast
