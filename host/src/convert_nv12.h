// BGRA -> NV12 colour conversion on the GPU.
//
// The captured frame never leaves VRAM: WGC hands us a BGRA texture, a compute
// shader writes both NV12 planes, and the encoder consumes that texture
// directly (.local/DESIGN.md §6).
//
// Colour: BT.709, **full range**. We control both ends of this link, and screen
// content is the one case where limited range's lost code levels are actually
// visible — banding on UI gradients and flat panel backgrounds. The range is
// signalled in the bitstream VUI, so the viewer reproduces it correctly.

#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace raidcast {

class Bgra2Nv12 {
public:
    bool Init(ID3D11Device* device, std::uint32_t width, std::uint32_t height,
              std::string* error = nullptr);

    // Converts into the internally-owned NV12 texture.
    bool Convert(ID3D11DeviceContext* ctx, ID3D11Texture2D* bgra);

    // Converts straight into a caller-owned NV12 texture — used to write into
    // the encoder's hardware frame pool with no intermediate copy. `slice`
    // selects the array element, since those pools are texture arrays.
    bool ConvertInto(ID3D11DeviceContext* ctx, ID3D11Texture2D* bgra,
                     ID3D11Texture2D* nv12, std::uint32_t slice);

    ID3D11Texture2D* nv12()   const { return nv12_.get(); }
    std::uint32_t    width()  const { return w_; }
    std::uint32_t    height() const { return h_; }

    // Reads the owned texture back and writes a raw NV12 frame. Slow —
    // diagnostics only, never on the streaming path.
    bool DumpNv12(ID3D11DeviceContext* ctx, const char* path, std::string* error = nullptr);

private:
    using UavPair = std::pair<winrt::com_ptr<ID3D11UnorderedAccessView>,
                              winrt::com_ptr<ID3D11UnorderedAccessView>>;

    // Plane views are cached per (texture, slice): the encoder cycles through a
    // small fixed pool, so this converges after the first few frames.
    UavPair* PlaneViews(ID3D11Texture2D* nv12, std::uint32_t slice);

    winrt::com_ptr<ID3D11ComputeShader> cs_;
    winrt::com_ptr<ID3D11Texture2D>     nv12_, stage_;
    winrt::com_ptr<ID3D11Buffer>        cb_;
    winrt::com_ptr<ID3D11Device>        device_;
    std::map<std::pair<ID3D11Texture2D*, std::uint32_t>, UavPair> uavs_;
    std::uint32_t                       w_ = 0, h_ = 0;
};

}  // namespace raidcast
