// D3D12 device construction, teardown and device-level queries.
//
// Creates the device context, the swap chain and the caches, then completes
// initialisation in a second phase. That two-phase construction is load-bearing
// rather than stylistic: several subsystems need a fully constructed device to
// create their own resources, and a device left half-initialised comes up in a
// state where individual calls succeed while the whole is inconsistent.
//
// The device is never reported as lost. Modern APIs virtualise the GPU and a
// genuine loss is rare; claiming one sends the game into a recovery path with
// nothing to recover.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy12/D9Device12.h>
#include <d3d9proxy12/D9Root12.h>
#include <core/FormatConverter.h>
#include <core/Log.h>
#include <sdk/PluginHost.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace mwon12 {

D9Device12::D9Device12(std::unique_ptr<DeviceContext12> ctx, D9Root12* root,
                       DWORD behaviourFlags, const D3DPRESENT_PARAMETERS& pp,
                       bool isEx) noexcept
    : m_ctx(std::move(ctx))
    , m_root(root)
    , m_behaviourFlags(behaviourFlags)
    , m_pp(pp)
    , m_isEx(isEx)
    , m_psoCache(m_ctx.get())
    , m_ffp(m_ctx.get())
    , m_blitter(m_ctx.get(), &m_psoCache)
{
    if (m_root) m_root->AddRef();

    m_fixed.Reset();

    m_viewport.X      = 0;
    m_viewport.Y      = 0;
    m_viewport.Width  = pp.BackBufferWidth;
    m_viewport.Height = pp.BackBufferHeight;
    m_viewport.MinZ   = 0.0f;
    m_viewport.MaxZ   = 1.0f;

    m_scissor = RECT{ 0, 0, LONG(pp.BackBufferWidth), LONG(pp.BackBufferHeight) };

    for (auto& f : m_streamFreq) f = 1;

    std::memset(&m_emu, 0, sizeof(m_emu));
    m_emu.g_emuMisc[0] = pp.BackBufferWidth  ? 1.0f / float(pp.BackBufferWidth)  : 0.0f;
    m_emu.g_emuMisc[1] = pp.BackBufferHeight ? 1.0f / float(pp.BackBufferHeight) : 0.0f;

    std::memset(&m_ffpConst, 0, sizeof(m_ffpConst));
    m_ffpConst.ViewportInfo[0] = m_emu.g_emuMisc[0];
    m_ffpConst.ViewportInfo[1] = m_emu.g_emuMisc[1];

    DXLOG_INFO("[dx12] D9Device12 created (%ux%u windowed=%d ex=%d)",
               pp.BackBufferWidth, pp.BackBufferHeight, pp.Windowed ? 1 : 0,
               isEx ? 1 : 0);
}

D9Device12::~D9Device12()
{
    // Before anything is torn down, and before the GPU wait, so a plugin can
    // still record or query if it needs to. Everything it owns must be gone by
    // the time this returns -- the D3D12 device does not survive this function.
    sdk::PluginHost::OnDeviceDestroyed();

    if (m_ctx) m_ctx->WaitForGPU();

    ReleaseImplicitSurfaces();

    for (auto*& rt : m_renderTargets) { if (rt) { rt->Release(); rt = nullptr; } }
    if (m_depthStencil) { m_depthStencil->Release(); m_depthStencil = nullptr; }

    auto& st = m_rst.State();
    for (auto*& t : st.textures)   { if (t) { t->Release(); t = nullptr; } }
    for (auto*& s : st.streams)    { if (s) { s->Release(); s = nullptr; } }
    if (st.indexBuffer)  { st.indexBuffer->Release();  st.indexBuffer = nullptr; }
    if (st.vertexDecl)   { st.vertexDecl->Release();   st.vertexDecl = nullptr; }
    if (st.vertexShader) { st.vertexShader->Release(); st.vertexShader = nullptr; }
    if (st.pixelShader)  { st.pixelShader->Release();  st.pixelShader = nullptr; }
    m_decl = nullptr; m_vs = nullptr; m_ps = nullptr;

    m_psoCache.Clear();
    m_privateData.Clear();
    m_ctx.reset();

    if (m_root) { m_root->Release(); m_root = nullptr; }
}

HRESULT D9Device12::FinishInit() noexcept
{
    if (!m_ctx) return D3DERR_INVALIDCALL;

    const auto rd = dx12::BufferDesc(256);
    HRESULT hr = m_zeroStream.Init(m_ctx.get(), rd, D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                   L"ZeroVertexStream");
    if (FAILED(hr)) return hr;
    if (uint8_t* p = m_zeroStream.MappedPtr())
        std::memset(p, 0, 256);

    hr = D9Surface12::CreateBackBuffer(this, &m_implicitBackBuffer);
    if (FAILED(hr)) return hr;

    if (m_pp.EnableAutoDepthStencil) {
        hr = D9Surface12::CreateStandalone(
            this, m_ctx->BackBufferWidth(), m_ctx->BackBufferHeight(),
            m_pp.AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL,
            D3DPOOL_DEFAULT, FALSE, &m_implicitDepth);
        if (FAILED(hr)) {
            DXLOG_WARN("[dx12] auto depth-stencil (%dx%d fmt=%d) could not be "
                       "created; the device will start with no depth buffer",
                       m_ctx->BackBufferWidth(), m_ctx->BackBufferHeight(),
                       (int)m_pp.AutoDepthStencilFormat);
            m_implicitDepth = nullptr;
        }
    }

    hr = BindDefaultTargets();
    if (FAILED(hr)) return hr;

    // Last, and only on success: a plugin is promised a device that is fully
    // constructed, and everything above can still fail.
    sdk::PluginHost::OnDeviceCreated(this);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9)) {
        *ppvObj = static_cast<IDirect3DDevice9*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(IDirect3DDevice9Ex) && m_isEx) {
        *ppvObj = static_cast<IDirect3DDevice9Ex*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9Device12::AddRef()
{ return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE D9Device12::Release()
{
    const ULONG prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9Device12::TestCooperativeLevel()
{
    if (m_ctx && m_ctx->IsDeviceLost()) return D3DERR_DEVICENOTRESET;
    return D3D_OK;
}

UINT STDMETHODCALLTYPE D9Device12::GetAvailableTextureMem()
{

    if (m_ctx && m_ctx->Factory()) {
        ComPtr<IDXGIAdapter3> adapter3;
        ComPtr<IDXGIAdapter1> adapter1;
        if (SUCCEEDED(m_ctx->Factory()->EnumAdapters1(0, adapter1.GetAddressOf())) &&
            SUCCEEDED(adapter1.As(&adapter3))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                    0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                const UINT64 avail = info.Budget > info.CurrentUsage
                                   ? info.Budget - info.CurrentUsage : 0;

                return static_cast<UINT>(std::min<UINT64>(avail, 0xFFFFFFFFull));
            }
        }
    }
    return 256u * 1024u * 1024u;
}

HRESULT STDMETHODCALLTYPE D9Device12::EvictManagedResources() { return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device12::GetDirect3D(IDirect3D9** ppD3D)
{
    if (!ppD3D) return D3DERR_INVALIDCALL;
    *ppD3D = m_root;
    if (m_root) m_root->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetDeviceCaps(D3DCAPS9* pCaps)
{
    if (!pCaps) return D3DERR_INVALIDCALL;
    return m_root ? m_root->GetDeviceCaps(0, D3DDEVTYPE_HAL, pCaps)
                  : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetDisplayMode(UINT, D3DDISPLAYMODE* pMode)
{
    if (!pMode) return D3DERR_INVALIDCALL;
    pMode->Width       = m_ctx ? m_ctx->BackBufferWidth()  : m_pp.BackBufferWidth;
    pMode->Height      = m_ctx ? m_ctx->BackBufferHeight() : m_pp.BackBufferHeight;
    pMode->RefreshRate = m_pp.FullScreen_RefreshRateInHz ? m_pp.FullScreen_RefreshRateInHz : 60;
    pMode->Format      = m_ctx ? m_ctx->BackBufferD3D9Format() : D3DFMT_X8R8G8B8;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetCreationParameters(
    D3DDEVICE_CREATION_PARAMETERS* pOut)
{
    if (!pOut) return D3DERR_INVALIDCALL;
    pOut->AdapterOrdinal = 0;
    pOut->DeviceType     = D3DDEVTYPE_HAL;
    pOut->hFocusWindow   = m_ctx ? m_ctx->TargetHwnd() : nullptr;
    pOut->BehaviorFlags  = m_behaviourFlags;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetCursorProperties(UINT, UINT, IDirect3DSurface9*)
{ return D3D_OK; }
void STDMETHODCALLTYPE D9Device12::SetCursorPosition(int x, int y, DWORD)
{

    POINT p{ x, y };
    if (m_ctx && m_ctx->TargetHwnd()) ClientToScreen(m_ctx->TargetHwnd(), &p);
    SetCursorPos(p.x, p.y);
}
BOOL STDMETHODCALLTYPE D9Device12::ShowCursor(BOOL bShow)
{
    const BOOL was = m_cursorVisible;
    m_cursorVisible = bShow;
    ::ShowCursor(bShow);
    return was;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetRasterStatus(UINT, D3DRASTER_STATUS* pRS)
{
    if (!pRS) return D3DERR_INVALIDCALL;
    pRS->InVBlank = FALSE;
    pRS->ScanLine = 0;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetDialogBoxMode(BOOL) { return D3D_OK; }

void STDMETHODCALLTYPE D9Device12::SetGammaRamp(UINT, DWORD, CONST D3DGAMMARAMP* pRamp)
{

    if (pRamp) m_gammaRamp = *pRamp;
    static bool s_warned = false;
    if (!s_warned) {
        s_warned = true;
        DXLOG_WARN("[dx12] SetGammaRamp has no effect under flip-model "
                   "presentation; the ramp is stored for GetGammaRamp only");
    }
}

void STDMETHODCALLTYPE D9Device12::GetGammaRamp(UINT, D3DGAMMARAMP* pRamp)
{
    if (pRamp) *pRamp = m_gammaRamp;
}

HRESULT STDMETHODCALLTYPE D9Device12::ValidateDevice(DWORD* pNumPasses)
{
    if (pNumPasses) *pNumPasses = 1;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetPaletteEntries(UINT, CONST PALETTEENTRY*)
{ return D3DERR_INVALIDCALL; }
HRESULT STDMETHODCALLTYPE D9Device12::GetPaletteEntries(UINT, PALETTEENTRY*)
{ return D3DERR_INVALIDCALL; }
HRESULT STDMETHODCALLTYPE D9Device12::SetCurrentTexturePalette(UINT)
{ return D3DERR_INVALIDCALL; }
HRESULT STDMETHODCALLTYPE D9Device12::GetCurrentTexturePalette(UINT* p)
{ if (p) *p = 0; return D3DERR_INVALIDCALL; }

HRESULT STDMETHODCALLTYPE D9Device12::SetSoftwareVertexProcessing(BOOL bSoftware)
{

    m_softwareVP = bSoftware;
    return D3D_OK;
}
BOOL STDMETHODCALLTYPE D9Device12::GetSoftwareVertexProcessing()
{ return m_softwareVP; }

HRESULT STDMETHODCALLTYPE D9Device12::SetNPatchMode(float n)
{

    if (n >= 1.0f) return D3DERR_INVALIDCALL;
    m_nPatchMode = n;
    return D3D_OK;
}
float STDMETHODCALLTYPE D9Device12::GetNPatchMode() { return m_nPatchMode; }

HRESULT STDMETHODCALLTYPE D9Device12::ProcessVertices(
    UINT, UINT, UINT, IDirect3DVertexBuffer9*, IDirect3DVertexDeclaration9*, DWORD)
{

    static bool s_warned = false;
    if (!s_warned) {
        s_warned = true;
        DXLOG_WARN("[dx12] ProcessVertices is not implemented");
    }
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D9Device12::DrawRectPatch(UINT, CONST float*, CONST D3DRECTPATCH_INFO*)
{ return D3DERR_INVALIDCALL; }
HRESULT STDMETHODCALLTYPE D9Device12::DrawTriPatch(UINT, CONST float*, CONST D3DTRIPATCH_INFO*)
{ return D3DERR_INVALIDCALL; }
HRESULT STDMETHODCALLTYPE D9Device12::DeletePatch(UINT) { return D3DERR_INVALIDCALL; }

HRESULT STDMETHODCALLTYPE D9Device12::SetClipStatus(CONST D3DCLIPSTATUS9* pCS)
{
    if (!pCS) return D3DERR_INVALIDCALL;
    m_clipStatus = *pCS;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device12::GetClipStatus(D3DCLIPSTATUS9* pCS)
{
    if (!pCS) return D3DERR_INVALIDCALL;
    *pCS = m_clipStatus;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetTransform(D3DTRANSFORMSTATETYPE state,
                                                    CONST D3DMATRIX* pMatrix)
{
    if (!pMatrix) return D3DERR_INVALIDCALL;
    const DWORD s = static_cast<DWORD>(state);

    if (s >= D3DTS_WORLDMATRIX(0) && s <= D3DTS_WORLDMATRIX(7))
        m_fixed.world[s - D3DTS_WORLDMATRIX(0)] = *pMatrix;
    else if (state == D3DTS_VIEW)       m_fixed.view = *pMatrix;
    else if (state == D3DTS_PROJECTION) m_fixed.proj = *pMatrix;
    else if (s >= D3DTS_TEXTURE0 && s <= D3DTS_TEXTURE7)
        m_fixed.texMatrix[s - D3DTS_TEXTURE0] = *pMatrix;
    else
        return D3DERR_INVALIDCALL;

    m_ffpConstDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetTransform(D3DTRANSFORMSTATETYPE state,
                                                    D3DMATRIX* pOut)
{
    if (!pOut) return D3DERR_INVALIDCALL;
    const DWORD s = static_cast<DWORD>(state);

    if (s >= D3DTS_WORLDMATRIX(0) && s <= D3DTS_WORLDMATRIX(7))
        *pOut = m_fixed.world[s - D3DTS_WORLDMATRIX(0)];
    else if (state == D3DTS_VIEW)       *pOut = m_fixed.view;
    else if (state == D3DTS_PROJECTION) *pOut = m_fixed.proj;
    else if (s >= D3DTS_TEXTURE0 && s <= D3DTS_TEXTURE7)
        *pOut = m_fixed.texMatrix[s - D3DTS_TEXTURE0];
    else
        return D3DERR_INVALIDCALL;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::MultiplyTransform(D3DTRANSFORMSTATETYPE state,
                                                         CONST D3DMATRIX* pMatrix)
{
    if (!pMatrix) return D3DERR_INVALIDCALL;
    D3DMATRIX cur{};
    const HRESULT hr = GetTransform(state, &cur);
    if (FAILED(hr)) return hr;

    D3DMATRIX out{};
    const float* a = &cur._11;
    const float* b = &pMatrix->_11;
    float* o = &out._11;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            o[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] +
                           a[r * 4 + 1] * b[1 * 4 + c] +
                           a[r * 4 + 2] * b[2 * 4 + c] +
                           a[r * 4 + 3] * b[3 * 4 + c];
    return SetTransform(state, &out);
}

HRESULT STDMETHODCALLTYPE D9Device12::SetMaterial(CONST D3DMATERIAL9* pMat)
{
    if (!pMat) return D3DERR_INVALIDCALL;
    m_fixed.material = *pMat;
    m_ffpConstDirty  = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetMaterial(D3DMATERIAL9* pMat)
{
    if (!pMat) return D3DERR_INVALIDCALL;
    *pMat = m_fixed.material;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetLight(DWORD index, CONST D3DLIGHT9* pLight)
{
    if (!pLight || index >= 8) return D3DERR_INVALIDCALL;
    m_fixed.lights[index] = *pLight;
    m_ffpConstDirty       = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetLight(DWORD index, D3DLIGHT9* pLight)
{
    if (!pLight || index >= 8) return D3DERR_INVALIDCALL;
    *pLight = m_fixed.lights[index];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::LightEnable(DWORD index, BOOL enable)
{
    if (index >= 8) return D3DERR_INVALIDCALL;
    m_fixed.lightEnabled[index] = (enable != FALSE);
    m_ffpConstDirty             = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetLightEnable(DWORD index, BOOL* pEnable)
{
    if (!pEnable || index >= 8) return D3DERR_INVALIDCALL;
    *pEnable = m_fixed.lightEnabled[index] ? TRUE : FALSE;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetClipPlane(DWORD index, CONST float* pPlane)
{
    if (!pPlane || index >= 6) return D3DERR_INVALIDCALL;
    std::memcpy(m_clipPlanes[index], pPlane, sizeof(float) * 4);
    m_emuDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetClipPlane(DWORD index, float* pPlane)
{
    if (!pPlane || index >= 6) return D3DERR_INVALIDCALL;
    std::memcpy(pPlane, m_clipPlanes[index], sizeof(float) * 4);
    return D3D_OK;
}

void D9Device12::CaptureInto(D9StateBlock12::Snapshot& s) noexcept
{
    s.rs       = m_rst.State();
    s.ffp      = m_fixed;
    s.viewport = m_viewport;
    s.scissor  = m_scissor;
    s.fvf      = m_fvf;

    std::memcpy(s.vsF, m_vsConst.c,  sizeof(s.vsF));
    std::memcpy(s.vsI, m_vsConst.ic, sizeof(s.vsI));
    for (int i = 0; i < 16; ++i)
        s.vsB[i] = m_vsConst.bc[i / 4][i % 4] ? TRUE : FALSE;
    std::memcpy(s.psF, m_psConst.c,  sizeof(s.psF));
    std::memcpy(s.psI, m_psConst.ic, sizeof(s.psI));
    for (int i = 0; i < 16; ++i)
        s.psB[i] = m_psConst.bc[i / 4][i % 4] ? TRUE : FALSE;
    std::memcpy(s.clipPlanes, m_clipPlanes, sizeof(s.clipPlanes));

    auto hold = [](IUnknown* p) { if (p) p->AddRef(); };
    s.decl = m_rst.State().vertexDecl;   hold(s.decl);
    s.vs   = m_rst.State().vertexShader; hold(s.vs);
    s.ps   = m_rst.State().pixelShader;  hold(s.ps);
    for (auto* t : s.rs.textures) hold(t);
    for (auto* v : s.rs.streams)  hold(v);
    hold(s.rs.indexBuffer);
}

void D9Device12::ApplyFrom(const D9StateBlock12::Snapshot& s,
                           D3DSTATEBLOCKTYPE type) noexcept
{

    const bool all    = (type == D3DSBT_ALL);
    const bool pixel  = all || (type == D3DSBT_PIXELSTATE);
    const bool vertex = all || (type == D3DSBT_VERTEXSTATE);

    auto& live = m_rst.State();

    auto swapRef = [](auto*& slot, auto* want) {
        if (slot == want) return;
        if (want) want->AddRef();
        if (slot) slot->Release();
        slot = want;
    };

    std::memcpy(live.rs, s.rs.rs, sizeof(live.rs));

    if (pixel) {
        std::memcpy(live.tss,  s.rs.tss,  sizeof(live.tss));
        std::memcpy(live.samp, s.rs.samp, sizeof(live.samp));
        for (int i = 0; i < 8; ++i) swapRef(live.textures[i], s.rs.textures[i]);
        swapRef(live.pixelShader, s.ps);
        m_ps = static_cast<D9PixelShader12*>(live.pixelShader);

        std::memcpy(m_psConst.c,  s.psF, sizeof(s.psF));
        std::memcpy(m_psConst.ic, s.psI, sizeof(s.psI));
        for (int i = 0; i < 16; ++i) m_psConst.bc[i / 4][i % 4] = s.psB[i] ? 1u : 0u;
        m_psConstDirty = true;
    }

    if (vertex) {
        std::memcpy(live.streamStrides, s.rs.streamStrides, sizeof(live.streamStrides));
        std::memcpy(live.streamOffsets, s.rs.streamOffsets, sizeof(live.streamOffsets));
        for (int i = 0; i < 16; ++i) swapRef(live.streams[i], s.rs.streams[i]);
        if (!s.rs.indexBuffer && live.indexBuffer) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                DXLOG_WARN("[dx12] a state block Apply cleared the index buffer "
                           "(block type %d) - the block was captured while none "
                           "was bound", (int)type);
            }
        }
        swapRef(live.indexBuffer,  s.rs.indexBuffer);
        swapRef(live.vertexDecl,   s.decl);
        swapRef(live.vertexShader, s.vs);
        m_decl = static_cast<D9VertexDecl12*>(live.vertexDecl);
        m_vs   = static_cast<D9VertexShader12*>(live.vertexShader);
        m_fvf  = s.fvf;
        m_fvfDeclCached = 0xFFFFFFFF;

        std::memcpy(m_vsConst.c,  s.vsF, sizeof(s.vsF));
        std::memcpy(m_vsConst.ic, s.vsI, sizeof(s.vsI));
        for (int i = 0; i < 16; ++i) m_vsConst.bc[i / 4][i % 4] = s.vsB[i] ? 1u : 0u;
        m_vsConstDirty = true;

        m_fixed        = s.ffp;
        m_ffpConstDirty = true;
    }

    if (all) {
        m_viewport = s.viewport;
        m_scissor  = s.scissor;
        std::memcpy(m_clipPlanes, s.clipPlanes, sizeof(m_clipPlanes));
        m_emuDirty = true;
    }

    m_rst.MarkAllDirty();
}

HRESULT STDMETHODCALLTYPE D9Device12::SetConvolutionMonoKernel(UINT, UINT, float*, float*)
{ return D3DERR_INVALIDCALL; }
HRESULT STDMETHODCALLTYPE D9Device12::ComposeRects(
    IDirect3DSurface9*, IDirect3DSurface9*, IDirect3DVertexBuffer9*, UINT,
    IDirect3DVertexBuffer9*, D3DCOMPOSERECTSOP, int, int)
{ return D3DERR_INVALIDCALL; }
HRESULT STDMETHODCALLTYPE D9Device12::GetGPUThreadPriority(INT* p)
{ if (p) *p = 0; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device12::SetGPUThreadPriority(INT) { return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device12::WaitForVBlank(UINT)
{
    if (!m_ctx || !m_ctx->SwapChain()) return D3D_OK;
    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(m_ctx->SwapChain()->GetContainingOutput(output.GetAddressOf())) && output)
        output->WaitForVBlank();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CheckResourceResidency(IDirect3DResource9**, UINT32)
{ return S_OK; }
HRESULT STDMETHODCALLTYPE D9Device12::SetMaximumFrameLatency(UINT n)
{ m_maxFrameLatency = n ? n : 1; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device12::GetMaximumFrameLatency(UINT* p)
{ if (p) *p = m_maxFrameLatency; return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device12::CheckDeviceState(HWND)
{
    if (m_ctx && m_ctx->IsDeviceLost()) return D3DERR_DEVICELOST;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateRenderTargetEx(
    UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE MS, DWORD Q, BOOL L,
    IDirect3DSurface9** ppS, HANDLE* pH, DWORD)
{ return CreateRenderTarget(W, H, F, MS, Q, L, ppS, pH); }

HRESULT STDMETHODCALLTYPE D9Device12::CreateOffscreenPlainSurfaceEx(
    UINT W, UINT H, D3DFORMAT F, D3DPOOL P, IDirect3DSurface9** ppS, HANDLE* pH, DWORD)
{ return CreateOffscreenPlainSurface(W, H, F, P, ppS, pH); }

HRESULT STDMETHODCALLTYPE D9Device12::CreateDepthStencilSurfaceEx(
    UINT W, UINT H, D3DFORMAT F, D3DMULTISAMPLE_TYPE MS, DWORD Q, BOOL D,
    IDirect3DSurface9** ppS, HANDLE* pH, DWORD)
{ return CreateDepthStencilSurface(W, H, F, MS, Q, D, ppS, pH); }

HRESULT STDMETHODCALLTYPE D9Device12::ResetEx(D3DPRESENT_PARAMETERS* pPP, D3DDISPLAYMODEEX*)
{ return Reset(pPP); }

HRESULT STDMETHODCALLTYPE D9Device12::GetDisplayModeEx(
    UINT SC, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRot)
{
    D3DDISPLAYMODE basic{};
    const HRESULT hr = GetDisplayMode(SC, &basic);
    if (SUCCEEDED(hr) && pMode) {
        pMode->Size             = sizeof(D3DDISPLAYMODEEX);
        pMode->Width            = basic.Width;
        pMode->Height           = basic.Height;
        pMode->RefreshRate      = basic.RefreshRate;
        pMode->Format           = basic.Format;
        pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }
    if (pRot) *pRot = D3DDISPLAYROTATION_IDENTITY;
    return hr;
}

HRESULT STDMETHODCALLTYPE D9Device12::PresentEx(
    CONST RECT* pSrc, CONST RECT* pDst, HWND hDest, CONST RGNDATA* pDirty, DWORD)
{ return Present(pSrc, pDst, hDest, pDirty); }

}
