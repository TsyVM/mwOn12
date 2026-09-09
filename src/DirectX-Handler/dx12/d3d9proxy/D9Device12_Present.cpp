// D3D12 frame presentation and reset.
//
// Present closes the command list, submits it, presents the swap chain and
// advances the frame fence. That fence is what makes the whole backend safe:
// it is what tells the next frame that the allocators, upload memory and
// retired resources belonging to an earlier frame are free to reuse.
//
// Nothing may be reused before its fence value has been reached. Reusing a
// command allocator that the GPU is still executing from corrupts work already
// submitted, and does so without any error being raised.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy12/D9Device12.h>
#include <core/BackendSelect.h>
#include <core/Log.h>
#include <sdk/PluginHost.h>

#include <algorithm>
#include <cstring>

namespace mwon12 {

void D9Device12::ReleaseImplicitSurfaces() noexcept
{
    if (m_implicitBackBuffer) { m_implicitBackBuffer->Release(); m_implicitBackBuffer = nullptr; }
    if (m_implicitDepth) { m_implicitDepth->Release(); m_implicitDepth = nullptr; }
}

HRESULT D9Device12::BindDefaultTargets() noexcept
{

    D9Surface12* bb = m_implicitBackBuffer;
    if (!bb) return D3DERR_INVALIDCALL;

    for (auto*& rt : m_renderTargets) { if (rt) { rt->Release(); rt = nullptr; } }
    bb->AddRef();
    m_renderTargets[0] = bb;

    if (m_depthStencil) { m_depthStencil->Release(); m_depthStencil = nullptr; }
    if (m_implicitDepth) {
        m_implicitDepth->AddRef();
        m_depthStencil = m_implicitDepth;
    }

    m_viewport.X      = 0;
    m_viewport.Y      = 0;
    m_viewport.Width  = m_ctx->BackBufferWidth();
    m_viewport.Height = m_ctx->BackBufferHeight();
    m_viewport.MinZ   = 0.0f;
    m_viewport.MaxZ   = 1.0f;
    m_scissor = RECT{ 0, 0, LONG(m_viewport.Width), LONG(m_viewport.Height) };

    m_emu.g_emuMisc[0] = m_viewport.Width  ? 1.0f / float(m_viewport.Width)  : 0.0f;
    m_emu.g_emuMisc[1] = m_viewport.Height ? 1.0f / float(m_viewport.Height) : 0.0f;
    m_emuDirty = true;
    m_ffpConst.ViewportInfo[0] = m_emu.g_emuMisc[0];
    m_ffpConst.ViewportInfo[1] = m_emu.g_emuMisc[1];
    m_ffpConstDirty = true;

    m_targetsDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::BeginScene()
{
    if (m_inScene) return D3DERR_INVALIDCALL;
    if (!m_ctx) return D3DERR_INVALIDCALL;
    m_inScene = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::EndScene()
{
    if (!m_inScene) return D3DERR_INVALIDCALL;
    m_inScene = false;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::Clear(
    DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color,
    float Z, DWORD Stencil)
{
    if (!m_ctx || !m_ctx->CmdList()) return D3DERR_INVALIDCALL;
    if (Flags == 0) return D3DERR_INVALIDCALL;

    auto* cmd = m_ctx->CmdList();

    std::vector<D3D12_RECT> rects;
    if (Count && pRects) {
        rects.reserve(Count);
        for (DWORD i = 0; i < Count; ++i)
            rects.push_back(D3D12_RECT{ pRects[i].x1, pRects[i].y1,
                                        pRects[i].x2, pRects[i].y2 });
    } else {
        rects.push_back(D3D12_RECT{
            LONG(m_viewport.X), LONG(m_viewport.Y),
            LONG(m_viewport.X + m_viewport.Width),
            LONG(m_viewport.Y + m_viewport.Height) });
    }
    const bool wholeTarget =
        rects.size() == 1 && rects[0].left == 0 && rects[0].top == 0 &&
        m_renderTargets[0] &&
        rects[0].right  == LONG(m_renderTargets[0]->Width()) &&
        rects[0].bottom == LONG(m_renderTargets[0]->Height());

    m_barrierScratch.clear();
    if (Flags & D3DCLEAR_TARGET) {
        for (auto* rt : m_renderTargets)
            if (rt) rt->AppendTransition(m_barrierScratch,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if ((Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && m_depthStencil)
        m_depthStencil->AppendTransition(m_barrierScratch,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (!m_barrierScratch.empty())
        cmd->ResourceBarrier(static_cast<UINT>(m_barrierScratch.size()),
                             m_barrierScratch.data());

    if (Flags & D3DCLEAR_TARGET) {

        const float rgba[4] = {
            ((Color >> 16) & 0xFF) / 255.0f,
            ((Color >>  8) & 0xFF) / 255.0f,
            ((Color      ) & 0xFF) / 255.0f,
            ((Color >> 24) & 0xFF) / 255.0f,
        };
        const bool srgbWrite = m_rst.State().rs[D3DRS_SRGBWRITEENABLE] != 0;
        for (auto* rt : m_renderTargets) {
            if (!rt) continue;
            const auto rtv = rt->Rtv(srgbWrite);
            if (rtv.ptr == SIZE_T(-1)) continue;
            if (wholeTarget)
                cmd->ClearRenderTargetView(rtv, rgba, 0, nullptr);
            else
                cmd->ClearRenderTargetView(rtv, rgba,
                                           static_cast<UINT>(rects.size()), rects.data());
        }
    }

    if ((Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && m_depthStencil) {
        // A clear writes, so it always goes through the writable view and the
        // buffer is now in DEPTH_WRITE. If a read-only view was bound, the
        // pairing no longer matches; re-establishing the binding on the next
        // draw keeps view and state consistent.
        m_targetsDirty = true;
        const auto dsv = m_depthStencil->Dsv(false);
        if (dsv.ptr != SIZE_T(-1)) {
            D3D12_CLEAR_FLAGS cf{};
            if (Flags & D3DCLEAR_ZBUFFER) cf |= D3D12_CLEAR_FLAG_DEPTH;
            if (Flags & D3DCLEAR_STENCIL) cf |= D3D12_CLEAR_FLAG_STENCIL;
            if (wholeTarget)
                cmd->ClearDepthStencilView(dsv, cf, Z, UINT8(Stencil), 0, nullptr);
            else
                cmd->ClearDepthStencilView(dsv, cf, Z, UINT8(Stencil),
                                           static_cast<UINT>(rects.size()), rects.data());
        }
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetViewport(CONST D3DVIEWPORT9* pViewport)
{
    if (!pViewport) return D3DERR_INVALIDCALL;
    if (m_recordingBlock) { m_recordingBlock->RecordViewport(*pViewport); return D3D_OK; }
    m_viewport = *pViewport;

    m_emu.g_emuMisc[0] = pViewport->Width  ? 1.0f / float(pViewport->Width)  : 0.0f;
    m_emu.g_emuMisc[1] = pViewport->Height ? 1.0f / float(pViewport->Height) : 0.0f;
    m_emuDirty = true;

    m_ffpConst.ViewportInfo[0] = m_emu.g_emuMisc[0];
    m_ffpConst.ViewportInfo[1] = m_emu.g_emuMisc[1];
    m_ffpConst.ViewportInfo[2] = float(pViewport->X);
    m_ffpConst.ViewportInfo[3] = float(pViewport->Y);
    m_ffpConstDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetViewport(D3DVIEWPORT9* pViewport)
{
    if (!pViewport) return D3DERR_INVALIDCALL;
    *pViewport = m_viewport;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetScissorRect(CONST RECT* pRect)
{
    if (!pRect) return D3DERR_INVALIDCALL;
    if (m_recordingBlock) { m_recordingBlock->RecordScissor(*pRect); return D3D_OK; }
    m_scissor = *pRect;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetScissorRect(RECT* pRect)
{
    if (!pRect) return D3DERR_INVALIDCALL;
    *pRect = m_scissor;
    return D3D_OK;
}

void D9Device12::ApplyViewportScissor() noexcept
{
    if (!m_ctx || !m_ctx->CmdList()) return;
    auto* cmd = m_ctx->CmdList();

    D3D12_VIEWPORT vp{};
    vp.TopLeftX = float(m_viewport.X);
    vp.TopLeftY = float(m_viewport.Y);
    vp.Width    = float(m_viewport.Width);
    vp.Height   = float(m_viewport.Height);
    vp.MinDepth = m_viewport.MinZ;
    vp.MaxDepth = m_viewport.MaxZ;
    cmd->RSSetViewports(1, &vp);

    D3D12_RECT sc{};
    if (m_rst.State().rs[D3DRS_SCISSORTESTENABLE]) {
        sc = D3D12_RECT{ m_scissor.left, m_scissor.top,
                         m_scissor.right, m_scissor.bottom };
    } else {
        const LONG w = m_renderTargets[0] ? LONG(m_renderTargets[0]->Width())
                                          : LONG(m_ctx->BackBufferWidth());
        const LONG h = m_renderTargets[0] ? LONG(m_renderTargets[0]->Height())
                                          : LONG(m_ctx->BackBufferHeight());
        sc = D3D12_RECT{ 0, 0, w, h };
    }
    cmd->RSSetScissorRects(1, &sc);
}

// Gives plugins the finished frame, immediately before it is presented.
//
// The back buffer is put into RENDER_TARGET and bound as the sole target with
// no depth buffer, and the viewport and scissor are set to cover it, so the
// simplest possible plugin -- set a PSO, draw a quad -- works without knowing
// anything about what the game left behind.
//
// Whatever a plugin does to the command list afterwards is undone the same way
// Blitter12 undoes its own: MarkCommandStateDirty makes the next draw
// re-establish the root signature, the descriptor heaps and every cached
// binding from scratch. Nothing is read back, so there is nothing to restore
// by hand.
//
// PresentFrame transitions the back buffer from whatever state we leave it in,
// so leaving it in RENDER_TARGET is correct and needs no undoing here.
void D9Device12::RunPresentPlugins() noexcept
{
    if (!sdk::PluginHost::WantsPresent()) return;
    if (!m_ctx || !m_ctx->CmdList() || !m_implicitBackBuffer) return;

    auto* cmd = m_ctx->CmdList();

    m_barrierScratch.clear();
    m_implicitBackBuffer->AppendTransition(m_barrierScratch,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!m_barrierScratch.empty())
        cmd->ResourceBarrier(static_cast<UINT>(m_barrierScratch.size()),
                             m_barrierScratch.data());

    const auto rtv = m_implicitBackBuffer->Rtv(false);
    if (rtv.ptr == SIZE_T(-1)) return;
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const UINT w = m_ctx->BackBufferWidth();
    const UINT h = m_ctx->BackBufferHeight();
    const D3D12_VIEWPORT vp{ 0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f };
    const D3D12_RECT     sc{ 0, 0, LONG(w), LONG(h) };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    auto* res = m_implicitBackBuffer->Gpu();
    sdk::PluginHost::OnPresent(this, res ? res->Native() : nullptr,
                               static_cast<UINT64>(rtv.ptr));

    m_ctx->MarkCommandStateDirty();
    m_targetsDirty = true;
}

HRESULT STDMETHODCALLTYPE D9Device12::Present(
    CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*)
{
    if (!m_ctx) return D3DERR_INVALIDCALL;
    if (m_ctx->IsDeviceLost()) return D3DERR_DEVICELOST;

    if (m_inScene) return D3DERR_INVALIDCALL;

    RunPresentPlugins();

    UINT syncInterval = 1;
    switch (m_pp.PresentationInterval) {
    case D3DPRESENT_INTERVAL_IMMEDIATE: syncInterval = 0; break;
    case D3DPRESENT_INTERVAL_TWO:       syncInterval = 2; break;
    case D3DPRESENT_INTERVAL_THREE:     syncInterval = 3; break;
    case D3DPRESENT_INTERVAL_FOUR:      syncInterval = 4; break;
    default:                            syncInterval = 1; break;
    }

    const HRESULT hr = m_ctx->PresentFrame(syncInterval);

    m_boundPso          = nullptr;
    m_boundTopology     = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    m_boundSrvTable     = D3D12_GPU_DESCRIPTOR_HANDLE{};
    m_boundSamplerTable = D3D12_GPU_DESCRIPTOR_HANDLE{};
    m_boundStencilRef   = UINT(-1);
    m_boundBlendFactor  = 0xFFFFFFFF;
    m_targetsDirty      = true;
    m_constFrame        = UINT64(-1);
    m_rst.MarkAllDirty();

    ++m_frameCount;
    if (BackendSelect::PerfLogEnabled() && (m_frameCount % 120) == 0) {
        DXLOG_INFO("[dx12] frame %llu | Draw* calls %llu -> submitted %llu "
                   "(screen-space %llu, FFP %llu) | "
                   "PSOs %zu (%llu misses) | "
                   "samplers %zu | shaders %zu | FFP perms %zu",
                   (unsigned long long)m_frameCount,
                   (unsigned long long)m_drawCalls,
                   (unsigned long long)m_drawCount,
                   (unsigned long long)m_posTDraws,
                   (unsigned long long)m_ffpDraws,
                   m_psoCache.PipelineCount(),
                   (unsigned long long)m_psoCache.Misses(),
                   m_psoCache.SamplerCount(),
                   m_shaders.EntryCount(),
                   m_ffp.PermutationCount());
    }

    if (m_ctx->IsDeviceLost()) return D3DERR_DEVICELOST;
    return SUCCEEDED(hr) ? D3D_OK : hr;
}

HRESULT STDMETHODCALLTYPE D9Device12::Reset(D3DPRESENT_PARAMETERS* pPP)
{
    if (!pPP || !m_ctx) return D3DERR_INVALIDCALL;

    m_inScene = false;

    if (pPP->BackBufferWidth  == 0) pPP->BackBufferWidth  = m_pp.BackBufferWidth;
    if (pPP->BackBufferHeight == 0) pPP->BackBufferHeight = m_pp.BackBufferHeight;
    if (pPP->BackBufferFormat == D3DFMT_UNKNOWN)
        pPP->BackBufferFormat = m_pp.BackBufferFormat;

    for (auto*& rt : m_renderTargets) { if (rt) { rt->Release(); rt = nullptr; } }
    if (m_depthStencil) { m_depthStencil->Release(); m_depthStencil = nullptr; }
    ReleaseImplicitSurfaces();

    m_ctx->ApplyWindowMode(*pPP);
    HRESULT hr = m_ctx->ResizeSwapChain(pPP->BackBufferWidth, pPP->BackBufferHeight, *pPP);
    if (FAILED(hr)) {
        m_ctx->ReportDeviceRemoved("Reset");
        return D3DERR_DEVICELOST;
    }

    m_pp = *pPP;

    hr = D9Surface12::CreateBackBuffer(this, &m_implicitBackBuffer);
    if (FAILED(hr)) return D3DERR_DEVICELOST;
    if (m_pp.EnableAutoDepthStencil) {
        hr = D9Surface12::CreateStandalone(
            this, m_ctx->BackBufferWidth(), m_ctx->BackBufferHeight(),
            m_pp.AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL,
            D3DPOOL_DEFAULT, FALSE, &m_implicitDepth);
        if (FAILED(hr)) m_implicitDepth = nullptr;
    }

    auto& st = m_rst.State();
    for (auto*& t : st.textures)   { if (t) { t->Release(); t = nullptr; } }
    for (auto*& s : st.streams)    { if (s) { s->Release(); s = nullptr; } }
    if (st.indexBuffer)  { st.indexBuffer->Release();  st.indexBuffer = nullptr; }
    if (st.vertexDecl)   { st.vertexDecl->Release();   st.vertexDecl = nullptr; }
    if (st.vertexShader) { st.vertexShader->Release(); st.vertexShader = nullptr; }
    if (st.pixelShader)  { st.pixelShader->Release();  st.pixelShader = nullptr; }
    m_decl = nullptr; m_vs = nullptr; m_ps = nullptr;
    m_rst.ResetToDefaults();
    m_fixed.Reset();
    m_fvf = 0;
    m_fvfDeclCached = 0xFFFFFFFF;
    for (auto& f : m_streamFreq) f = 1;
    std::memset(m_clipPlanes, 0, sizeof(m_clipPlanes));

    m_vsConstDirty = m_psConstDirty = m_emuDirty = m_ffpConstDirty = true;
    m_constFrame   = UINT64(-1);
    m_boundPso     = nullptr;
    m_boundTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    m_boundSrvTable     = D3D12_GPU_DESCRIPTOR_HANDLE{};
    m_boundSamplerTable = D3D12_GPU_DESCRIPTOR_HANDLE{};
    m_boundStencilRef   = UINT(-1);
    m_boundBlendFactor  = 0xFFFFFFFF;

    hr = BindDefaultTargets();
    DXLOG_INFO("[dx12] Reset -> %ux%u fmt=%d", m_pp.BackBufferWidth,
               m_pp.BackBufferHeight, (int)m_pp.BackBufferFormat);

    // After the new back buffer exists and is bound, so a plugin rebuilding
    // something sized to it reads the size that is now true.
    if (SUCCEEDED(hr)) sdk::PluginHost::OnResize(this);

    return SUCCEEDED(hr) ? D3D_OK : D3DERR_DEVICELOST;
}

}
