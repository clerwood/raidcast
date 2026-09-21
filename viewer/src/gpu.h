// Adapter enumeration and decode capability.
//
// The decode path is D3D11VA, which is the vendor-neutral Windows video
// acceleration API - AMD, NVIDIA and Intel all implement HEVC through it. This
// exists to prove that per machine rather than assume it, and to let a viewer on
// a hybrid-graphics laptop pick the adapter that can actually decode.

#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <string>
#include <vector>

namespace raidcast {

struct AdapterInfo {
    int           index = 0;
    std::string   name;
    std::uint64_t vram_mb = 0;
    bool          d3d11       = false;  // a D3D11 device could be created at all
    bool          hevc_main   = false;  // 8-bit HEVC decode, which is what we send
    bool          hevc_main10 = false;
    bool          h264        = false;  // the fallback codec
    std::string   note;                 // why something is unavailable
};

std::vector<AdapterInfo> EnumerateAdapters();

// Creates a device on a specific adapter, or the default when index < 0.
bool CreateDeviceOnAdapter(int index, winrt::com_ptr<ID3D11Device>* device,
                           winrt::com_ptr<ID3D11DeviceContext>* context,
                           std::string* chosen_name, std::string* error);

}  // namespace raidcast
