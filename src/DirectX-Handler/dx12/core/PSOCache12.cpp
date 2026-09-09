// Builds and caches D3D12 pipeline state objects.
//
// D3D12 collapses both shaders, the input layout, the blend, rasteriser and
// depth-stencil state, the topology type and the render target formats into a
// single immutable object. Any difference means a different object, and
// creating one is slow enough to stall a frame.
//
// The full description is therefore hashed into a key and the result cached.
// The render target formats are part of that key, which is easy to overlook
// until a render-to-texture pass fails validation for using a pipeline built
// against a different format.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/PSOCache12.h>
#include <core/DeviceContext12.h>
#include <core/Log.h>

#include <algorithm>
#include <cstring>

namespace mwon12 {

D3D12_BLEND TranslateBlend12(D3DBLEND b) noexcept
{
    switch (b) {
    case D3DBLEND_ZERO:            return D3D12_BLEND_ZERO;
    case D3DBLEND_ONE:             return D3D12_BLEND_ONE;
    case D3DBLEND_SRCCOLOR:        return D3D12_BLEND_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:     return D3D12_BLEND_INV_SRC_COLOR;
    case D3DBLEND_SRCALPHA:        return D3D12_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:     return D3D12_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:       return D3D12_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA:    return D3D12_BLEND_INV_DEST_ALPHA;
    case D3DBLEND_DESTCOLOR:       return D3D12_BLEND_DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR:    return D3D12_BLEND_INV_DEST_COLOR;
    case D3DBLEND_SRCALPHASAT:     return D3D12_BLEND_SRC_ALPHA_SAT;

    case D3DBLEND_BOTHSRCALPHA:    return D3D12_BLEND_SRC_ALPHA;
    case D3DBLEND_BOTHINVSRCALPHA: return D3D12_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_BLENDFACTOR:     return D3D12_BLEND_BLEND_FACTOR;
    case D3DBLEND_INVBLENDFACTOR:  return D3D12_BLEND_INV_BLEND_FACTOR;
    case D3DBLEND_SRCCOLOR2:       return D3D12_BLEND_SRC1_COLOR;
    case D3DBLEND_INVSRCCOLOR2:    return D3D12_BLEND_INV_SRC1_COLOR;
    default:                       return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND SanitizeAlphaBlend12(D3D12_BLEND b) noexcept
{
    switch (b) {
    case D3D12_BLEND_SRC_COLOR:      return D3D12_BLEND_SRC_ALPHA;
    case D3D12_BLEND_INV_SRC_COLOR:  return D3D12_BLEND_INV_SRC_ALPHA;
    case D3D12_BLEND_DEST_COLOR:     return D3D12_BLEND_DEST_ALPHA;
    case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
    case D3D12_BLEND_SRC1_COLOR:     return D3D12_BLEND_SRC1_ALPHA;
    case D3D12_BLEND_INV_SRC1_COLOR: return D3D12_BLEND_INV_SRC1_ALPHA;
    default:                         return b;
    }
}

D3D12_BLEND_OP TranslateBlendOp12(D3DBLENDOP op) noexcept
{
    switch (op) {
    case D3DBLENDOP_ADD:         return D3D12_BLEND_OP_ADD;
    case D3DBLENDOP_SUBTRACT:    return D3D12_BLEND_OP_SUBTRACT;
    case D3DBLENDOP_REVSUBTRACT: return D3D12_BLEND_OP_REV_SUBTRACT;
    case D3DBLENDOP_MIN:         return D3D12_BLEND_OP_MIN;
    case D3DBLENDOP_MAX:         return D3D12_BLEND_OP_MAX;
    default:                     return D3D12_BLEND_OP_ADD;
    }
}

D3D12_COMPARISON_FUNC TranslateCmpFunc12(D3DCMPFUNC f) noexcept
{
    switch (f) {
    case D3DCMP_NEVER:        return D3D12_COMPARISON_FUNC_NEVER;
    case D3DCMP_LESS:         return D3D12_COMPARISON_FUNC_LESS;
    case D3DCMP_EQUAL:        return D3D12_COMPARISON_FUNC_EQUAL;
    case D3DCMP_LESSEQUAL:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case D3DCMP_GREATER:      return D3D12_COMPARISON_FUNC_GREATER;
    case D3DCMP_NOTEQUAL:     return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case D3DCMP_GREATEREQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case D3DCMP_ALWAYS:       return D3D12_COMPARISON_FUNC_ALWAYS;
    default:                  return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

D3D12_STENCIL_OP TranslateStencilOp12(D3DSTENCILOP op) noexcept
{
    switch (op) {
    case D3DSTENCILOP_KEEP:    return D3D12_STENCIL_OP_KEEP;
    case D3DSTENCILOP_ZERO:    return D3D12_STENCIL_OP_ZERO;
    case D3DSTENCILOP_REPLACE: return D3D12_STENCIL_OP_REPLACE;
    case D3DSTENCILOP_INCRSAT: return D3D12_STENCIL_OP_INCR_SAT;
    case D3DSTENCILOP_DECRSAT: return D3D12_STENCIL_OP_DECR_SAT;
    case D3DSTENCILOP_INVERT:  return D3D12_STENCIL_OP_INVERT;
    case D3DSTENCILOP_INCR:    return D3D12_STENCIL_OP_INCR;
    case D3DSTENCILOP_DECR:    return D3D12_STENCIL_OP_DECR;
    default:                   return D3D12_STENCIL_OP_KEEP;
    }
}

D3D12_CULL_MODE TranslateCullMode12(D3DCULL c) noexcept
{
    switch (c) {
    case D3DCULL_NONE: return D3D12_CULL_MODE_NONE;
    case D3DCULL_CW:   return D3D12_CULL_MODE_FRONT;
    case D3DCULL_CCW:  return D3D12_CULL_MODE_BACK;
    default:           return D3D12_CULL_MODE_NONE;
    }
}

D3D12_FILL_MODE TranslateFillMode12(D3DFILLMODE f) noexcept
{

    return (f == D3DFILL_WIREFRAME || f == D3DFILL_POINT)
         ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
}

D3D12_TEXTURE_ADDRESS_MODE TranslateAddress12(D3DTEXTUREADDRESS a) noexcept
{
    switch (a) {
    case D3DTADDRESS_WRAP:       return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case D3DTADDRESS_MIRROR:     return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case D3DTADDRESS_CLAMP:      return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case D3DTADDRESS_BORDER:     return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case D3DTADDRESS_MIRRORONCE: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default:                     return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyTypeOf12(D3DPRIMITIVETYPE p) noexcept
{
    switch (p) {
    case D3DPT_POINTLIST:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case D3DPT_LINELIST:
    case D3DPT_LINESTRIP:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case D3DPT_TRIANGLELIST:
    case D3DPT_TRIANGLESTRIP:

    case D3DPT_TRIANGLEFAN:   return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    default:                  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

D3D12_PRIMITIVE_TOPOLOGY TopologyOf12(D3DPRIMITIVETYPE p) noexcept
{
    switch (p) {
    case D3DPT_POINTLIST:     return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case D3DPT_LINELIST:      return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case D3DPT_LINESTRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case D3DPT_TRIANGLELIST:  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_TRIANGLESTRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case D3DPT_TRIANGLEFAN:   return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

UINT VertexCountFor12(D3DPRIMITIVETYPE type, UINT primCount) noexcept
{
    switch (type) {
    case D3DPT_POINTLIST:     return primCount;
    case D3DPT_LINELIST:      return primCount * 2;
    case D3DPT_LINESTRIP:     return primCount + 1;
    case D3DPT_TRIANGLELIST:  return primCount * 3;
    case D3DPT_TRIANGLESTRIP: return primCount + 2;
    case D3DPT_TRIANGLEFAN:   return primCount + 2;
    default:                  return primCount * 3;
    }
}

ID3D12PipelineState* PSOCache12::GetOrCreate(
    const PipelineKey12& key,
    const void* vsDxbc, size_t vsBytes,
    const void* psDxbc, size_t psBytes,
    const D3D12_INPUT_ELEMENT_DESC* inputElements, UINT inputCount) noexcept
{
    if (!m_ctx || !m_ctx->Device() || !vsDxbc || vsBytes == 0)
        return nullptr;

    {
        AcquireSRWLockShared(&m_psoLock);
        if (const auto it = m_pipelines.find(key); it != m_pipelines.end()) {
            auto* p = it->second.Get();
            ReleaseSRWLockShared(&m_psoLock);
            return p;
        }
        ReleaseSRWLockShared(&m_psoLock);
    }

    ++m_misses;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_ctx->RootSignature();
    pd.VS = { vsDxbc, vsBytes };
    if (psDxbc && psBytes) pd.PS = { psDxbc, psBytes };

    pd.BlendState.AlphaToCoverageEnable  = key.alphaToCoverage != 0;
    pd.BlendState.IndependentBlendEnable = key.independentBlend != 0;
    for (UINT i = 0; i < 8; ++i) {
        auto& rt = pd.BlendState.RenderTarget[i];
        const UINT src = (i < kMaxRenderTargets12) ? i : 0;
        rt.BlendEnable           = key.blendEnable != 0;
        rt.LogicOpEnable         = FALSE;
        rt.SrcBlend              = TranslateBlend12(static_cast<D3DBLEND>(key.srcBlend));
        rt.DestBlend             = TranslateBlend12(static_cast<D3DBLEND>(key.destBlend));
        rt.BlendOp               = TranslateBlendOp12(static_cast<D3DBLENDOP>(key.blendOp));
        rt.SrcBlendAlpha         = SanitizeAlphaBlend12(
            TranslateBlend12(static_cast<D3DBLEND>(
                key.separateAlpha ? key.srcBlendAlpha : key.srcBlend)));
        rt.DestBlendAlpha        = SanitizeAlphaBlend12(
            TranslateBlend12(static_cast<D3DBLEND>(
                key.separateAlpha ? key.destBlendAlpha : key.destBlend)));
        rt.BlendOpAlpha          = TranslateBlendOp12(static_cast<D3DBLENDOP>(
            key.separateAlpha ? key.blendOpAlpha : key.blendOp));
        rt.LogicOp               = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = key.colorWrite[src];
    }

    float depthBiasF = 0.0f, slopeBiasF = 0.0f;
    std::memcpy(&depthBiasF, &key.depthBiasBits, sizeof(float));
    std::memcpy(&slopeBiasF, &key.slopeScaleBiasBits, sizeof(float));

    pd.RasterizerState.FillMode              = TranslateFillMode12(
        static_cast<D3DFILLMODE>(key.fillMode));
    pd.RasterizerState.CullMode              = TranslateCullMode12(
        static_cast<D3DCULL>(key.cullMode));
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthBias             = static_cast<INT>(depthBiasF * float(1 << 24));
    pd.RasterizerState.DepthBiasClamp        = 0.0f;
    pd.RasterizerState.SlopeScaledDepthBias  = slopeBiasF;
    pd.RasterizerState.DepthClipEnable       = TRUE;
    pd.RasterizerState.MultisampleEnable     = key.multisampleEnable != 0;
    pd.RasterizerState.AntialiasedLineEnable = key.antialiasedLine != 0;
    pd.RasterizerState.ForcedSampleCount     = 0;
    pd.RasterizerState.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    const bool haveDsv = key.dsvFormat != DXGI_FORMAT_UNKNOWN;
    pd.DepthStencilState.DepthEnable    = haveDsv && key.depthEnable != 0;
    pd.DepthStencilState.DepthWriteMask = (haveDsv && key.depthWrite)
                                        ? D3D12_DEPTH_WRITE_MASK_ALL
                                        : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc      = TranslateCmpFunc12(
        static_cast<D3DCMPFUNC>(key.depthFunc));
    pd.DepthStencilState.StencilEnable    = haveDsv && key.stencilEnable != 0;
    pd.DepthStencilState.StencilReadMask  = key.stencilReadMask;
    pd.DepthStencilState.StencilWriteMask = key.stencilWriteMask;

    pd.DepthStencilState.FrontFace.StencilFailOp      = TranslateStencilOp12(
        static_cast<D3DSTENCILOP>(key.stencilFail));
    pd.DepthStencilState.FrontFace.StencilDepthFailOp = TranslateStencilOp12(
        static_cast<D3DSTENCILOP>(key.stencilZFail));
    pd.DepthStencilState.FrontFace.StencilPassOp      = TranslateStencilOp12(
        static_cast<D3DSTENCILOP>(key.stencilPass));
    pd.DepthStencilState.FrontFace.StencilFunc        = TranslateCmpFunc12(
        static_cast<D3DCMPFUNC>(key.stencilFunc));

    if (key.twoSidedStencil) {
        pd.DepthStencilState.BackFace.StencilFailOp      = TranslateStencilOp12(
            static_cast<D3DSTENCILOP>(key.ccwFail));
        pd.DepthStencilState.BackFace.StencilDepthFailOp = TranslateStencilOp12(
            static_cast<D3DSTENCILOP>(key.ccwZFail));
        pd.DepthStencilState.BackFace.StencilPassOp      = TranslateStencilOp12(
            static_cast<D3DSTENCILOP>(key.ccwPass));
        pd.DepthStencilState.BackFace.StencilFunc        = TranslateCmpFunc12(
            static_cast<D3DCMPFUNC>(key.ccwFunc));
    } else {
        pd.DepthStencilState.BackFace = pd.DepthStencilState.FrontFace;
    }

    pd.InputLayout.pInputElementDescs = inputCount ? inputElements : nullptr;
    pd.InputLayout.NumElements        = inputCount;
    pd.IBStripCutValue      = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pd.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(key.topologyType);
    pd.NumRenderTargets      = key.numRenderTargets;
    for (UINT i = 0; i < kMaxRenderTargets12; ++i)
        pd.RTVFormats[i] = (i < key.numRenderTargets) ? key.rtvFormats[i]
                                                      : DXGI_FORMAT_UNKNOWN;
    pd.DSVFormat        = key.dsvFormat;
    pd.SampleDesc.Count = key.sampleCount ? key.sampleCount : 1;
    pd.SampleDesc.Quality = 0;
    pd.SampleMask       = UINT_MAX;
    pd.NodeMask         = 0;
    pd.Flags            = D3D12_PIPELINE_STATE_FLAG_NONE;

    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = m_ctx->Device()->CreateGraphicsPipelineState(
        &pd, IID_PPV_ARGS(pso.GetAddressOf()));
    if (FAILED(hr)) {
        // A removed device fails every pipeline creation from here on, and each
        // one of those is noise sitting on top of the real cause.
        if (m_ctx->NoteDeviceRemoved(hr, "CreateGraphicsPipelineState"))
            return nullptr;

        DXLOG_HR(hr, "[dx12] CreateGraphicsPipelineState failed | vs=%016llX ps=%016llX "
                     "il=%016llX rt0=%d dsv=%d numRT=%u topo=%u blend=%u "
                     "src=%u dst=%u srcA=%u dstA=%u cw0=0x%X depth=%u/%u/%u sten=%u",
                 (unsigned long long)key.vsHash, (unsigned long long)key.psHash,
                 (unsigned long long)key.inputLayoutHash,
                 (int)key.rtvFormats[0], (int)key.dsvFormat,
                 (unsigned)key.numRenderTargets, (unsigned)key.topologyType,
                 (unsigned)key.blendEnable, (unsigned)key.srcBlend,
                 (unsigned)key.destBlend, (unsigned)key.srcBlendAlpha,
                 (unsigned)key.destBlendAlpha, (unsigned)key.colorWrite[0],
                 (unsigned)key.depthEnable, (unsigned)key.depthWrite,
                 (unsigned)key.depthFunc, (unsigned)key.stencilEnable);
        return nullptr;
    }

    AcquireSRWLockExclusive(&m_psoLock);
    auto [it, inserted] = m_pipelines.emplace(key, std::move(pso));
    auto* result = it->second.Get();
    ReleaseSRWLockExclusive(&m_psoLock);
    return result;
}

D3D12_CPU_DESCRIPTOR_HANDLE PSOCache12::GetOrCreateSampler(const SamplerKey12& key) noexcept
{
    if (!m_ctx || !m_ctx->Device())
        return D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };

    {
        AcquireSRWLockShared(&m_sampLock);
        if (const auto it = m_samplers.find(key); it != m_samplers.end()) {
            const auto h = it->second;
            ReleaseSRWLockShared(&m_sampLock);
            return h;
        }
        ReleaseSRWLockShared(&m_sampLock);
    }

    const bool aniso = (key.minFilter == D3DTEXF_ANISOTROPIC ||
                        key.magFilter == D3DTEXF_ANISOTROPIC) &&
                       key.maxAnisotropy > 1;

    D3D12_FILTER filter;
    if (aniso) {
        filter = D3D12_FILTER_ANISOTROPIC;
    } else {
        const bool minLin = key.minFilter >= D3DTEXF_LINEAR;
        const bool magLin = key.magFilter >= D3DTEXF_LINEAR;
        const bool mipLin = key.mipFilter == D3DTEXF_LINEAR;
        UINT f = 0;
        if (minLin) f |= 0x10;
        if (magLin) f |= 0x4;
        if (mipLin) f |= 0x1;
        filter = static_cast<D3D12_FILTER>(f);
    }

    float lodBias = 0.0f;
    std::memcpy(&lodBias, &key.mipLodBiasBits, sizeof(float));

    D3D12_SAMPLER_DESC sd{};
    sd.Filter         = filter;
    sd.AddressU       = TranslateAddress12(static_cast<D3DTEXTUREADDRESS>(key.addressU));
    sd.AddressV       = TranslateAddress12(static_cast<D3DTEXTUREADDRESS>(key.addressV));
    sd.AddressW       = TranslateAddress12(static_cast<D3DTEXTUREADDRESS>(key.addressW));
    sd.MipLODBias     = lodBias;
    sd.MaxAnisotropy  = std::clamp<UINT>(key.maxAnisotropy, 1, 16);
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;

    sd.MinLOD         = static_cast<float>(key.maxMipLevel);

    sd.MaxLOD         = (key.mipFilter == D3DTEXF_NONE)
                      ? static_cast<float>(key.maxMipLevel)
                      : D3D12_FLOAT32_MAX;

    const D3DCOLOR bc = key.borderColor;
    sd.BorderColor[0] = ((bc >> 16) & 0xFF) / 255.0f;
    sd.BorderColor[1] = ((bc >>  8) & 0xFF) / 255.0f;
    sd.BorderColor[2] = ((bc      ) & 0xFF) / 255.0f;
    sd.BorderColor[3] = ((bc >> 24) & 0xFF) / 255.0f;

    const auto h = m_ctx->SamplerStaging().Alloc();
    if (h.ptr == SIZE_T(-1)) {
        DXLOG_ERROR("[dx12] sampler staging heap exhausted (%zu unique states)",
                    m_samplers.size());
        return DefaultSampler();
    }
    m_ctx->Device()->CreateSampler(&sd, h);

    AcquireSRWLockExclusive(&m_sampLock);
    auto [it, inserted] = m_samplers.emplace(key, h);
    const auto result = it->second;
    ReleaseSRWLockExclusive(&m_sampLock);

    // Lost the race: another thread created a sampler for this key while this
    // one was building the description, so the map kept theirs and `h` is
    // referenced by nothing. Giving it back matters -- the sampler staging
    // heap is fixed-size, and a descriptor stranded here is stranded for the
    // life of the device. The failure it eventually produces is the
    // "sampler staging heap exhausted" above, at which point every subsequent
    // sampler silently becomes the default one and textures start wrapping
    // and filtering wrongly, somewhere far from here.
    if (!inserted) m_ctx->SamplerStaging().Free(h);

    return result;
}

D3D12_CPU_DESCRIPTOR_HANDLE PSOCache12::NullSrv(D3D12_SRV_DIMENSION dim) noexcept
{
    if (!m_ctx || !m_ctx->Device())
        return D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };

    std::atomic<SIZE_T>* slot = &m_nullSrv2D;
    if (dim == D3D12_SRV_DIMENSION_TEXTURECUBE) slot = &m_nullSrvCube;
    else if (dim == D3D12_SRV_DIMENSION_TEXTURE3D) slot = &m_nullSrv3D;

    if (const SIZE_T have = slot->load(std::memory_order_acquire);
        have != SIZE_T(-1))
        return D3D12_CPU_DESCRIPTOR_HANDLE{ have };

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension           = dim;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (dim) {
    case D3D12_SRV_DIMENSION_TEXTURECUBE: sd.TextureCube.MipLevels = 1; break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:   sd.Texture3D.MipLevels   = 1; break;
    default:                              sd.Texture2D.MipLevels   = 1; break;
    }

    const auto h = m_ctx->SrvStaging().Alloc();
    if (h.ptr == SIZE_T(-1)) return h;

    m_ctx->Device()->CreateShaderResourceView(nullptr, &sd, h);

    // Published only if this thread is the one that got here first. The view
    // is created before the exchange, so any thread that reads a non-sentinel
    // value reads a descriptor that is already complete.
    SIZE_T expected = SIZE_T(-1);
    if (!slot->compare_exchange_strong(expected, h.ptr,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        m_ctx->SrvStaging().Free(h);
        return D3D12_CPU_DESCRIPTOR_HANDLE{ expected };
    }
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE PSOCache12::DefaultSampler() noexcept
{
    if (const SIZE_T have = m_defaultSampler.load(std::memory_order_acquire);
        have != SIZE_T(-1))
        return D3D12_CPU_DESCRIPTOR_HANDLE{ have };
    if (!m_ctx || !m_ctx->Device())
        return D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };

    D3D12_SAMPLER_DESC sd{};
    sd.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sd.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sd.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sd.MaxAnisotropy  = 1;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.MinLOD         = 0.0f;
    sd.MaxLOD         = D3D12_FLOAT32_MAX;

    const auto h = m_ctx->SamplerStaging().Alloc();
    if (h.ptr == SIZE_T(-1)) return h;
    m_ctx->Device()->CreateSampler(&sd, h);

    SIZE_T expected = SIZE_T(-1);
    if (!m_defaultSampler.compare_exchange_strong(expected, h.ptr,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
        m_ctx->SamplerStaging().Free(h);
        return D3D12_CPU_DESCRIPTOR_HANDLE{ expected };
    }
    return h;
}

void PSOCache12::Clear() noexcept
{
    AcquireSRWLockExclusive(&m_psoLock);
    m_pipelines.clear();
    ReleaseSRWLockExclusive(&m_psoLock);

    // The descriptors go back too, and the cached handles are reset to their
    // constructed value.
    //
    // Without this, Clear left m_samplers full of handles into a staging heap
    // that is about to be destroyed with the context -- and a
    // GetOrCreateSampler after a Clear would find one and hand it out. The
    // heap dies with the device either way, so nothing was leaking across the
    // process; what was wrong is that "cleared" did not mean cleared.
    AcquireSRWLockExclusive(&m_sampLock);

    // Exchanged rather than read-then-reset, so a descriptor is freed exactly
    // once even if this ran alongside a thread still in NullSrv.
    auto take = [](std::atomic<SIZE_T>& slot, CpuDescriptorHeap* heap) {
        const SIZE_T h = slot.exchange(SIZE_T(-1), std::memory_order_acq_rel);
        if (h != SIZE_T(-1) && heap) heap->Free(D3D12_CPU_DESCRIPTOR_HANDLE{ h });
    };

    if (m_ctx) {
        auto& samplers = m_ctx->SamplerStaging();
        for (const auto& [key, h] : m_samplers)
            if (h.ptr != SIZE_T(-1)) samplers.Free(h);
        take(m_defaultSampler, &samplers);
        take(m_nullSrv2D,   &m_ctx->SrvStaging());
        take(m_nullSrvCube, &m_ctx->SrvStaging());
        take(m_nullSrv3D,   &m_ctx->SrvStaging());
    } else {
        take(m_defaultSampler, nullptr);
        take(m_nullSrv2D,   nullptr);
        take(m_nullSrvCube, nullptr);
        take(m_nullSrv3D,   nullptr);
    }
    m_samplers.clear();
    ReleaseSRWLockExclusive(&m_sampLock);
}

}
