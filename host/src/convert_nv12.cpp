#include "convert_nv12.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <vector>

namespace raidcast {
namespace {

// TODO(perf): precompile with fxc at build time and embed the bytecode. Runtime
// D3DCompile costs ~10 ms once at startup, which is fine for now but is a
// needless dependency on d3dcompiler_47.dll.
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

bool MakeTex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind,
             D3D11_USAGE usage, UINT cpu, ID3D11Texture2D** out) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width          = w;
    d.Height         = h;
    d.MipLevels      = 1;
    d.ArraySize      = 1;
    d.Format         = fmt;
    d.SampleDesc     = {1, 0};
    d.Usage          = usage;
    d.BindFlags      = bind;
    d.CPUAccessFlags = cpu;
    return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, out));
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
                                           nullptr, cs_.put()))) {
        return fail("CreateComputeShader failed");
    }

    if (!MakeTex(device, w_, h_, DXGI_FORMAT_R8_UNORM,
                 D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                 D3D11_USAGE_DEFAULT, 0, y_.put()))
        return fail("Y plane allocation failed");

    if (!MakeTex(device, w_ / 2, h_ / 2, DXGI_FORMAT_R8G8_UNORM,
                 D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                 D3D11_USAGE_DEFAULT, 0, uv_.put()))
        return fail("UV plane allocation failed");

    if (FAILED(device->CreateUnorderedAccessView(y_.get(), nullptr, y_uav_.put())) ||
        FAILED(device->CreateUnorderedAccessView(uv_.get(), nullptr, uv_uav_.put())))
        return fail("UAV creation failed — typed UAV stores unsupported for R8/R8G8?");

    struct Params { std::uint32_t w, h, p0, p1; } params{w_, h_, 0, 0};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(Params);
    bd.Usage          = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{&params, 0, 0};
    if (FAILED(device->CreateBuffer(&bd, &init, cb_.put()))) return fail("cbuffer failed");

    return true;
}

bool Bgra2Nv12::Convert(ID3D11DeviceContext* ctx, ID3D11Texture2D* bgra) {
    if (!cs_ || bgra == nullptr) return false;

    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    if (FAILED(device_->CreateShaderResourceView(bgra, nullptr, srv.put()))) return false;

    ID3D11ShaderResourceView*  srvs[] = {srv.get()};
    ID3D11UnorderedAccessView* uavs[] = {y_uav_.get(), uv_uav_.get()};
    ID3D11Buffer*              cbs[]  = {cb_.get()};

    ctx->CSSetShader(cs_.get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ctx->Dispatch((w_ / 2 + 7) / 8, (h_ / 2 + 7) / 8, 1);

    // Unbind so the textures can be read by the encoder next.
    ID3D11ShaderResourceView*  no_srv[] = {nullptr};
    ID3D11UnorderedAccessView* no_uav[] = {nullptr, nullptr};
    ctx->CSSetShaderResources(0, 1, no_srv);
    ctx->CSSetUnorderedAccessViews(0, 2, no_uav, nullptr);
    return true;
}

bool Bgra2Nv12::DumpNv12(ID3D11DeviceContext* ctx, const char* path, std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };

    if (!y_stage_ &&
        !MakeTex(device_.get(), w_, h_, DXGI_FORMAT_R8_UNORM, 0, D3D11_USAGE_STAGING,
                 D3D11_CPU_ACCESS_READ, y_stage_.put()))
        return fail("Y staging allocation failed");
    if (!uv_stage_ &&
        !MakeTex(device_.get(), w_ / 2, h_ / 2, DXGI_FORMAT_R8G8_UNORM, 0,
                 D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, uv_stage_.put()))
        return fail("UV staging allocation failed");

    ctx->CopyResource(y_stage_.get(), y_.get());
    ctx->CopyResource(uv_stage_.get(), uv_.get());

    FILE* f = std::fopen(path, "wb");
    if (!f) return fail("cannot open output file");

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(y_stage_.get(), 0, D3D11_MAP_READ, 0, &m))) {
        for (std::uint32_t y = 0; y < h_; ++y)
            std::fwrite(static_cast<const std::uint8_t*>(m.pData) + y * m.RowPitch, 1, w_, f);
        ctx->Unmap(y_stage_.get(), 0);
    }
    if (SUCCEEDED(ctx->Map(uv_stage_.get(), 0, D3D11_MAP_READ, 0, &m))) {
        for (std::uint32_t y = 0; y < h_ / 2; ++y)
            std::fwrite(static_cast<const std::uint8_t*>(m.pData) + y * m.RowPitch, 1, w_, f);
        ctx->Unmap(uv_stage_.get(), 0);
    }
    std::fclose(f);
    return true;
}

}  // namespace raidcast
