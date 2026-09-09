// D3D12 resource creation, locking and copying.
//
// Locking is harder here than under D3D11, which has no map-with-discard on a
// default-heap resource. A lockable resource therefore keeps a CPU-side copy,
// and its contents move to and from the GPU through the upload and readback
// paths explicitly.
//
// Releasing a resource does not destroy it. Anything referenced by a recorded
// command list must outlive the GPU's execution of that list, typically two or
// three frames after the game let go of it, so resources retire into a
// deferred queue and are freed once the fence shows their frame has completed.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy12/D9Device12.h>
#include <core/FormatConverter.h>
#include <core/Log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace mwon12 {

namespace {

struct SurfaceRef {
    TextureStorage12* storage{ nullptr };
    Resource12*       gpu{ nullptr };
    UINT              face{ 0 };
    UINT              level{ 0 };
    UINT              sub{ 0 };
    UINT              width{ 0 };
    UINT              height{ 0 };
    D3DFORMAT         format{ D3DFMT_UNKNOWN };
};

// D3DMULTISAMPLE_NONMASKABLE is 1 and does not mean "one sample" -- it means
// "any quality level the driver offers". Printing it as "1x MSAA" described a
// mode that does not exist. The message also fired per surface, so a game that
// creates render targets each time the resolution changes filled the log with
// the same gap. It is a gap, not a fault: CheckDeviceMultiSampleType already
// answers D3DERR_NOTAVAILABLE for everything but NONE, so a caller arriving
// here asked without checking -- in MW's case because the player turned
// anti-aliasing on in the game's own video options.
void NoteMultisampleIgnored(const char* who, D3DMULTISAMPLE_TYPE ms) noexcept
{
    if (ms == D3DMULTISAMPLE_NONE) return;

    static D3DMULTISAMPLE_TYPE s_reported[8]{};
    static unsigned            s_count = 0;
    for (unsigned i = 0; i < s_count; ++i)
        if (s_reported[i] == ms) return;
    if (s_count < 8) s_reported[s_count++] = ms;

    char what[32];
    if (ms == D3DMULTISAMPLE_NONMASKABLE)
        _snprintf_s(what, _TRUNCATE, "non-maskable");
    else
        _snprintf_s(what, _TRUNCATE, "%d-sample", (int)ms);

    DXLOG_WARN("[dx12] %s asked for %s multisampling; the DX12 backend does not "
               "implement MSAA, so the surface is created single-sampled and "
               "edges will not be antialiased. Turn anti-aliasing off in the "
               "game's video options to stop it being requested, or use "
               "Backend=2 for a backend that reports real sample counts.",
               who, what);
}

SurfaceRef Describe(D9Surface12* s) noexcept
{
    SurfaceRef r{};
    if (!s) return r;
    r.storage = s->Storage();
    r.gpu     = s->Gpu();
    r.face    = s->Face();
    r.level   = s->Level();
    r.sub     = s->Subresource();
    r.width   = s->Width();
    r.height  = s->Height();
    r.format  = s->Format();
    return r;
}

}

HRESULT STDMETHODCALLTYPE D9Device12::CreateTexture(
    UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
    D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE*)
{
    if (!ppTexture) return D3DERR_INVALIDCALL;
    *ppTexture = nullptr;
    D9Texture12* tex = nullptr;
    const HRESULT hr = D9Texture12::Create(this, Width, Height, Levels, Usage,
                                           Format, Pool, &tex);
    if (FAILED(hr)) return hr;
    *ppTexture = tex;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateVolumeTexture(
    UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
    D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE*)
{
    if (!ppVolumeTexture) return D3DERR_INVALIDCALL;
    *ppVolumeTexture = nullptr;
    D9VolumeTexture12* tex = nullptr;
    const HRESULT hr = D9VolumeTexture12::Create(this, Width, Height, Depth, Levels,
                                                 Usage, Format, Pool, &tex);
    if (FAILED(hr)) return hr;
    *ppVolumeTexture = tex;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateCubeTexture(
    UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DCubeTexture9** ppCubeTexture, HANDLE*)
{
    if (!ppCubeTexture) return D3DERR_INVALIDCALL;
    *ppCubeTexture = nullptr;
    D9CubeTexture12* tex = nullptr;
    const HRESULT hr = D9CubeTexture12::Create(this, EdgeLength, Levels, Usage,
                                               Format, Pool, &tex);
    if (FAILED(hr)) return hr;
    *ppCubeTexture = tex;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateVertexBuffer(
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
    IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE*)
{
    if (!ppVertexBuffer) return D3DERR_INVALIDCALL;
    *ppVertexBuffer = nullptr;
    D9VertexBuffer12* vb = nullptr;
    const HRESULT hr = D9VertexBuffer12::Create(this, Length, Usage, FVF, Pool, &vb);
    if (FAILED(hr)) return hr;
    *ppVertexBuffer = vb;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateIndexBuffer(
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE*)
{
    if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
    *ppIndexBuffer = nullptr;
    D9IndexBuffer12* ib = nullptr;
    const HRESULT hr = D9IndexBuffer12::Create(this, Length, Usage, Format, Pool, &ib);
    if (FAILED(hr)) return hr;
    *ppIndexBuffer = ib;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    DWORD, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE*)
{
    if (!ppSurface) return D3DERR_INVALIDCALL;
    *ppSurface = nullptr;
    NoteMultisampleIgnored("CreateRenderTarget", MultiSample);
    D9Surface12* s = nullptr;
    const HRESULT hr = D9Surface12::CreateStandalone(
        this, Width, Height, Format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT,
        Lockable, &s);
    if (FAILED(hr)) return hr;
    *ppSurface = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    DWORD, BOOL, IDirect3DSurface9** ppSurface, HANDLE*)
{
    if (!ppSurface) return D3DERR_INVALIDCALL;
    *ppSurface = nullptr;
    NoteMultisampleIgnored("CreateDepthStencilSurface", MultiSample);
    D9Surface12* s = nullptr;
    const HRESULT hr = D9Surface12::CreateStandalone(
        this, Width, Height, Format, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT,
        FALSE, &s);
    if (FAILED(hr)) return hr;
    *ppSurface = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateOffscreenPlainSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DSurface9** ppSurface, HANDLE*)
{
    if (!ppSurface) return D3DERR_INVALIDCALL;
    *ppSurface = nullptr;
    D9Surface12* s = nullptr;
    const HRESULT hr = D9Surface12::CreateStandalone(this, Width, Height, Format,
                                                     0, Pool, TRUE, &s);
    if (FAILED(hr)) return hr;
    *ppSurface = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateVertexDeclaration(
    CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl)
{
    if (!ppDecl || !pVertexElements) return D3DERR_INVALIDCALL;
    *ppDecl = nullptr;
    D9VertexDecl12* d = nullptr;
    const HRESULT hr = D9VertexDecl12::Create(this, pVertexElements, &d);
    if (FAILED(hr)) return hr;
    *ppDecl = d;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateVertexShader(
    CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader)
{
    if (!ppShader || !pFunction) return D3DERR_INVALIDCALL;
    *ppShader = nullptr;
    D9VertexShader12* s = nullptr;
    const HRESULT hr = D9VertexShader12::Create(this, pFunction, &s);
    if (FAILED(hr)) return hr;
    *ppShader = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreatePixelShader(
    CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader)
{
    if (!ppShader || !pFunction) return D3DERR_INVALIDCALL;
    *ppShader = nullptr;
    D9PixelShader12* s = nullptr;
    const HRESULT hr = D9PixelShader12::Create(this, pFunction, &s);
    if (FAILED(hr)) return hr;
    *ppShader = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateStateBlock(
    D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB)
{
    if (!ppSB) return D3DERR_INVALIDCALL;
    *ppSB = nullptr;
    if (m_recordingBlock) return D3DERR_INVALIDCALL;
    D9StateBlock12* sb = nullptr;
    const HRESULT hr = D9StateBlock12::CreateCaptured(this, Type, &sb);
    if (FAILED(hr)) return hr;
    *ppSB = sb;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::BeginStateBlock()
{
    if (m_recordingBlock) return D3DERR_INVALIDCALL;
    return D9StateBlock12::CreateRecording(this, &m_recordingBlock);
}

HRESULT STDMETHODCALLTYPE D9Device12::EndStateBlock(IDirect3DStateBlock9** ppSB)
{
    if (!ppSB) return D3DERR_INVALIDCALL;
    *ppSB = nullptr;
    if (!m_recordingBlock) return D3DERR_INVALIDCALL;

    *ppSB = m_recordingBlock;
    m_recordingBlock = nullptr;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateQuery(D3DQUERYTYPE Type,
                                                   IDirect3DQuery9** ppQuery)
{

    if (!ppQuery) return D9Query12::Supported(Type);
    *ppQuery = nullptr;
    D9Query12* q = nullptr;
    const HRESULT hr = D9Query12::Create(this, Type, &q);
    if (FAILED(hr)) return hr;
    *ppQuery = q;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetBackBuffer(
    UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
    IDirect3DSurface9** ppBackBuffer)
{
    if (!ppBackBuffer) return D3DERR_INVALIDCALL;
    *ppBackBuffer = nullptr;
    if (iSwapChain != 0 || Type != D3DBACKBUFFER_TYPE_MONO)
        return D3DERR_INVALIDCALL;

    if (iBackBuffer != 0) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            DXLOG_WARN("[dx12] GetBackBuffer(%u) refused: a flip-model swap chain "
                       "exposes only back buffer 0 to the application", iBackBuffer);
        }
        return D3DERR_INVALIDCALL;
    }
    if (!m_implicitBackBuffer) return D3DERR_INVALIDCALL;
    *ppBackBuffer = m_implicitBackBuffer;
    (*ppBackBuffer)->AddRef();
    return D3D_OK;
}

UINT STDMETHODCALLTYPE D9Device12::GetNumberOfSwapChains() { return 1; }

HRESULT STDMETHODCALLTYPE D9Device12::GetSwapChain(UINT iSwapChain,
                                                    IDirect3DSwapChain9** ppSwapChain)
{
    if (!ppSwapChain) return D3DERR_INVALIDCALL;
    *ppSwapChain = nullptr;
    if (iSwapChain != 0) return D3DERR_INVALIDCALL;

    static bool s_warned = false;
    if (!s_warned) {
        s_warned = true;
        DXLOG_WARN("[dx12] GetSwapChain is not implemented; use GetBackBuffer "
                   "and Present on the device");
    }
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D9Device12::CreateAdditionalSwapChain(
    D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9** ppSwapChain)
{
    if (ppSwapChain) *ppSwapChain = nullptr;
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetRenderTarget(DWORD RenderTargetIndex,
                                                       IDirect3DSurface9* pRenderTarget)
{
    if (RenderTargetIndex >= kMaxRenderTargets12) return D3DERR_INVALIDCALL;

    if (RenderTargetIndex == 0 && !pRenderTarget) return D3DERR_INVALIDCALL;

    auto* surf = static_cast<D9Surface12*>(pRenderTarget);
    if (surf && !(surf->Format() != D3DFMT_UNKNOWN)) return D3DERR_INVALIDCALL;

    if (m_renderTargets[RenderTargetIndex] == surf) return D3D_OK;
    if (surf) surf->AddRef();
    if (m_renderTargets[RenderTargetIndex]) m_renderTargets[RenderTargetIndex]->Release();
    m_renderTargets[RenderTargetIndex] = surf;
    m_targetsDirty = true;

    if (RenderTargetIndex == 0 && surf) {
        m_viewport.X      = 0;
        m_viewport.Y      = 0;
        m_viewport.Width  = surf->Width();
        m_viewport.Height = surf->Height();
        m_viewport.MinZ   = 0.0f;
        m_viewport.MaxZ   = 1.0f;
        m_scissor = RECT{ 0, 0, LONG(surf->Width()), LONG(surf->Height()) };

        m_emu.g_emuMisc[0] = surf->Width()  ? 1.0f / float(surf->Width())  : 0.0f;
        m_emu.g_emuMisc[1] = surf->Height() ? 1.0f / float(surf->Height()) : 0.0f;
        m_emuDirty = true;
        m_ffpConst.ViewportInfo[0] = m_emu.g_emuMisc[0];
        m_ffpConst.ViewportInfo[1] = m_emu.g_emuMisc[1];
        m_ffpConst.ViewportInfo[2] = 0.0f;
        m_ffpConst.ViewportInfo[3] = 0.0f;
        m_ffpConstDirty = true;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetRenderTarget(DWORD RenderTargetIndex,
                                                       IDirect3DSurface9** ppRenderTarget)
{
    if (!ppRenderTarget || RenderTargetIndex >= kMaxRenderTargets12)
        return D3DERR_INVALIDCALL;
    *ppRenderTarget = m_renderTargets[RenderTargetIndex];
    if (*ppRenderTarget) (*ppRenderTarget)->AddRef();
    return *ppRenderTarget ? D3D_OK : D3DERR_NOTFOUND;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil)
{
    auto* surf = static_cast<D9Surface12*>(pNewZStencil);
    if (m_depthStencil == surf) return D3D_OK;
    if (surf) surf->AddRef();
    if (m_depthStencil) m_depthStencil->Release();
    m_depthStencil = surf;
    m_targetsDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface)
{
    if (!ppZStencilSurface) return D3DERR_INVALIDCALL;
    *ppZStencilSurface = m_depthStencil;
    if (*ppZStencilSurface) (*ppZStencilSurface)->AddRef();
    return *ppZStencilSurface ? D3D_OK : D3DERR_NOTFOUND;
}

HRESULT STDMETHODCALLTYPE D9Device12::UpdateSurface(
    IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
    IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint)
{
    auto* src = static_cast<D9Surface12*>(pSourceSurface);
    auto* dst = static_cast<D9Surface12*>(pDestinationSurface);
    if (!src || !dst) return D3DERR_INVALIDCALL;
    if (src->Format() != dst->Format()) return D3DERR_INVALIDCALL;

    const SurfaceRef s = Describe(src);
    const SurfaceRef d = Describe(dst);

    RECT sr = pSourceRect ? *pSourceRect
                          : RECT{ 0, 0, LONG(s.width), LONG(s.height) };
    const LONG dx = pDestPoint ? pDestPoint->x : 0;
    const LONG dy = pDestPoint ? pDestPoint->y : 0;

    if (s.storage && s.storage->Placement() == Placement12::CpuOnly && d.gpu) {
        auto& stage = s.storage->Stage(s.face, s.level);
        if (stage.shadow.empty()) return D3DERR_INVALIDCALL;
        const UINT bpp = FormatConverter::BlockOrPixelSize(s.format);
        const bool bc  = FormatConverter::IsBlockCompressed(s.format);
        const size_t off = size_t(bc ? sr.top / 4 : sr.top) * stage.rowPitch +
                           size_t(bc ? sr.left / 4 : sr.left) * bpp;
        return d.gpu->UploadSubresourceRegion(
            d.sub, UINT(dx), UINT(dy), 0,
            UINT(sr.right - sr.left), UINT(sr.bottom - sr.top), 1,
            stage.shadow.data() + off, stage.rowPitch, stage.slicePitch);
    }

    if (!s.gpu || !d.gpu) return D3DERR_INVALIDCALL;

    D3D12_BOX box{};
    box.left   = UINT(sr.left);
    box.top    = UINT(sr.top);
    box.front  = 0;
    box.right  = UINT(sr.right);
    box.bottom = UINT(sr.bottom);
    box.back   = 1;
    return d.gpu->CopySubresourceFrom(*s.gpu, s.sub, d.sub, &box,
                                      UINT(dx), UINT(dy), 0);
}

HRESULT STDMETHODCALLTYPE D9Device12::UpdateTexture(
    IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture)
{
    if (!pSourceTexture || !pDestinationTexture) return D3DERR_INVALIDCALL;

    auto storageOf = [](IDirect3DBaseTexture9* t) -> TextureStorage12* {
        switch (t->GetType()) {
        case D3DRTYPE_TEXTURE:       return static_cast<D9Texture12*>(t)->Storage();
        case D3DRTYPE_CUBETEXTURE:   return static_cast<D9CubeTexture12*>(t)->Storage();
        case D3DRTYPE_VOLUMETEXTURE: return static_cast<D9VolumeTexture12*>(t)->Storage();
        default:                     return nullptr;
        }
    };

    TextureStorage12* s = storageOf(pSourceTexture);
    TextureStorage12* d = storageOf(pDestinationTexture);
    if (!s || !d) return D3DERR_INVALIDCALL;
    if (s->Format() != d->Format()) return D3DERR_INVALIDCALL;

    const UINT levels = std::min(s->Levels(), d->Levels());
    const UINT faces  = std::min(s->Faces(),  d->Faces());
    HRESULT hr = D3D_OK;

    for (UINT f = 0; f < faces; ++f) {
        for (UINT l = 0; l < levels; ++l) {
            if (s->Placement() == Placement12::CpuOnly) {
                auto& stage = s->Stage(f, l);
                if (stage.shadow.empty() || !d->HasGpu()) continue;
                const HRESULT one = d->Gpu().UploadSubresource(
                    d->Subresource(f, l), stage.shadow.data(),
                    stage.rowPitch, stage.slicePitch);
                if (FAILED(one)) hr = one;
            } else if (s->HasGpu() && d->HasGpu()) {
                const HRESULT one = d->Gpu().CopySubresourceFrom(
                    s->Gpu(), s->Subresource(f, l), d->Subresource(f, l),
                    nullptr, 0, 0, 0);
                if (FAILED(one)) hr = one;
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetRenderTargetData(
    IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface)
{
    auto* src = static_cast<D9Surface12*>(pRenderTarget);
    auto* dst = static_cast<D9Surface12*>(pDestSurface);
    if (!src || !dst) return D3DERR_INVALIDCALL;
    if (src->Format() != dst->Format()) return D3DERR_INVALIDCALL;
    if (src->Width() != dst->Width() || src->Height() != dst->Height())
        return D3DERR_INVALIDCALL;

    Resource12* gpu = src->Gpu();
    if (!gpu) return D3DERR_INVALIDCALL;

    const SurfaceRef d = Describe(dst);
    if (d.storage && d.storage->Placement() == Placement12::CpuOnly) {
        auto& stage = d.storage->Stage(d.face, d.level);
        if (stage.shadow.empty()) {
            stage.rowPitch   = d.storage->LevelRowPitch(d.level);
            stage.slicePitch = d.storage->LevelSlicePitch(d.level);
            stage.shadow.assign(stage.slicePitch, 0);
        }
        stage.everWritten = true;
        return gpu->ReadbackSubresource(src->Subresource(), stage.shadow.data(),
                                        stage.rowPitch, stage.slicePitch);
    }

    if (Resource12* dgpu = dst->Gpu())
        return dgpu->CopySubresourceFrom(*gpu, src->Subresource(),
                                         dst->Subresource(), nullptr, 0, 0, 0);
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetFrontBufferData(UINT, IDirect3DSurface9* pDestSurface)
{

    if (!pDestSurface || !m_ctx) return D3DERR_INVALIDCALL;
    if (!m_implicitBackBuffer) return D3DERR_INVALIDCALL;
    return GetRenderTargetData(m_implicitBackBuffer, pDestSurface);
}

HRESULT STDMETHODCALLTYPE D9Device12::StretchRect(
    IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
    IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect,
    D3DTEXTUREFILTERTYPE Filter)
{
    auto* src = static_cast<D9Surface12*>(pSourceSurface);
    auto* dst = static_cast<D9Surface12*>(pDestSurface);
    if (!src || !dst || !m_ctx) return D3DERR_INVALIDCALL;

    const SurfaceRef s = Describe(src);
    const SurfaceRef d = Describe(dst);
    if (!s.gpu || !d.gpu) return D3DERR_INVALIDCALL;

    const RECT sr = pSourceRect ? *pSourceRect : RECT{ 0, 0, LONG(s.width), LONG(s.height) };
    const RECT dr = pDestRect   ? *pDestRect   : RECT{ 0, 0, LONG(d.width), LONG(d.height) };

    const bool sameSize   = (sr.right - sr.left) == (dr.right - dr.left) &&
                            (sr.bottom - sr.top) == (dr.bottom - dr.top);
    const bool sameFormat = s.format == d.format;

    auto* cmd = m_ctx->CmdList();

    if (sameSize && sameFormat) {
        D3D12_BOX box{};
        box.left   = UINT(sr.left);
        box.top    = UINT(sr.top);
        box.front  = 0;
        box.right  = UINT(sr.right);
        box.bottom = UINT(sr.bottom);
        box.back   = 1;
        return d.gpu->CopySubresourceFrom(*s.gpu, s.sub, d.sub, &box,
                                          UINT(dr.left), UINT(dr.top), 0);
    }

    if (!s.storage || !d.storage) return D3DERR_INVALIDCALL;
    if (!(d.storage->Usage() & D3DUSAGE_RENDERTARGET)) {
        DXLOG_WARN("[dx12] StretchRect needs to scale into a surface that is not "
                   "a render target (fmt=%d %ux%u); the operation is skipped",
                   (int)d.format, d.width, d.height);
        return D3DERR_INVALIDCALL;
    }

    const auto srcSrv = s.storage->Srv(false);
    const auto dstRtv = d.storage->Rtv(d.face, d.level, false);
    if (srcSrv.ptr == SIZE_T(-1) || dstRtv.ptr == SIZE_T(-1))
        return D3DERR_INVALIDCALL;

    m_barrierScratch.clear();
    s.gpu->AppendTransition(m_barrierScratch, s.sub,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d.gpu->AppendTransition(m_barrierScratch, d.sub,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!m_barrierScratch.empty())
        cmd->ResourceBarrier(static_cast<UINT>(m_barrierScratch.size()),
                             m_barrierScratch.data());

    const HRESULT hr = m_blitter.Blit(
        dstRtv, d.storage->DxgiResourceFormat(), d.width, d.height, dr,
        srcSrv, s.width, s.height, sr, Filter != D3DTEXF_NONE && Filter != D3DTEXF_POINT);

    m_targetsDirty = true;
    m_boundPso     = nullptr;
    m_boundTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    return hr;
}

HRESULT STDMETHODCALLTYPE D9Device12::ColorFill(
    IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color)
{
    auto* surf = static_cast<D9Surface12*>(pSurface);
    if (!surf || !m_ctx) return D3DERR_INVALIDCALL;

    const SurfaceRef s = Describe(surf);
    if (!s.gpu) return D3DERR_INVALIDCALL;

    const auto rtv = surf->Rtv(false);
    if (rtv.ptr == SIZE_T(-1)) {
        DXLOG_WARN("[dx12] ColorFill on a surface that is not a render target "
                   "(fmt=%d) is not supported", (int)s.format);
        return D3DERR_INVALIDCALL;
    }

    m_barrierScratch.clear();
    surf->AppendTransition(m_barrierScratch, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!m_barrierScratch.empty())
        m_ctx->CmdList()->ResourceBarrier(
            static_cast<UINT>(m_barrierScratch.size()), m_barrierScratch.data());

    const float rgba[4] = {
        ((color >> 16) & 0xFF) / 255.0f,
        ((color >>  8) & 0xFF) / 255.0f,
        ((color      ) & 0xFF) / 255.0f,
        ((color >> 24) & 0xFF) / 255.0f,
    };
    if (pRect) {
        const D3D12_RECT r{ pRect->left, pRect->top, pRect->right, pRect->bottom };
        m_ctx->CmdList()->ClearRenderTargetView(rtv, rgba, 1, &r);
    } else {
        m_ctx->CmdList()->ClearRenderTargetView(rtv, rgba, 0, nullptr);
    }
    return D3D_OK;
}

HRESULT D9Device12::GenerateMips(TextureStorage12* storage) noexcept
{
    if (!storage || !storage->HasGpu() || storage->Levels() <= 1)
        return D3D_OK;

    if (!(storage->Usage() & D3DUSAGE_RENDERTARGET)) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            DXLOG_WARN("[dx12] GenerateMipSubLevels needs a render-target-capable "
                       "texture; levels 1..N of non-renderable textures keep "
                       "whatever the game uploaded");
        }
        return D3D_OK;
    }

    HRESULT hr = storage->FlushDirty();
    if (FAILED(hr)) return hr;

    for (UINT face = 0; face < storage->Faces(); ++face) {
        hr = m_blitter.GenerateMipChain(storage->Gpu(),
                                        storage->DxgiResourceFormat(),
                                        storage->Width(), storage->Height(),
                                        storage->Levels(), face, storage->Faces());
        if (FAILED(hr)) return hr;
    }

    m_targetsDirty  = true;
    m_boundPso      = nullptr;
    m_boundTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    return D3D_OK;
}

}
