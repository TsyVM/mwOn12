
// The D3D12 draw path: from the game calling DrawPrimitive to work being
// recorded into a command list.
//
// PreDrawFlush resolves the whole pipeline once per draw, in a fixed order:
// select or build the pipeline state object, update and bind constants,
// populate descriptor tables for the bound textures, bind vertex and index
// buffers, emit any resource barriers the draw requires, then set the
// remaining dynamic state. Only after all of that is the draw recorded.
//
// D3D12 makes several things the caller's responsibility that D3D11 handled
// internally, and each one shapes this file:
//
//   Nothing executes when it is called. Everything is recorded and submitted
//   later, so any resource a recorded draw touches has to stay alive and
//   unmodified until the GPU has finished with it -- not merely until the call
//   returns.
//
//   Resource transitions are explicit. Using a resource in a state it is not
//   currently in is undefined rather than an error, so barriers are collected
//   and submitted in batches; each batch is a pipeline stall, so issuing them
//   individually is expensive.
//
//   Textures are not bound. Descriptors are copied into a shader-visible heap
//   and a table is bound. Doing that per draw is too slow, so the tables are
//   cached.
//
// The rule that matters most here: never discard a draw silently. A guard in
// this file that rejected work and returned success once cost an entire class
// of the game's rendering, with no error and no log line anywhere to show for
// it. Anything that drops a draw must say so.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy12/D9Device12.h>
#include <core/BackendSelect.h>
#include <core/FormatConverter.h>
#include <core/Log.h>

#include <algorithm>
#include <cstring>

namespace mwon12 {

namespace {

constexpr D3D12_RESOURCE_STATES kShaderRead12 =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

TextureStorage12* StorageOf(IDirect3DBaseTexture9* t) noexcept
{
    if (!t) return nullptr;
    switch (t->GetType()) {
    case D3DRTYPE_TEXTURE:       return static_cast<D9Texture12*>(t)->Storage();
    case D3DRTYPE_CUBETEXTURE:   return static_cast<D9CubeTexture12*>(t)->Storage();
    case D3DRTYPE_VOLUMETEXTURE: return static_cast<D9VolumeTexture12*>(t)->Storage();
    default:                     return nullptr;
    }
}

D3D12_SRV_DIMENSION SrvDimOf(IDirect3DBaseTexture9* t) noexcept
{
    if (!t) return D3D12_SRV_DIMENSION_TEXTURE2D;
    switch (t->GetType()) {
    case D3DRTYPE_CUBETEXTURE:   return D3D12_SRV_DIMENSION_TEXTURECUBE;
    case D3DRTYPE_VOLUMETEXTURE: return D3D12_SRV_DIMENSION_TEXTURE3D;
    default:                     return D3D12_SRV_DIMENSION_TEXTURE2D;
    }
}

}

const D3DVERTEXELEMENT9* D9Device12::EffectiveDeclaration() noexcept
{
    if (m_decl && !m_decl->Elements().empty())
        return m_decl->Elements().data();

    if (m_fvfDeclCached != m_fvf) {
        ExpandFVF(m_fvf, m_fvfDecl);
        m_fvfDeclCached = m_fvf;
    }
    return m_fvfDecl;
}

HRESULT D9Device12::ResolveShaders(const CompiledShader12** outVS,
                                   const CompiledShader12** outPS) noexcept
{
    *outVS = nullptr;
    *outPS = nullptr;
    const auto& rs = m_rst.State();

    const uint32_t atestFunc = rs.rs[D3DRS_ALPHATESTENABLE]
                             ? (rs.rs[D3DRS_ALPHAFUNC] & 0xFu) : 0u;
    uint32_t fogMode = 0u;
    if (rs.rs[D3DRS_FOGENABLE]) {
        const uint32_t tbl = rs.rs[D3DRS_FOGTABLEMODE]  & 3u;
        const uint32_t vtx = rs.rs[D3DRS_FOGVERTEXMODE] & 3u;
        fogMode = tbl ? tbl : (vtx ? 4u : 0u);
    }

    uint32_t texKindMask = 0;
    for (DWORD s = 0; s < 8; ++s) {
        if (!rs.textures[s]) continue;
        const D3DRESOURCETYPE t = rs.textures[s]->GetType();
        uint32_t kind = 0;
        if (t == D3DRTYPE_CUBETEXTURE)        kind = 1;
        else if (t == D3DRTYPE_VOLUMETEXTURE) kind = 2;
        if (kind) texKindMask |= (kind << (s * 2));
    }

    HRESULT hr = D3D_OK;
    if (m_vs) {
        hr = m_shaders.GetOrCompile(m_vs->Bytecode(), m_vs->ByteSize(), true,
                                    0, 0, 0, outVS);
        if (FAILED(hr)) return hr;
    }
    if (m_ps) {
        hr = m_shaders.GetOrCompile(m_ps->Bytecode(), m_ps->ByteSize(), false,
                                    atestFunc, fogMode, texKindMask, outPS);
        if (FAILED(hr)) return hr;
    }
    if (*outVS && *outPS)
        return D3D_OK;

    FFPPermKey key = BuildFFPKey(rs, m_fixed, m_fvf, EffectiveDeclaration());
    if (!*outVS && *outPS) {

        key.mixedFullTexOutputs = 1;
    } else if (*outVS && !*outPS) {

        key.mixedPsLink = 1;
        key.linkTexMask = (*outVS)->outTexMask & 0xFFu;
        key.linkColor0  = (*outVS)->outColor0 ? 1 : 0;
        key.linkColor1  = (*outVS)->outColor1 ? 1 : 0;
        key.linkFog     = (*outVS)->outFog    ? 1 : 0;
    }

    const FFPProgram12* prog = nullptr;
    hr = m_ffp.GetOrCompile(key, &prog);
    if (FAILED(hr) || !prog) return hr == S_OK ? E_FAIL : hr;

    if (!*outVS) *outVS = &prog->vs;
    if (!*outPS) *outPS = &prog->ps;
    return D3D_OK;
}

void D9Device12::BuildPipelineKey(PipelineKey12& key, D3DPRIMITIVETYPE primType,
                                  const CompiledShader12& vs,
                                  const CompiledShader12* ps) noexcept
{
    key = PipelineKey12{};
    const auto& rs = m_rst.State().rs;

    key.vsHash = dx12::HashBytes(vs.dxbc.data(), vs.dxbc.size());
    key.psHash = (ps && !ps->dxbc.empty())
               ? dx12::HashBytes(ps->dxbc.data(), ps->dxbc.size()) : 0;
    key.inputLayoutHash = m_decl ? m_decl->Hash()
                                 : (0x9E3779B9ull ^ uint64_t(m_fvf));

    key.blendEnable = rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0;
    key.srcBlend    = uint8_t(rs[D3DRS_SRCBLEND]);
    key.destBlend   = uint8_t(rs[D3DRS_DESTBLEND]);
    key.blendOp     = uint8_t(rs[D3DRS_BLENDOP]);

    if (rs[D3DRS_SRCBLEND] == D3DBLEND_BOTHSRCALPHA) {
        key.srcBlend  = uint8_t(D3DBLEND_SRCALPHA);
        key.destBlend = uint8_t(D3DBLEND_INVSRCALPHA);
    } else if (rs[D3DRS_SRCBLEND] == D3DBLEND_BOTHINVSRCALPHA) {
        key.srcBlend  = uint8_t(D3DBLEND_INVSRCALPHA);
        key.destBlend = uint8_t(D3DBLEND_SRCALPHA);
    }

    key.separateAlpha  = rs[D3DRS_SEPARATEALPHABLENDENABLE] ? 1 : 0;
    key.srcBlendAlpha  = uint8_t(rs[D3DRS_SRCBLENDALPHA]);
    key.destBlendAlpha = uint8_t(rs[D3DRS_DESTBLENDALPHA]);
    key.blendOpAlpha   = uint8_t(rs[D3DRS_BLENDOPALPHA]);

    key.colorWrite[0] = uint8_t(rs[D3DRS_COLORWRITEENABLE]  & 0xF);
    key.colorWrite[1] = uint8_t(rs[D3DRS_COLORWRITEENABLE1] & 0xF);
    key.colorWrite[2] = uint8_t(rs[D3DRS_COLORWRITEENABLE2] & 0xF);
    key.colorWrite[3] = uint8_t(rs[D3DRS_COLORWRITEENABLE3] & 0xF);
    key.independentBlend =
        (key.colorWrite[1] != key.colorWrite[0] ||
         key.colorWrite[2] != key.colorWrite[0] ||
         key.colorWrite[3] != key.colorWrite[0]) ? 1 : 0;

    key.fillMode           = uint8_t(rs[D3DRS_FILLMODE]);
    key.cullMode           = uint8_t(rs[D3DRS_CULLMODE]);
    key.multisampleEnable  = rs[D3DRS_MULTISAMPLEANTIALIAS] ? 1 : 0;
    key.antialiasedLine    = rs[D3DRS_ANTIALIASEDLINEENABLE] ? 1 : 0;
    key.depthBiasBits      = rs[D3DRS_DEPTHBIAS];
    key.slopeScaleBiasBits = rs[D3DRS_SLOPESCALEDEPTHBIAS];

    key.depthEnable      = (static_cast<D3DZBUFFERTYPE>(rs[D3DRS_ZENABLE]) != D3DZB_FALSE) ? 1 : 0;
    key.depthWrite       = rs[D3DRS_ZWRITEENABLE] ? 1 : 0;
    key.depthFunc        = uint8_t(rs[D3DRS_ZFUNC]);
    key.stencilEnable    = rs[D3DRS_STENCILENABLE] ? 1 : 0;
    key.stencilReadMask  = uint8_t(rs[D3DRS_STENCILMASK] & 0xFF);
    key.stencilWriteMask = uint8_t(rs[D3DRS_STENCILWRITEMASK] & 0xFF);
    key.stencilFail      = uint8_t(rs[D3DRS_STENCILFAIL]);
    key.stencilZFail     = uint8_t(rs[D3DRS_STENCILZFAIL]);
    key.stencilPass      = uint8_t(rs[D3DRS_STENCILPASS]);
    key.stencilFunc      = uint8_t(rs[D3DRS_STENCILFUNC]);
    key.twoSidedStencil  = rs[D3DRS_TWOSIDEDSTENCILMODE] ? 1 : 0;
    key.ccwFail          = uint8_t(rs[D3DRS_CCW_STENCILFAIL]);
    key.ccwZFail         = uint8_t(rs[D3DRS_CCW_STENCILZFAIL]);
    key.ccwPass          = uint8_t(rs[D3DRS_CCW_STENCILPASS]);
    key.ccwFunc          = uint8_t(rs[D3DRS_CCW_STENCILFUNC]);

    const bool srgbWrite = rs[D3DRS_SRGBWRITEENABLE] != 0;
    UINT rtCount = 0;
    for (UINT i = 0; i < kMaxRenderTargets12; ++i) {
        if (!m_renderTargets[i]) continue;
        DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
        if (m_renderTargets[i]->IsBackBuffer()) {
            f = m_ctx->BackBufferFormat();
        } else if (auto* s = m_renderTargets[i]->Storage()) {
            f = s->DxgiResourceFormat();
            if (srgbWrite) {
                const DXGI_FORMAT srgb = FormatConverter::ToSRGBView(f);
                if (srgb != DXGI_FORMAT_UNKNOWN) f = srgb;
            } else {
                f = FormatConverter::ToLinearView(f);
            }
        }
        key.rtvFormats[i] = f;
        rtCount = i + 1;
    }
    key.numRenderTargets = uint8_t(rtCount);

    key.dsvFormat = DXGI_FORMAT_UNKNOWN;
    if (m_depthStencil) {
        if (auto* s = m_depthStencil->Storage()) {
            FormatConverter::DepthFormatViews dv{};
            if (FormatConverter::GetDepthFormatViews(s->Format(), dv))
                key.dsvFormat = dv.dsv;
        }
    }

    key.topologyType = uint8_t(TopologyTypeOf12(primType));
    key.sampleCount  = 1;
}

// Whether this draw can modify the depth-stencil buffer at all.
//
// A stencil op left at KEEP on every face cannot change the buffer, so an
// "enabled" stencil that only KEEPs is not a write. Getting this wrong in
// either direction is caught by the debug layer: claiming writes when there
// are none forces the buffer out of a state a shader could read it in, and
// claiming none when there are is rejected at the draw.
bool D9Device12::DepthStencilWritesEnabled() const noexcept
{
    const auto& rs = m_rst.State().rs;
    if (rs[D3DRS_ZWRITEENABLE]) return true;
    if (!rs[D3DRS_STENCILENABLE]) return false;

    auto keeps = [&](D3DRENDERSTATETYPE op) noexcept {
        return static_cast<D3DSTENCILOP>(rs[op]) == D3DSTENCILOP_KEEP;
    };
    if (!keeps(D3DRS_STENCILFAIL) || !keeps(D3DRS_STENCILZFAIL) ||
        !keeps(D3DRS_STENCILPASS))
        return true;
    if (rs[D3DRS_TWOSIDEDSTENCILMODE] &&
        (!keeps(D3DRS_CCW_STENCILFAIL) || !keeps(D3DRS_CCW_STENCILZFAIL) ||
         !keeps(D3DRS_CCW_STENCILPASS)))
        return true;
    return false;
}

// The resource state a read-only depth bind wants is the combined read state
// DEPTH_READ | shader-read, which is what makes it legal for the same texture
// to be sampled while it is bound for depth testing. That is only available on
// a resource that was created shader-readable; one carrying
// DENY_SHADER_RESOURCE must stay at plain DEPTH_READ.
D3D12_RESOURCE_STATES D9Device12::DepthStateFor(bool readOnly) noexcept
{
    if (!readOnly) return D3D12_RESOURCE_STATE_DEPTH_WRITE;

    D3D12_RESOURCE_STATES s = D3D12_RESOURCE_STATE_DEPTH_READ;
    TextureStorage12* st = m_depthStencil ? m_depthStencil->Storage() : nullptr;
    if (st && st->HasGpu() &&
        !(st->Gpu().Desc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        s |= kShaderRead12;
    return s;
}

void D9Device12::ApplyRenderTargets(std::vector<D3D12_RESOURCE_BARRIER>& barriers) noexcept
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kMaxRenderTargets12]{};
    UINT rtCount = 0;
    const bool srgbWrite = m_rst.State().rs[D3DRS_SRGBWRITEENABLE] != 0;

    for (UINT i = 0; i < kMaxRenderTargets12; ++i) {
        if (!m_renderTargets[i]) continue;

        const auto rtv = m_renderTargets[i]->Rtv(srgbWrite);
        if (rtv.ptr == SIZE_T(-1)) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                DXLOG_WARN("[dx12] render target %u (%ux%u fmt=%d) has no usable RTV "
                           "- the slot is left unbound for this draw",
                           i, m_renderTargets[i]->Width(), m_renderTargets[i]->Height(),
                           (int)m_renderTargets[i]->Format());
            }
            continue;
        }
        m_renderTargets[i]->AppendTransition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtvs[i] = rtv;
        rtCount = i + 1;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsv{ SIZE_T(-1) };
    if (m_depthStencil) {
        const bool readOnly = !DepthStencilWritesEnabled();
        m_depthStencil->AppendTransition(barriers, DepthStateFor(readOnly));
        dsv = m_depthStencil->Dsv(readOnly);
        m_dsvReadOnlyBound = readOnly;
    }

    if (!barriers.empty()) {
        m_ctx->CmdList()->ResourceBarrier(static_cast<UINT>(barriers.size()),
                                          barriers.data());
        barriers.clear();
    }

    m_ctx->CmdList()->OMSetRenderTargets(
        rtCount, rtCount ? rtvs : nullptr, FALSE,
        dsv.ptr != SIZE_T(-1) ? &dsv : nullptr);
}

HRESULT D9Device12::UploadConstants() noexcept
{

    const UINT64 frame = m_ctx->FrameCounter();
    if (frame != m_constFrame) {
        m_constFrame   = frame;
        m_vsConstDirty = m_psConstDirty = m_emuDirty = m_ffpConstDirty = true;
    }

    auto upload = [&](const void* src, size_t bytes,
                      D3D12_GPU_VIRTUAL_ADDRESS& gpu) -> bool {
        auto a = m_ctx->AllocUpload(dx12::AlignUp<UINT64>(bytes, dx12::kCBAlign),
                                    dx12::kCBAlign);
        if (!a.Valid()) return false;
        std::memcpy(a.cpu, src, bytes);
        gpu = a.gpu;
        return true;
    };

    if (m_vsConstDirty) {
        if (!upload(&m_vsConst, sizeof(m_vsConst), m_vsConstGpu)) return E_OUTOFMEMORY;
        m_vsConstDirty = false;
    }
    if (m_psConstDirty) {
        if (!upload(&m_psConst, sizeof(m_psConst), m_psConstGpu)) return E_OUTOFMEMORY;
        m_psConstDirty = false;
    }
    if (m_emuDirty) {

        const auto& rs = m_rst.State().rs;
        std::memcpy(&m_emu.g_fogParams[0], &rs[D3DRS_FOGSTART],   4);
        std::memcpy(&m_emu.g_fogParams[1], &rs[D3DRS_FOGEND],     4);
        std::memcpy(&m_emu.g_fogParams[2], &rs[D3DRS_FOGDENSITY], 4);
        m_emu.g_fogParams[3] = 0.0f;
        const DWORD fc = rs[D3DRS_FOGCOLOR];
        m_emu.g_fogColor[0] = ((fc >> 16) & 0xFF) / 255.0f;
        m_emu.g_fogColor[1] = ((fc >>  8) & 0xFF) / 255.0f;
        m_emu.g_fogColor[2] = ((fc      ) & 0xFF) / 255.0f;
        m_emu.g_fogColor[3] = ((fc >> 24) & 0xFF) / 255.0f;

        const DWORD enabled = rs[D3DRS_CLIPPLANEENABLE];
        for (int i = 0; i < 6; ++i) {
            if (enabled & (1u << i))
                std::memcpy(m_emu.g_clipPlanes[i], m_clipPlanes[i], sizeof(float) * 4);
            else
                std::memset(m_emu.g_clipPlanes[i], 0, sizeof(float) * 4);
        }

        if (!upload(&m_emu, sizeof(m_emu), m_emuGpu)) return E_OUTOFMEMORY;
        m_emuDirty = false;
    }
    if (m_ffpConstDirty) {
        FillFFPConstants(m_ffpConst, m_rst.State(), m_fixed);
        if (!upload(&m_ffpConst, sizeof(m_ffpConst), m_ffpGpu)) return E_OUTOFMEMORY;
        m_ffpConstDirty = false;
    }
    return D3D_OK;
}

HRESULT D9Device12::ResolveDescriptorTables(D3D12_GPU_DESCRIPTOR_HANDLE* outSrv,
                                            D3D12_GPU_DESCRIPTOR_HANDLE* outSampler) noexcept
{
    const auto& st = m_rst.State();

    D3D12_CPU_DESCRIPTOR_HANDLE srvs[kMaxTextures12];
    D3D12_CPU_DESCRIPTOR_HANDLE samps[kMaxSamplers12];
    const auto defSamp = m_psoCache.DefaultSampler();

    for (UINT i = 0; i < kMaxTextures12; ++i)
        srvs[i] = m_psoCache.NullSrv(D3D12_SRV_DIMENSION_TEXTURE2D);
    for (UINT i = 0; i < kMaxSamplers12; ++i)
        samps[i] = defSamp;

    for (DWORD s = 0; s < 8; ++s) {
        IDirect3DBaseTexture9* tex = st.textures[s];
        if (!tex) continue;

        TextureStorage12* storage = StorageOf(tex);
        if (!storage || !storage->HasGpu()) continue;

        bool aliasesTarget = false;
        for (auto* rt : m_renderTargets) {
            if (rt && rt->Gpu() && rt->Gpu()->Native() == storage->Gpu().Native()) {
                aliasesTarget = true;
                break;
            }
        }
        if (aliasesTarget) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                DXLOG_WARN("[dx12] a texture bound at stage %lu is also a bound "
                           "render target; the sampler reads zero for that draw "
                           "(D3D9 left this undefined)", s);
            }
            continue;
        }

        storage->FlushDirty();

        storage->Gpu().AppendTransition(m_barrierScratch,
                                        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                        kShaderRead12);

        const bool srgbRead = st.samp[s][D3DSAMP_SRGBTEXTURE] != 0;
        const auto srv = storage->Srv(srgbRead);
        if (srv.ptr != SIZE_T(-1))
            srvs[s] = srv;
        else
            srvs[s] = m_psoCache.NullSrv(SrvDimOf(tex));

        SamplerKey12 sk{};
        sk.minFilter     = uint8_t(st.samp[s][D3DSAMP_MINFILTER]);
        sk.magFilter     = uint8_t(st.samp[s][D3DSAMP_MAGFILTER]);
        sk.mipFilter     = uint8_t(st.samp[s][D3DSAMP_MIPFILTER]);
        sk.addressU      = uint8_t(st.samp[s][D3DSAMP_ADDRESSU]);
        sk.addressV      = uint8_t(st.samp[s][D3DSAMP_ADDRESSV]);
        sk.addressW      = uint8_t(st.samp[s][D3DSAMP_ADDRESSW]);
        sk.maxAnisotropy = uint8_t(std::clamp<DWORD>(st.samp[s][D3DSAMP_MAXANISOTROPY], 1, 16));
        sk.maxMipLevel   = uint8_t(std::min<DWORD>(st.samp[s][D3DSAMP_MAXMIPLEVEL], 15));
        sk.mipLodBiasBits = st.samp[s][D3DSAMP_MIPMAPLODBIAS];
        sk.borderColor    = st.samp[s][D3DSAMP_BORDERCOLOR];

        const auto samp = m_psoCache.GetOrCreateSampler(sk);
        if (samp.ptr != SIZE_T(-1))
            samps[s] = samp;
    }

    struct SlotId { SIZE_T ptr; uint32_t gen; uint32_t pad; };
    SlotId srvIds[kMaxTextures12]{};
    SlotId sampIds[kMaxSamplers12]{};
    for (UINT i = 0; i < kMaxTextures12; ++i) {
        srvIds[i].ptr = srvs[i].ptr;
        srvIds[i].gen = m_ctx->SrvStaging().GenerationOf(srvs[i]);
    }
    for (UINT i = 0; i < kMaxSamplers12; ++i) {
        sampIds[i].ptr = samps[i].ptr;
        sampIds[i].gen = m_ctx->SamplerStaging().GenerationOf(samps[i]);
    }

    const uint64_t srvKey  = dx12::HashBytes(srvIds,  sizeof(srvIds));
    const uint64_t sampKey = dx12::HashBytes(sampIds, sizeof(sampIds));

    if (!m_ctx->SrvArena().GetOrCreate(m_ctx->Device(), srvKey, srvs, outSrv) ||
        !m_ctx->SamplerArena().GetOrCreate(m_ctx->Device(), sampKey, samps, outSampler)) {

        DXLOG_WARN("[dx12] descriptor arena full mid-frame; draw skipped "
                   "(recycles at the next Present)");
        return E_OUTOFMEMORY;
    }
    return D3D_OK;
}

namespace {

void NoteDrawOutsideScene(bool inScene) noexcept
{
    if (inScene) return;
    static bool s_warned = false;
    if (s_warned) return;
    s_warned = true;
    DXLOG_WARN("[dx12] draw issued outside BeginScene/EndScene - honouring it, "
               "as the retail D3D9 runtime does (MW's front end relies on this)");
}

void NoteDrawDropped(const char* reason) noexcept
{
    static const char* s_seen[16]{};
    static unsigned    s_count = 0;
    for (unsigned i = 0; i < s_count; ++i)
        if (s_seen[i] == reason) return;
    if (s_count < 16) s_seen[s_count++] = reason;
    DXLOG_WARN("[dx12] DRAW DROPPED: %s (first occurrence; this geometry does "
               "not reach the screen)", reason);
}
}

const D9Device12::InputLayout12*
D9Device12::ResolveInputLayout(const CompiledShader12& vs) noexcept
{

    const uint64_t declId = m_decl ? m_decl->Hash()
                                   : (0x9E3779B97F4A7C15ull ^ uint64_t(m_fvf));
    const uint64_t key = declId * 0xC2B2AE3D27D4EB4Full
                       ^ reinterpret_cast<uintptr_t>(&vs);

    if (const auto it = m_inputLayouts.find(key); it != m_inputLayouts.end())
        return it->second.get();

    auto entry = std::make_unique<InputLayout12>();
    const HRESULT hr = BuildInputLayout12(EffectiveDeclaration(), vs,
                                          entry->elems, entry->names);
    if (FAILED(hr)) return nullptr;

    auto* raw = entry.get();
    m_inputLayouts.emplace(key, std::move(entry));
    return raw;
}

HRESULT D9Device12::PreDrawFlush(D3DPRIMITIVETYPE primType, bool indexed) noexcept
{
    if (!m_ctx || !m_ctx->CmdList()) return D3DERR_INVALIDCALL;
    auto* cmd = m_ctx->CmdList();

    if (m_ctx->CommandStateDirty()) {
        m_ctx->BindDescriptorHeaps();
        cmd->SetGraphicsRootSignature(m_ctx->RootSignature());
        m_ctx->ClearCommandStateDirty();
        m_boundPso          = nullptr;
        m_boundTopology     = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        m_boundSrvTable     = D3D12_GPU_DESCRIPTOR_HANDLE{};
        m_boundSamplerTable = D3D12_GPU_DESCRIPTOR_HANDLE{};
        m_boundStencilRef   = UINT(-1);
        m_boundBlendFactor  = 0xFFFFFFFF;
        m_targetsDirty      = true;
        m_constFrame        = UINT64(-1);
    }

    const CompiledShader12* vs = nullptr;
    const CompiledShader12* ps = nullptr;
    HRESULT hr = ResolveShaders(&vs, &ps);
    if (FAILED(hr) || !vs) {
        NoteDrawDropped(FAILED(hr) ? "ResolveShaders failed"
                                   : "ResolveShaders produced no vertex shader");
        return FAILED(hr) ? hr : E_FAIL;
    }

    const InputLayout12* layout = ResolveInputLayout(*vs);
    if (!layout) {
        NoteDrawDropped("BuildInputLayout12 failed - the vertex declaration and "
                        "the shader's input signature could not be reconciled");
        return E_FAIL;
    }

    PipelineKey12 key;
    BuildPipelineKey(key, primType, *vs, ps);

    ID3D12PipelineState* pso = m_psoCache.GetOrCreate(
        key, vs->dxbc.data(), vs->dxbc.size(),
        ps ? ps->dxbc.data() : nullptr, ps ? ps->dxbc.size() : 0,
        layout->elems.data(), static_cast<UINT>(layout->elems.size()));
    if (!pso) {
        NoteDrawDropped("PSOCache12::GetOrCreate returned no pipeline");
        return E_FAIL;
    }

    hr = UploadConstants();
    if (FAILED(hr)) {
        NoteDrawDropped("UploadConstants failed - constant upload ring exhausted");
        return hr;
    }

    m_barrierScratch.clear();

    D3D12_GPU_DESCRIPTOR_HANDLE srvTable{}, sampTable{};
    hr = ResolveDescriptorTables(&srvTable, &sampTable);
    if (FAILED(hr)) {
        NoteDrawDropped("ResolveDescriptorTables failed - SRV/sampler descriptor "
                        "arena could not supply a table");
        return hr;
    }

    // The depth-stencil state a draw needs follows D3DRS_ZWRITEENABLE and the
    // stencil ops, which change between draws without the render targets
    // changing. Deciding it once when the targets were last bound left the
    // buffer in DEPTH_READ for draws that write depth, which the debug layer
    // rejects at the draw itself. Binding the depth texture as a shader
    // resource also moves it out from under the DSV. Both are re-checked here,
    // every draw.
    if (m_depthStencil) {
        const bool readOnly = !DepthStencilWritesEnabled();
        if (readOnly != m_dsvReadOnlyBound)
            m_targetsDirty = true;
        else
            m_depthStencil->AppendTransition(m_barrierScratch, DepthStateFor(readOnly));
    }

    if (m_targetsDirty) {
        ApplyRenderTargets(m_barrierScratch);
        m_targetsDirty = false;
    } else if (!m_barrierScratch.empty()) {
        cmd->ResourceBarrier(static_cast<UINT>(m_barrierScratch.size()),
                             m_barrierScratch.data());
        m_barrierScratch.clear();
    }

    ApplyViewportScissor();

    if (m_boundPso != pso) {
        cmd->SetPipelineState(pso);
        m_boundPso = pso;
    }

    cmd->SetGraphicsRootConstantBufferView(kRP_VSConst, m_vsConstGpu);
    cmd->SetGraphicsRootConstantBufferView(kRP_PSConst, m_psConstGpu);
    cmd->SetGraphicsRootConstantBufferView(kRP_VSEmu, m_vs ? m_emuGpu : m_ffpGpu);
    cmd->SetGraphicsRootConstantBufferView(kRP_PSEmu, m_ps ? m_emuGpu : m_ffpGpu);

    if (m_boundSrvTable.ptr != srvTable.ptr) {
        cmd->SetGraphicsRootDescriptorTable(kRP_SRVs, srvTable);
        m_boundSrvTable = srvTable;
    }
    if (m_boundSamplerTable.ptr != sampTable.ptr) {
        cmd->SetGraphicsRootDescriptorTable(kRP_Samplers, sampTable);
        m_boundSamplerTable = sampTable;
    }

    const auto& rs = m_rst.State().rs;
    const UINT stencilRef = rs[D3DRS_STENCILREF];
    if (m_boundStencilRef != stencilRef) {
        cmd->OMSetStencilRef(stencilRef);
        m_boundStencilRef = stencilRef;
    }
    const DWORD bf = rs[D3DRS_BLENDFACTOR];
    if (m_boundBlendFactor != bf) {
        const float factor[4] = {
            ((bf >> 16) & 0xFF) / 255.0f,
            ((bf >>  8) & 0xFF) / 255.0f,
            ((bf      ) & 0xFF) / 255.0f,
            ((bf >> 24) & 0xFF) / 255.0f,
        };
        cmd->OMSetBlendFactor(factor);
        m_boundBlendFactor = bf;
    }

    const D3D12_PRIMITIVE_TOPOLOGY topo = TopologyOf12(primType);
    if (m_boundTopology != topo) {
        cmd->IASetPrimitiveTopology(topo);
        m_boundTopology = topo;
    }

    D3D12_VERTEX_BUFFER_VIEW vbv[16]{};
    UINT highestSlot = 0;
    bool needZero = false;
    for (const auto& e : layout->elems) {
        if (e.InputSlot == kZeroStreamSlot12) { needZero = true; continue; }
        highestSlot = std::max(highestSlot, e.InputSlot + 1u);
    }

    const auto& st = m_rst.State();
    for (UINT slot = 0; slot < highestSlot && slot < 16; ++slot) {
        auto* vb = static_cast<D9VertexBuffer12*>(st.streams[slot]);
        if (!vb) continue;
        const UINT stride = st.streamStrides[slot];
        if (stride == 0) continue;
        vbv[slot].BufferLocation = vb->CurrentGpuAddress() + st.streamOffsets[slot];
        vbv[slot].SizeInBytes    = vb->ByteSize() > st.streamOffsets[slot]
                                 ? vb->ByteSize() - st.streamOffsets[slot] : 0;
        vbv[slot].StrideInBytes  = stride;

        if (vb->Gpu().Valid())
            vb->Gpu().AppendTransition(m_barrierScratch, 0,
                                       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }
    if (highestSlot)
        cmd->IASetVertexBuffers(0, highestSlot, vbv);

    if (needZero) {
        D3D12_VERTEX_BUFFER_VIEW zero{};
        zero.BufferLocation = m_zeroStream.Gpu();
        zero.SizeInBytes    = 256;
        zero.StrideInBytes  = 0;
        cmd->IASetVertexBuffers(kZeroStreamSlot12, 1, &zero);
    }

    // `indexed` means "bind the index buffer the game set", which is not the
    // same thing as "this draw uses indices". The user-pointer draws
    // (DrawIndexedPrimitiveUP) carry their own indices and deliberately null
    // st.indexBuffer, so they must reach here with indexed=false.
    //
    // This guard used to be a bare `if (!ib) return;` with no logging, and the
    // user-pointer path passed indexed=true. Most Wanted draws its intro, its
    // menus and its entire HUD through DrawIndexedPrimitiveUP, so that one
    // silent early-out discarded 100% of the game's front-end draws while
    // returning success. Nothing errored and nothing logged; the screen was
    // simply black. It is loud now for exactly that reason.
    if (indexed) {
        auto* ib = static_cast<D9IndexBuffer12*>(st.indexBuffer);
        if (!ib) {

            static unsigned s_reported = 0;
            if (s_reported < 12u) {
                ++s_reported;
                UINT rtw = 0, rth = 0;
                if (m_renderTargets[0]) {
                    rtw = m_renderTargets[0]->Width();
                    rth = m_renderTargets[0]->Height();
                }
                DXLOG_WARN("[dx12] indexed draw dropped (no index buffer bound) "
                           "#%u: prim=%d RT0=%ux%u vs=%p ps=%p decl=%p fvf=0x%08X "
                           "stream0=%p frame=%llu",
                           s_reported, (int)primType, rtw, rth,
                           (void*)m_vs, (void*)m_ps, (void*)m_decl,
                           (unsigned)m_fvf, (void*)st.streams[0],
                           (unsigned long long)m_frameCount);
            }
            NoteDrawDropped("indexed draw with no index buffer bound "
                            "(SetIndices was never called for this draw)");
            return D3DERR_INVALIDCALL;
        }
        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = ib->CurrentGpuAddress();
        ibv.SizeInBytes    = ib->ByteSize();
        ibv.Format         = ib->DxgiIndexFormat();
        if (ib->Gpu().Valid())
            ib->Gpu().AppendTransition(m_barrierScratch, 0,
                                       D3D12_RESOURCE_STATE_INDEX_BUFFER);
        cmd->IASetIndexBuffer(&ibv);
    }

    if (!m_barrierScratch.empty()) {
        cmd->ResourceBarrier(static_cast<UINT>(m_barrierScratch.size()),
                             m_barrierScratch.data());
        m_barrierScratch.clear();
    }

    {
        static const bool     s_census    = BackendSelect::DumpFrameEnabled();
        static const unsigned s_censusCap = BackendSelect::CensusLimit();
        if (s_census) {
            static uint64_t s_frame      = UINT64_MAX;
            static unsigned s_worldDraws = 0;
            static unsigned s_lineN      = 0;
            static unsigned s_framesDone = 0;
            if (m_frameCount != s_frame) {
                if (s_frame != UINT64_MAX && s_lineN) ++s_framesDone;
                s_frame = m_frameCount; s_worldDraws = 0; s_lineN = 0;
            }
            const auto& crs = m_rst.State().rs;
            const bool zOn =
                static_cast<D3DZBUFFERTYPE>(crs[D3DRS_ZENABLE]) != D3DZB_FALSE;
            if (zOn) ++s_worldDraws;

            // Two triggers, checked in this order: the in-race heuristic, then
            // the F11 key. The key is what reaches a front-end screen, which
            // the draw-count heuristic can never arm on.
            static bool     s_gameplay = false;
            static bool     s_keyArmed = false;
            static uint64_t s_keyFrame = UINT64_MAX;
            if (m_frameCount != s_keyFrame) {
                s_keyFrame = m_frameCount;
                if (BackendSelect::CensusTriggerPressed()) {
                    s_keyArmed   = true;
                    s_framesDone = 0;
                    DXLOG_WARN("census: F11 pressed - capturing the next 6 frames "
                               "of whatever is on screen");
                }
            }
            if (s_worldDraws >= 100u) s_gameplay = true;
            if ((s_gameplay || s_keyArmed) && s_framesDone < 6u && s_lineN < s_censusCap) {
                ++s_lineN;
                auto kind = [](IDirect3DBaseTexture9* t) -> const char* {
                    if (!t) return "none";
                    switch (t->GetType()) {
                        case D3DRTYPE_CUBETEXTURE:   return "CUBE";
                        case D3DRTYPE_VOLUMETEXTURE: return "VOL";
                        default:                     return "2D";
                    }
                };
                const DWORD vtok = m_vs && m_vs->Bytecode() ? m_vs->Bytecode()[0] : 0;
                const DWORD ptok = m_ps && m_ps->Bytecode() ? m_ps->Bytecode()[0] : 0;

                UINT s0w = 0, s0h = 0, s0fmt = 0;
                const void* s0tex = m_rst.State().textures[0];
                if (auto* st0 = StorageOf(m_rst.State().textures[0])) {
                    s0w = st0->Width(); s0h = st0->Height();
                    s0fmt = (unsigned)st0->Gpu().Desc().Format;
                }
                UINT rtw = 0, rth = 0, rtbb = 0;
                if (m_renderTargets[0]) {
                    rtw = m_renderTargets[0]->Width();
                    rth = m_renderTargets[0]->Height();
                    rtbb = m_renderTargets[0]->IsBackBuffer() ? 1u : 0u;
                }
                UINT dsw = 0, dsh = 0;
                if (m_depthStencil) {
                    dsw = m_depthStencil->Width();
                    dsh = m_depthStencil->Height();
                }
                DXLOG_WARN("census#%u f%llu %s vs=0x%08X ps=0x%08X s0=%s s1=%s "
                           "s0tex=%p s0dim=%ux%u s0fmt=%u "
                           "RT0=%ux%u RT0bb=%u blend=%u cw=0x%X atest=%u "
                           "zen=%u zw=%u zfunc=%u sten=%u sref=%u sfunc=%u DS=%ux%u",
                           s_lineN, (unsigned long long)m_frameCount,
                           (m_vs ? "prog" : "FFP"), vtok, ptok,
                           kind(m_rst.State().textures[0]),
                           kind(m_rst.State().textures[1]),
                           s0tex, s0w, s0h, s0fmt, rtw, rth, rtbb,
                           (unsigned)(crs[D3DRS_ALPHABLENDENABLE] ? 1 : 0),
                           (unsigned)crs[D3DRS_COLORWRITEENABLE],
                           (unsigned)(crs[D3DRS_ALPHATESTENABLE] ? 1 : 0),
                           (unsigned)(zOn ? 1 : 0),
                           (unsigned)(crs[D3DRS_ZWRITEENABLE] ? 1 : 0),
                           (unsigned)crs[D3DRS_ZFUNC],
                           (unsigned)(crs[D3DRS_STENCILENABLE] ? 1 : 0),
                           (unsigned)crs[D3DRS_STENCILREF],
                           (unsigned)crs[D3DRS_STENCILFUNC],
                           dsw, dsh);
            }
        }
    }

    ++m_drawCount;
    if (!m_vs || !m_ps) ++m_ffpDraws;
    if (const D3DVERTEXELEMENT9* d = EffectiveDeclaration()) {
        for (UINT i = 0; i < MAXD3DDECLLENGTH && d[i].Stream != 0xFF; ++i) {
            if (d[i].Usage == D3DDECLUSAGE_POSITIONT) { ++m_posTDraws; break; }
        }
    }
    return D3D_OK;
}

HRESULT D9Device12::BuildFanIndices(UINT primCount, UINT startVertex,
                                    D3D12_INDEX_BUFFER_VIEW* outView) noexcept
{
    if (!outView || primCount == 0) return D3DERR_INVALIDCALL;

    const UINT indexCount = primCount * 3;
    const UINT bytes      = indexCount * sizeof(uint32_t);
    auto a = m_ctx->AllocUpload(bytes, 4);
    if (!a.Valid()) return E_OUTOFMEMORY;

    auto* idx = reinterpret_cast<uint32_t*>(a.cpu);
    for (UINT i = 0; i < primCount; ++i) {
        idx[i * 3 + 0] = startVertex;
        idx[i * 3 + 1] = startVertex + i + 1;
        idx[i * 3 + 2] = startVertex + i + 2;
    }

    outView->BufferLocation = a.gpu;
    outView->SizeInBytes    = bytes;
    outView->Format         = DXGI_FORMAT_R32_UINT;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::DrawPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{

    ++m_drawCalls;
    NoteDrawOutsideScene(m_inScene);
    if (PrimitiveCount == 0) return D3D_OK;

    const bool fan = (PrimitiveType == D3DPT_TRIANGLEFAN);

    HRESULT hr = PreDrawFlush(fan ? D3DPT_TRIANGLELIST : PrimitiveType, false);
    if (FAILED(hr)) return hr;

    auto* cmd = m_ctx->CmdList();
    if (fan) {

        D3D12_INDEX_BUFFER_VIEW ibv{};
        hr = BuildFanIndices(PrimitiveCount, StartVertex, &ibv);
        if (FAILED(hr)) return hr;
        cmd->IASetIndexBuffer(&ibv);
        cmd->DrawIndexedInstanced(PrimitiveCount * 3, 1, 0, 0, 0);
        return D3D_OK;
    }

    cmd->DrawInstanced(VertexCountFor12(PrimitiveType, PrimitiveCount), 1,
                       StartVertex, 0);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
    UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{

    ++m_drawCalls;
    NoteDrawOutsideScene(m_inScene);
    if (PrimitiveCount == 0) return D3D_OK;
    (void)MinVertexIndex; (void)NumVertices;

    if (PrimitiveType == D3DPT_TRIANGLEFAN) {

        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            DXLOG_WARN("[dx12] indexed D3DPT_TRIANGLEFAN is not supported");
        }
        return D3DERR_INVALIDCALL;
    }

    HRESULT hr = PreDrawFlush(PrimitiveType, true);
    if (FAILED(hr)) return hr;

    m_ctx->CmdList()->DrawIndexedInstanced(
        VertexCountFor12(PrimitiveType, PrimitiveCount), 1,
        StartIndex, BaseVertexIndex, 0);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
    CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{

    ++m_drawCalls;
    NoteDrawOutsideScene(m_inScene);
    if (!pVertexStreamZeroData || VertexStreamZeroStride == 0 || PrimitiveCount == 0)
        return D3DERR_INVALIDCALL;

    const UINT vertexCount = VertexCountFor12(PrimitiveType, PrimitiveCount);
    const UINT bytes       = vertexCount * VertexStreamZeroStride;

    auto a = m_ctx->AllocUpload(bytes, 16);
    if (!a.Valid()) return E_OUTOFMEMORY;
    std::memcpy(a.cpu, pVertexStreamZeroData, bytes);

    auto& st = m_rst.State();
    IDirect3DVertexBuffer9* savedVB = st.streams[0];
    const UINT savedStride = st.streamStrides[0];
    const UINT savedOffset = st.streamOffsets[0];
    st.streams[0]       = nullptr;
    st.streamStrides[0] = VertexStreamZeroStride;
    st.streamOffsets[0] = 0;

    const bool fan = (PrimitiveType == D3DPT_TRIANGLEFAN);

    HRESULT hr = PreDrawFlush(fan ? D3DPT_TRIANGLELIST : PrimitiveType, false);
    if (SUCCEEDED(hr)) {
        auto* cmd = m_ctx->CmdList();
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = a.gpu;
        vbv.SizeInBytes    = bytes;
        vbv.StrideInBytes  = VertexStreamZeroStride;
        cmd->IASetVertexBuffers(0, 1, &vbv);

        if (fan) {
            D3D12_INDEX_BUFFER_VIEW ibv{};
            hr = BuildFanIndices(PrimitiveCount, 0, &ibv);
            if (SUCCEEDED(hr)) {
                cmd->IASetIndexBuffer(&ibv);
                cmd->DrawIndexedInstanced(PrimitiveCount * 3, 1, 0, 0, 0);
            }
        } else {
            cmd->DrawInstanced(vertexCount, 1, 0, 0);
        }
    }

    st.streams[0]       = savedVB;
    st.streamStrides[0] = savedStride;
    st.streamOffsets[0] = savedOffset;
    return hr;
}

HRESULT STDMETHODCALLTYPE D9Device12::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
    UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat,
    CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{

    ++m_drawCalls;
    NoteDrawOutsideScene(m_inScene);
    if (!pIndexData || !pVertexStreamZeroData || VertexStreamZeroStride == 0 ||
        PrimitiveCount == 0)
        return D3DERR_INVALIDCALL;

    const bool fan = (PrimitiveType == D3DPT_TRIANGLEFAN);

    const UINT indexCount = fan ? PrimitiveCount * 3u
                                : VertexCountFor12(PrimitiveType, PrimitiveCount);
    const UINT indexSize  = (IndexDataFormat == D3DFMT_INDEX32) ? 4u : 2u;
    const UINT vbBytes    = (MinVertexIndex + NumVertices) * VertexStreamZeroStride;
    const UINT ibBytes    = indexCount * indexSize;

    auto va = m_ctx->AllocUpload(vbBytes, 16);
    auto ia = m_ctx->AllocUpload(ibBytes, 16);
    if (!va.Valid() || !ia.Valid()) return E_OUTOFMEMORY;
    std::memcpy(va.cpu, pVertexStreamZeroData, vbBytes);
    if (fan) {
        if (indexSize == 2) {
            const auto* src = static_cast<const uint16_t*>(pIndexData);
            auto* dst = reinterpret_cast<uint16_t*>(ia.cpu);
            for (UINT i = 0; i < PrimitiveCount; ++i) {
                *dst++ = src[0];
                *dst++ = src[i + 1];
                *dst++ = src[i + 2];
            }
        } else {
            const auto* src = static_cast<const uint32_t*>(pIndexData);
            auto* dst = reinterpret_cast<uint32_t*>(ia.cpu);
            for (UINT i = 0; i < PrimitiveCount; ++i) {
                *dst++ = src[0];
                *dst++ = src[i + 1];
                *dst++ = src[i + 2];
            }
        }
    } else {
        std::memcpy(ia.cpu, pIndexData, ibBytes);
    }

    // The vertex and index data for this draw came from user pointers and now
    // live in the upload ring, so stream 0 and the index buffer must not be
    // taken from the tracked state. They are nulled for the duration and put
    // back afterwards.
    //
    // D3D9 documents that DrawIndexedPrimitiveUP *leaves* stream 0 and the
    // index buffer set to NULL when it returns. Restoring them is therefore a
    // deliberate divergence from the specification. It is more permissive than
    // the real runtime, so nothing that works on D3D9 breaks here — but a game
    // written to rely on the documented nulling would see stale bindings.
    auto& st = m_rst.State();
    IDirect3DVertexBuffer9* savedVB = st.streams[0];
    IDirect3DIndexBuffer9*  savedIB = st.indexBuffer;
    const UINT savedStride = st.streamStrides[0];
    const UINT savedOffset = st.streamOffsets[0];
    st.streams[0]       = nullptr;
    st.indexBuffer      = nullptr;
    st.streamStrides[0] = VertexStreamZeroStride;
    st.streamOffsets[0] = 0;

    // indexed=false: the indices are ours, not the game's. See the guard in
    // PreDrawFlush for why passing true here was catastrophic.
    HRESULT hr = PreDrawFlush(fan ? D3DPT_TRIANGLELIST : PrimitiveType, false);
    if (SUCCEEDED(hr)) {
        auto* cmd = m_ctx->CmdList();
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = va.gpu;
        vbv.SizeInBytes    = vbBytes;
        vbv.StrideInBytes  = VertexStreamZeroStride;
        cmd->IASetVertexBuffers(0, 1, &vbv);

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = ia.gpu;
        ibv.SizeInBytes    = ibBytes;
        ibv.Format         = (indexSize == 4) ? DXGI_FORMAT_R32_UINT
                                              : DXGI_FORMAT_R16_UINT;
        cmd->IASetIndexBuffer(&ibv);
        cmd->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    }

    st.streams[0]       = savedVB;
    st.indexBuffer      = savedIB;
    st.streamStrides[0] = savedStride;
    st.streamOffsets[0] = savedOffset;
    return hr;
}

}
