// Must precede any d3d11 header: the decoder profile GUIDs are declared by the
// SDK but only defined when INITGUID is in effect, and they live in no import
// library.
#include <initguid.h>

#include "gpu.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>

namespace raidcast {
namespace {

std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

winrt::com_ptr<IDXGIFactory1> Factory() {
    winrt::com_ptr<IDXGIFactory1> f;
    CreateDXGIFactory1(winrt::guid_of<IDXGIFactory1>(), f.put_void());
    return f;
}

// Asks the driver directly whether it can decode a profile, which is the only
// answer that means anything - vendor and model names do not tell you.
bool SupportsProfile(ID3D11Device* device, const GUID& profile) {
    winrt::com_ptr<ID3D11VideoDevice> vd;
    if (FAILED(device->QueryInterface(winrt::guid_of<ID3D11VideoDevice>(), vd.put_void())))
        return false;

    const UINT count = vd->GetVideoDecoderProfileCount();
    for (UINT i = 0; i < count; ++i) {
        GUID g{};
        if (FAILED(vd->GetVideoDecoderProfile(i, &g))) continue;
        if (g != profile) continue;

        // Present in the list is not the same as usable with our pixel format.
        BOOL supported = FALSE;
        if (SUCCEEDED(vd->CheckVideoDecoderFormat(&g, DXGI_FORMAT_NV12, &supported)) && supported)
            return true;
    }
    return false;
}

}  // namespace

std::vector<AdapterInfo> EnumerateAdapters() {
    std::vector<AdapterInfo> out;
    auto factory = Factory();
    if (!factory) return out;

    for (UINT i = 0;; ++i) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        AdapterInfo info;
        info.index   = static_cast<int>(i);
        info.name    = Narrow(desc.Description);
        info.vram_mb = desc.DedicatedVideoMemory / (1024 * 1024);

        // Skip Microsoft's software renderer: it reports no decode support and
        // listing it only invites someone to select it.
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            info.note = "software renderer";
            out.push_back(std::move(info));
            continue;
        }

        winrt::com_ptr<ID3D11Device> device;
        D3D_FEATURE_LEVEL            fl{};
        const HRESULT hr = D3D11CreateDevice(
            adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, device.put(), &fl, nullptr);
        if (FAILED(hr) || !device) {
            info.note = "no D3D11 device";
            out.push_back(std::move(info));
            continue;
        }
        info.d3d11 = true;

        info.hevc_main   = SupportsProfile(device.get(), D3D11_DECODER_PROFILE_HEVC_VLD_MAIN);
        info.hevc_main10 = SupportsProfile(device.get(), D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10);
        info.h264        = SupportsProfile(device.get(), D3D11_DECODER_PROFILE_H264_VLD_NOFGT);
        out.push_back(std::move(info));
    }
    return out;
}

bool CreateDeviceOnAdapter(int index, winrt::com_ptr<ID3D11Device>* device,
                           winrt::com_ptr<ID3D11DeviceContext>* context,
                           std::string* chosen_name, std::string* error) {
    // With no explicit choice, prefer an adapter that can actually decode HEVC
    // rather than whatever D3D11 picks by default. On a machine with mixed
    // graphics the default may be the one that cannot, and the symptom of that
    // is a blank window rather than an error.
    if (index < 0) {
        for (const auto& a : EnumerateAdapters()) {
            if (a.d3d11 && a.hevc_main) {
                index = a.index;
                break;
            }
        }
    }

    winrt::com_ptr<IDXGIAdapter1> adapter;
    if (index >= 0) {
        auto factory = Factory();
        if (!factory || factory->EnumAdapters1(static_cast<UINT>(index), adapter.put()) ==
                            DXGI_ERROR_NOT_FOUND) {
            if (error) *error = "no adapter with index " + std::to_string(index);
            return false;
        }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (chosen_name) *chosen_name = Narrow(desc.Description);
    }

    D3D_FEATURE_LEVEL fl{};
    const HRESULT hr = D3D11CreateDevice(
        adapter.get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, device->put(), &fl, context->put());
    if (FAILED(hr)) {
        if (error) *error = "D3D11CreateDevice failed";
        return false;
    }

    if (!adapter && chosen_name) {
        // Report what the default actually resolved to, so the log is useful.
        if (auto dxgi = device->try_as<IDXGIDevice>()) {
            winrt::com_ptr<IDXGIAdapter> got;
            if (SUCCEEDED(dxgi->GetAdapter(got.put()))) {
                DXGI_ADAPTER_DESC d{};
                got->GetDesc(&d);
                *chosen_name = Narrow(d.Description);
            }
        }
    }
    return true;
}

}  // namespace raidcast
