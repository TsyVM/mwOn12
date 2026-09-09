// GPU-side copies, rescales and mip generation for the D3D12 backend.
//
// D3D12's copy operations require the source and destination to match in size
// and format. D3D9's StretchRect does not -- it rescales and filters, and
// across formats -- and mip generation means repeatedly halving an image.
// Neither can be expressed as a copy.
//
// Both are therefore done by drawing a full-screen triangle that samples the
// source, with a pipeline state per destination format. The mip chain is
// produced one level at a time, each rendering from the level above.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/Blitter12.h>
#include <core/DeviceContext12.h>
#include <core/PSOCache12.h>
#include <core/Log.h>

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace mwon12 {

namespace {

constexpr char kBlitVS[] =
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut main(uint vid : SV_VertexID) {\n"
    "  VSOut o;\n"
    "  float2 uv = float2((vid << 1) & 2, vid & 2);\n"
    "  o.uv  = uv;\n"
    "  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "  return o;\n"
    "}\n";

constexpr char kBlitPS[] =
    "Texture2D    gSrc : register(t0);\n"
    "SamplerState gSmp : register(s0);\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(VSOut i) : SV_Target { return gSrc.Sample(gSmp, i.uv); }\n";

HRESULT CompileBlit(const char* src, const char* target,
                    std::vector<uint8_t>& out) noexcept
{
    ComPtr<ID3DBlob> blob, errs;
    const HRESULT hr = D3DCompile(src, std::strlen(src), "mw4real_blit",
                                  nullptr, nullptr, "main", target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                  blob.GetAddressOf(), errs.GetAddressOf());
    if (FAILED(hr)) {
        if (errs)
            DXLOG_ERROR("[dx12] blit shader (%s) failed: %s", target,
                        static_cast<const char*>(errs->GetBufferPointer()));
        return hr;
    }
    out.assign(static_cast<const uint8_t*>(blob->GetBufferPointer()),
               static_cast<const uint8_t*>(blob->GetBufferPointer()) +
                   blob->GetBufferSize());
    return S_OK;
}

}

HRESULT Blitter12::EnsureShaders() noexcept
{
    if (!m_vsDxbc.empty() && !m_psDxbc.empty()) return S_OK;
    HRESULT hr = CompileBlit(kBlitVS, "vs_5_0", m_vsDxbc);
    if (FAILED(hr)) return hr;
    hr = CompileBlit(kBlitPS, "ps_5_0", m_psDxbc);
    if (FAILED(hr)) return hr;

    D3D12_SAMPLER_DESC sd{};
    sd.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.MaxAnisotropy  = 1;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.MinLOD         = 0.0f;
    sd.MaxLOD         = D3D12_FLOAT32_MAX;

    // Allocated only when not already held. A previous call that compiled the
    // vertex shader and then failed on the pixel shader comes back through
    // here, and re-allocating would strand the descriptors it took the first
    // time -- the staging heap is fixed-size and has no owner to reclaim them.
    if (m_pointSampler.ptr == SIZE_T(-1)) {
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        m_pointSampler = m_ctx->SamplerStaging().Alloc();
        if (m_pointSampler.ptr == SIZE_T(-1)) return E_OUTOFMEMORY;
        m_ctx->Device()->CreateSampler(&sd, m_pointSampler);
    }

    if (m_linearSampler.ptr == SIZE_T(-1)) {
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        m_linearSampler = m_ctx->SamplerStaging().Alloc();
        if (m_linearSampler.ptr == SIZE_T(-1)) return E_OUTOFMEMORY;
        m_ctx->Device()->CreateSampler(&sd, m_linearSampler);
    }

    return S_OK;
}

void Blitter12::Clear() noexcept
{
    m_pipelines.clear();

    // The two samplers go back to the staging heap. Without this a Clear
    // followed by any further use leaks a descriptor slot per cycle, and the
    // heap is fixed-size.
    if (m_ctx) {
        if (m_pointSampler.ptr  != SIZE_T(-1)) m_ctx->SamplerStaging().Free(m_pointSampler);
        if (m_linearSampler.ptr != SIZE_T(-1)) m_ctx->SamplerStaging().Free(m_linearSampler);
    }
    m_pointSampler  = D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };
    m_linearSampler = D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };

    // Dropped with the samplers, not kept: EnsureShaders treats non-empty
    // bytecode as "already set up" and would skip re-allocating them.
    m_vsDxbc.clear();
    m_psDxbc.clear();
}

ID3D12PipelineState* Blitter12::PipelineFor(DXGI_FORMAT dstFormat) noexcept
{
    const uint32_t key = static_cast<uint32_t>(dstFormat);
    if (const auto it = m_pipelines.find(key); it != m_pipelines.end())
        return it->second.Get();

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_ctx->RootSignature();
    pd.VS = { m_vsDxbc.data(), m_vsDxbc.size() };
    pd.PS = { m_psDxbc.data(), m_psDxbc.size() };

    for (auto& rt : pd.BlendState.RenderTarget)
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pd.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;

    pd.DepthStencilState.DepthEnable   = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;

    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets      = 1;
    pd.RTVFormats[0]         = dstFormat;
    pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count      = 1;
    pd.SampleMask            = UINT_MAX;

    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = m_ctx->Device()->CreateGraphicsPipelineState(
        &pd, IID_PPV_ARGS(pso.GetAddressOf()));
    if (FAILED(hr)) {
        DXLOG_HR(hr, "[dx12] blit pipeline for format %d failed", (int)dstFormat);
        return nullptr;
    }
    auto [it, inserted] = m_pipelines.emplace(key, std::move(pso));
    return it->second.Get();
}

HRESULT Blitter12::Blit(D3D12_CPU_DESCRIPTOR_HANDLE dstRtv, DXGI_FORMAT dstFormat,
                        UINT dstWidth, UINT dstHeight, const RECT& dstRect,
                        D3D12_CPU_DESCRIPTOR_HANDLE srcSrv,
                        UINT srcWidth, UINT srcHeight, const RECT& srcRect,
                        bool linearFilter) noexcept
{
    if (!m_ctx || !m_ctx->CmdList()) return E_FAIL;
    if (dstRtv.ptr == SIZE_T(-1) || srcSrv.ptr == SIZE_T(-1)) return E_INVALIDARG;

    HRESULT hr = EnsureShaders();
    if (FAILED(hr)) return hr;

    ID3D12PipelineState* pso = PipelineFor(dstFormat);
    if (!pso) return E_FAIL;

    D3D12_CPU_DESCRIPTOR_HANDLE srvs[kMaxTextures12];
    D3D12_CPU_DESCRIPTOR_HANDLE samps[kMaxSamplers12];
    const auto nullSrv = m_pso->NullSrv(D3D12_SRV_DIMENSION_TEXTURE2D);
    const auto defSamp = m_pso->DefaultSampler();
    srvs[0] = srcSrv;
    for (UINT i = 1; i < kMaxTextures12; ++i) srvs[i] = nullSrv;
    samps[0] = linearFilter ? m_linearSampler : m_pointSampler;
    for (UINT i = 1; i < kMaxSamplers12; ++i) samps[i] = defSamp;

    const uint64_t srvKey  = dx12::HashBytes(srvs,  sizeof(srvs));
    const uint64_t sampKey = dx12::HashBytes(samps, sizeof(samps));

    D3D12_GPU_DESCRIPTOR_HANDLE srvTable{}, sampTable{};
    if (!m_ctx->SrvArena().GetOrCreate(m_ctx->Device(), srvKey, srvs, &srvTable) ||
        !m_ctx->SamplerArena().GetOrCreate(m_ctx->Device(), sampKey, samps, &sampTable)) {
        DXLOG_WARN("[dx12] blit descriptor arena is full; the copy is skipped "
                   "for this frame");
        return E_OUTOFMEMORY;
    }

    auto* cmd = m_ctx->CmdList();
    m_ctx->BindDescriptorHeaps();
    cmd->SetGraphicsRootSignature(m_ctx->RootSignature());
    cmd->SetGraphicsRootDescriptorTable(kRP_SRVs, srvTable);
    cmd->SetGraphicsRootDescriptorTable(kRP_Samplers, sampTable);
    cmd->SetPipelineState(pso);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->OMSetRenderTargets(1, &dstRtv, FALSE, nullptr);

    const float srcW = float(std::max(1u, srcWidth));
    const float srcH = float(std::max(1u, srcHeight));
    const float u0 = float(srcRect.left)   / srcW;
    const float v0 = float(srcRect.top)    / srcH;
    const float u1 = float(srcRect.right)  / srcW;
    const float v1 = float(srcRect.bottom) / srcH;
    const float du = (u1 - u0) > 1e-6f ? (u1 - u0) : 1.0f;
    const float dv = (v1 - v0) > 1e-6f ? (v1 - v0) : 1.0f;

    const float dstX = float(dstRect.left);
    const float dstY = float(dstRect.top);
    const float dstW = float(dstRect.right  - dstRect.left);
    const float dstH = float(dstRect.bottom - dstRect.top);

    D3D12_VIEWPORT vp{};
    vp.TopLeftX = dstX - (u0 / du) * dstW;
    vp.TopLeftY = dstY - (v0 / dv) * dstH;
    vp.Width    = dstW / du;
    vp.Height   = dstH / dv;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &vp);

    D3D12_RECT sc{ dstRect.left, dstRect.top, dstRect.right, dstRect.bottom };
    sc.left   = std::clamp<LONG>(sc.left,   0, LONG(dstWidth));
    sc.top    = std::clamp<LONG>(sc.top,    0, LONG(dstHeight));
    sc.right  = std::clamp<LONG>(sc.right,  sc.left, LONG(dstWidth));
    sc.bottom = std::clamp<LONG>(sc.bottom, sc.top,  LONG(dstHeight));
    cmd->RSSetScissorRects(1, &sc);

    cmd->DrawInstanced(3, 1, 0, 0);

    m_ctx->MarkCommandStateDirty();
    return S_OK;
}

HRESULT Blitter12::GenerateMipChain(Resource12& res, DXGI_FORMAT rtvFormat,
                                    UINT width, UINT height, UINT mipLevels,
                                    UINT arraySlice, UINT arraySize) noexcept
{
    if (!res.Valid() || mipLevels <= 1 || !m_ctx) return S_OK;
    ID3D12Resource* resource = res.Native();

    HRESULT hr = EnsureShaders();
    if (FAILED(hr)) return hr;

    auto* dev = m_ctx->Device();
    auto* cmd = m_ctx->CmdList();

    for (UINT level = 1; level < mipLevels; ++level) {
        const UINT srcLevel = level - 1;
        const UINT dstW = std::max(1u, width  >> level);
        const UINT dstH = std::max(1u, height >> level);
        const UINT srcW = std::max(1u, width  >> srcLevel);
        const UINT srcH = std::max(1u, height >> srcLevel);

        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                  = rtvFormat;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (arraySize > 1) {
            sd.ViewDimension                     = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MostDetailedMip    = srcLevel;
            sd.Texture2DArray.MipLevels          = 1;
            sd.Texture2DArray.FirstArraySlice    = arraySlice;
            sd.Texture2DArray.ArraySize          = 1;
        } else {
            sd.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MostDetailedMip     = srcLevel;
            sd.Texture2D.MipLevels           = 1;
        }
        const auto srv = m_ctx->SrvStaging().Alloc();
        if (srv.ptr == SIZE_T(-1)) return E_OUTOFMEMORY;
        dev->CreateShaderResourceView(resource, &sd, srv);

        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = rtvFormat;
        if (arraySize > 1) {
            rd.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.MipSlice        = level;
            rd.Texture2DArray.FirstArraySlice = arraySlice;
            rd.Texture2DArray.ArraySize       = 1;
        } else {
            rd.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
            rd.Texture2D.MipSlice = level;
        }
        const auto rtv = m_ctx->RtvHeap().Alloc();
        if (rtv.ptr == SIZE_T(-1)) { m_ctx->SrvStaging().Free(srv); return E_OUTOFMEMORY; }
        dev->CreateRenderTargetView(resource, &rd, rtv);

        const UINT srcSub = dx12::CalcSubresource(srcLevel, arraySlice, 0, mipLevels, arraySize);
        const UINT dstSub = dx12::CalcSubresource(level,    arraySlice, 0, mipLevels, arraySize);
        std::vector<D3D12_RESOURCE_BARRIER> pre;
        res.AppendTransition(pre, srcSub, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        res.AppendTransition(pre, dstSub, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (!pre.empty())
            cmd->ResourceBarrier(static_cast<UINT>(pre.size()), pre.data());

        const RECT full{ 0, 0, LONG(dstW), LONG(dstH) };
        const RECT srcAll{ 0, 0, LONG(srcW), LONG(srcH) };
        hr = Blit(rtv, rtvFormat, dstW, dstH, full, srv, srcW, srcH, srcAll, true);

        m_ctx->SrvStaging().Free(srv);
        m_ctx->RtvHeap().Free(rtv);
        if (FAILED(hr)) return hr;
    }

    std::vector<D3D12_RESOURCE_BARRIER> post;
    res.AppendTransition(post, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!post.empty())
        cmd->ResourceBarrier(static_cast<UINT>(post.size()), post.data());

    return S_OK;
}

}
