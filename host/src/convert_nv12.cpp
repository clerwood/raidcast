#include "convert_nv12.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <vector>

namespace raidcast {
namespace {

// TODO(perf): precompile with fxc at build time and embed the bytecode. Runtime
// D3DCompile costs ~10 ms once at startup, which is fine but is a needless
// runtime dependency on d3dcompiler_47.dll.
constexpr char kShader[] = R"HLSL(
Texture2D<float4>   src   : register(t0);
RWTexture2D<float>  dstY  : register(u0);
RWTexture2D<float2> dstUV : register(u1);

cbuffer Params : register(b0) { uint2 size; uint2 _pad; };

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);  // BT.709
float Luma(float3 c) { return dot(c, kLuma); }

// One thread per 2x2 block: four luma samples, one chroma pair.
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy * 2;
    if (p.x >= size.x || p.y >= size.y) return;

    uint2 p1 = uint2(min(p.x + 1, size.x - 1), p.y);
    uint2 p2 = uint2(p.x, min(p.y + 1, size.y - 1));
    uint2 p3 = uint2(p1.x, p2.y);

    float3 c0 = src[p].rgb, c1 = src[p1].rgb, c2 = src[p2].rgb, c3 = src[p3].rgb;

    dstY[p]  = Luma(c0);
    dstY[p1] = Luma(c1);
    dstY[p2] = Luma(c2);
    dstY[p3] = Luma(c3);

    float3 avg = (c0 + c1 + c2 + c3) * 0.25;
    float  ya  = Luma(avg);
    dstUV[tid.xy] = float2((avg.b - ya) / 1.8556 + 0.5,
                           (avg.r - ya) / 1.5748 + 0.5);
}
)HLSL";

bool MakeUav(ID3D11Device* dev, ID3D11Texture2D* tex, DXGI_FORMAT fmt,
             std::uint32_t slice, ID3D11UnorderedAccessView** out) {
    // NV12 plane selection in D3D11 is by view format, not a plane index:
    // R8_UNORM addresses Y, R8G8_UNORM addresses the interleaved UV plane.
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format                         = fmt;
    d.ViewDimension                  = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    d.Texture2DArray.MipSlice        = 0;
    d.Texture2DArray.FirstArraySlice = slice;
    d.Texture2DArray.ArraySize       = 1;
    return SUCCEEDED(dev->CreateUnorderedAccessView(tex, &d, out));
}

}  // namespace

bool Bgra2Nv12::Init(ID3D11Device* device, std::uint32_t width, std::uint32_t height,
                     std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };

    device_.copy_from(device);
    w_ = width & ~1u;   // NV12 chroma is subsampled; odd dimensions have no
    h_ = height & ~1u;  // representation, so round down rather than guess
    if (w_ == 0 || h_ == 0) return fail("degenerate capture size");

    winrt::com_ptr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "bgra_to_nv12.hlsl", nullptr,
                          nullptr, "main", "cs_5_0", 0, 0, code.put(), errors.put()))) {
        if (error) {
            *error = "shader compile failed: ";
            if (errors) *error += static_cast<const char*>(errors->GetBufferPointer());
        }
        return false;
    }
    if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                           nullptr, cs_.put())))
        return fail("CreateComputeShader failed");

    D3D11_TEXTURE2D_DESC d{};
    d.Width      = w_;
    d.Height     = h_;
    d.MipLevels  = 1;
    d.ArraySize  = 1;
    d.Format     = DXGI_FORMAT_NV12;
    d.SampleDesc = {1, 0};
    d.Usage      = D3D11_USAGE_DEFAULT;
    d.BindFlags  = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&d, nullptr, nv12_.put())))
        return fail("NV12 texture allocation failed (BIND_UNORDERED_ACCESS unsupported?)");

    if (!PlaneViews(nv12_.get(), 0))
        return fail("NV12 plane UAVs unsupported — typed UAV stores on R8/R8G8 unavailable");

    struct Params { std::uint32_t w, h, p0, p1; } params{w_, h_, 0, 0};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Params);
    bd.Usage     = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{&params, 0, 0};
    if (FAILED(device->CreateBuffer(&bd, &init, cb_.put()))) return fail("cbuffer failed");

    return true;
}

Bgra2Nv12::UavPair* Bgra2Nv12::PlaneViews(ID3D11Texture2D* nv12, std::uint32_t slice) {
    const auto key = std::make_pair(nv12, slice);
    auto it = uavs_.find(key);
    if (it != uavs_.end()) return &it->second;

    UavPair pair;
    if (!MakeUav(device_.get(), nv12, DXGI_FORMAT_R8_UNORM, slice, pair.first.put()) ||
        !MakeUav(device_.get(), nv12, DXGI_FORMAT_R8G8_UNORM, slice, pair.second.put()))
        return nullptr;

    return &uavs_.emplace(key, std::move(pair)).first->second;
}

bool Bgra2Nv12::ConvertInto(ID3D11DeviceContext* ctx, ID3D11Texture2D* bgra,
                            ID3D11Texture2D* nv12, std::uint32_t slice) {
    if (!cs_ || bgra == nullptr || nv12 == nullptr) return false;

    UavPair* planes = PlaneViews(nv12, slice);
    if (!planes) return false;

    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    if (FAILED(device_->CreateShaderResourceView(bgra, nullptr, srv.put()))) return false;

    ID3D11ShaderResourceView*  srvs[] = {srv.get()};
    ID3D11UnorderedAccessView* uavs[] = {planes->first.get(), planes->second.get()};
    ID3D11Buffer*              cbs[]  = {cb_.get()};

    ctx->CSSetShader(cs_.get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ctx->Dispatch((w_ / 2 + 7) / 8, (h_ / 2 + 7) / 8, 1);

    // Unbind so the encoder can read the texture next.
    ID3D11ShaderResourceView*  no_srv[] = {nullptr};
    ID3D11UnorderedAccessView* no_uav[] = {nullptr, nullptr};
    ctx->CSSetShaderResources(0, 1, no_srv);
    ctx->CSSetUnorderedAccessViews(0, 2, no_uav, nullptr);
    return true;
}

bool Bgra2Nv12::Convert(ID3D11DeviceContext* ctx, ID3D11Texture2D* bgra) {
    return ConvertInto(ctx, bgra, nv12_.get(), 0);
}

bool Bgra2Nv12::DumpNv12(ID3D11DeviceContext* ctx, const char* path, std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };

    if (!stage_) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width          = w_;
        d.Height         = h_;
        d.MipLevels      = 1;
        d.ArraySize      = 1;
        d.Format         = DXGI_FORMAT_NV12;
        d.SampleDesc     = {1, 0};
        d.Usage          = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, stage_.put())))
            return fail("NV12 staging allocation failed");
    }

    ctx->CopyResource(stage_.get(), nv12_.get());

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(stage_.get(), 0, D3D11_MAP_READ, 0, &m))) return fail("Map failed");

    FILE* f = std::fopen(path, "wb");
    if (!f) {
        ctx->Unmap(stage_.get(), 0);
        return fail("cannot open output file");
    }

    // D3D11 NV12 layout: Y rows first, then the UV plane at RowPitch * Height.
    const auto* base = static_cast<const std::uint8_t*>(m.pData);
    for (std::uint32_t y = 0; y < h_; ++y)
        std::fwrite(base + static_cast<std::size_t>(y) * m.RowPitch, 1, w_, f);
    const auto* uv = base + static_cast<std::size_t>(m.RowPitch) * h_;
    for (std::uint32_t y = 0; y < h_ / 2; ++y)
        std::fwrite(uv + static_cast<std::size_t>(y) * m.RowPitch, 1, w_, f);

    std::fclose(f);
    ctx->Unmap(stage_.get(), 0);
    return true;
}

}  // namespace raidcast
