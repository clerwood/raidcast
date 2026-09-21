// Hardware video decoder, via libav with D3D11VA.
//
// Decoded frames stay as D3D11 textures so present is a copy inside VRAM
// (.local/DESIGN.md §6). Headers ride in-band, so no out-of-band extradata has
// to survive the transport.

#pragma once

#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace raidcast {

struct DecodedFrame {
    ID3D11Texture2D* texture = nullptr;  // borrowed; valid for the callback only
    std::uint32_t    slice   = 0;        // array index within the pool
    std::uint32_t    width   = 0;
    std::uint32_t    height  = 0;
    std::int64_t     pts_us  = 0;
};

class Decoder {
public:
    using FrameSink = std::function<void(const DecodedFrame&)>;

    Decoder();
    ~Decoder();
    Decoder(const Decoder&)            = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool Open(ID3D11Device* device, bool hevc, std::string* error = nullptr);

    // Feeds one reassembled access unit and drains whatever comes out.
    bool Decode(const std::uint8_t* data, std::size_t len, std::int64_t pts_us,
                const FrameSink& sink, std::string* error = nullptr);

    // Copies the most recent frame to system memory as NV12. Diagnostics only.
    bool DumpLastNv12(const char* path, std::string* error = nullptr);

    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace raidcast
