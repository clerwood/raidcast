// BGRA -> NV12 colour conversion on the GPU.
//
// The captured frame never leaves VRAM: WGC hands us a BGRA texture, a compute
// shader writes NV12 planes, and the encoder consumes those directly
// (.local/DESIGN.md §6).
//
// Colour: BT.709, **full range**. We control both ends of this link, and screen
// content is the one case where limited range's lost code levels are actually
// visible — banding on UI gradients and flat panel backgrounds. The range is
// signalled in the bitstream VUI, so the viewer reproduces it correctly.

#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <string>

namespace raidcast {

class Bgra2Nv12 {
public:
    bool Init(ID3D11Device* device, std::uint32_t width, std::uint32_t height,
              std::string* error = nullptr);

    // Converts into the internally-owned Y and UV planes.
    bool Convert(ID3D11DeviceContext* ctx, ID3D11Texture2D* bgra);

    // Y is R8_UNORM at full resolution; UV is R8G8_UNORM at half resolution.
    ID3D11Texture2D* y_plane()  const { return y_.get(); }
    ID3D11Texture2D* uv_plane() const { return uv_.get(); }

    std::uint32_t width()  const { return w_; }
    std::uint32_t height() const { return h_; }

    // Reads both planes back and writes a raw NV12 frame (Y plane followed by
    // interleaved UV). Slow — diagnostics only, never on the streaming path.
    bool DumpNv12(ID3D11DeviceContext* ctx, const char* path, std::string* error = nullptr);

private:
    winrt::com_ptr<ID3D11ComputeShader>       cs_;
    winrt::com_ptr<ID3D11Texture2D>           y_, uv_;
    winrt::com_ptr<ID3D11UnorderedAccessView> y_uav_, uv_uav_;
    winrt::com_ptr<ID3D11Texture2D>           y_stage_, uv_stage_;
    winrt::com_ptr<ID3D11Buffer>              cb_;
    winrt::com_ptr<ID3D11Device>              device_;
    std::uint32_t                             w_ = 0, h_ = 0;
};

}  // namespace raidcast
