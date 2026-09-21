#include "present.h"

#include <d3dcompiler.h>

namespace raidcast {
namespace {

// BT.709 full range, the inverse of the host's conversion (DESIGN.md D6/D7).
constexpr char kShader[] = R"HLSL(
Texture2DArray<float>  srcY  : register(t0);
Texture2DArray<float2> srcUV : register(t1);
RWTexture2D<float4>    dst   : register(u0);

cbuffer Params : register(b0) { uint2 size; uint slice; uint _pad; };

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= size.x || tid.y >= size.y) return;

    float  y  = srcY[uint3(tid.xy, slice)];
    float2 uv = srcUV[uint3(tid.xy / 2, slice)] - 0.5;

    dst[tid.xy] = float4(y + 1.5748 * uv.y,
                         y - 0.1873 * uv.x - 0.4681 * uv.y,
                         y + 1.8556 * uv.x,
                         1.0);
}
)HLSL";

struct Params { std::uint32_t w, h, slice, pad; };

}  // namespace

bool Presenter::Init(ID3D11Device* device, HWND hwnd, std::uint32_t width,
                     std::uint32_t height, std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };

    device_.copy_from(device);
    w_ = width;
    h_ = height;

    winrt::com_ptr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "nv12_to_rgb.hlsl", nullptr, nullptr,
                          "main", "cs_5_0", 0, 0, code.put(), errors.put()))) {
        if (error) {
            *error = "shader compile failed: ";
            if (errors) *error += static_cast<const char*>(errors->GetBufferPointer());
        }
        return false;
    }
    if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                           nullptr, cs_.put())))
        return fail("CreateComputeShader failed");

    auto dxgi_dev = device_.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(adapter.put());
    winrt::com_ptr<IDXGIFactory5> factory;
    if (FAILED(adapter->GetParent(winrt::guid_of<IDXGIFactory5>(), factory.put_void())))
        return fail("IDXGIFactory5 unavailable");

    BOOL allow = FALSE;
    factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow));
    tearing_ = allow != FALSE;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width       = w_;
    scd.Height      = h_;
    // Not BGRA: typed UAV stores to B8G8R8A8_UNORM are not broadly supported.
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc  = {1, 0};
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS;
    scd.BufferCount = 2;
    scd.Scaling     = DXGI_SCALING_STRETCH;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags       = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    if (FAILED(factory->CreateSwapChainForHwnd(device, hwnd, &scd, nullptr, nullptr,
                                               swap_.put())))
        return fail("CreateSwapChainForHwnd failed");

    winrt::com_ptr<ID3D11Texture2D> back;
    if (FAILED(swap_->GetBuffer(0, winrt::guid_of<ID3D11Texture2D>(), back.put_void())))
        return fail("GetBuffer failed");
    if (FAILED(device->CreateUnorderedAccessView(back.get(), nullptr, back_uav_.put())))
        return fail("backbuffer UAV failed");
    if (FAILED(device->CreateRenderTargetView(back.get(), nullptr, back_rtv_.put())))
        return fail("backbuffer RTV failed");

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(Params);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, cb_.put()))) return fail("cbuffer failed");

    D3D11_TEXTURE2D_DESC td{};
    td.Width      = w_;
    td.Height     = h_;
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = DXGI_FORMAT_NV12;
    td.SampleDesc = {1, 0};
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, scratch_.put())))
        return fail("sampleable NV12 texture allocation failed");

    auto plane_srv = [&](DXGI_FORMAT fmt, ID3D11ShaderResourceView** out) {
        // NV12 plane selection is by view format: R8 is Y, R8G8 is chroma.
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format                         = fmt;
        d.ViewDimension                  = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MostDetailedMip = 0;
        d.Texture2DArray.MipLevels       = 1;
        d.Texture2DArray.FirstArraySlice = 0;
        d.Texture2DArray.ArraySize       = 1;
        return SUCCEEDED(device->CreateShaderResourceView(scratch_.get(), &d, out));
    };
    if (!plane_srv(DXGI_FORMAT_R8_UNORM, scratch_views_.y.put()) ||
        !plane_srv(DXGI_FORMAT_R8G8_UNORM, scratch_views_.uv.put()))
        return fail("NV12 plane SRVs failed");

    return true;
}

bool Presenter::Render(ID3D11DeviceContext* ctx, ID3D11Texture2D* nv12,
                       std::uint32_t slice) {
    if (!swap_ || nv12 == nullptr || !scratch_) return false;

    // One VRAM-to-VRAM copy out of the decoder pool into something samplable.
    ctx->CopySubresourceRegion(scratch_.get(), 0, 0, 0, 0, nv12, slice, nullptr);

    if (cb_slice_ != 0) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(cb_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            Params p{w_, h_, 0, 0};  // always slice 0 of the scratch texture
            *static_cast<Params*>(m.pData) = p;
            ctx->Unmap(cb_.get(), 0);
            cb_slice_ = 0;
        }
    }

    ID3D11ShaderResourceView*  srvs[] = {scratch_views_.y.get(), scratch_views_.uv.get()};
    ID3D11UnorderedAccessView* uavs[] = {back_uav_.get()};
    ID3D11Buffer*              cbs[]  = {cb_.get()};

    ctx->CSSetShader(cs_.get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ctx->Dispatch((w_ + 7) / 8, (h_ + 7) / 8, 1);

    ID3D11ShaderResourceView*  no_srv[] = {nullptr, nullptr};
    ID3D11UnorderedAccessView* no_uav[] = {nullptr};
    ctx->CSSetShaderResources(0, 2, no_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, no_uav, nullptr);
    return true;
}

bool Presenter::Swap() {
    // Sync interval 0: present now, tear if we must.
    if (!swap_) return false;
    return SUCCEEDED(swap_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0u));
}

}  // namespace raidcast
