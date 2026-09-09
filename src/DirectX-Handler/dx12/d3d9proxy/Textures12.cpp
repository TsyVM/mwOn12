// The D3D12 texture and surface proxies.
//
// Implement the D3D9 texture interfaces over TextureStorage12, including
// surface proxies for individual mip levels and cube faces, and the lock path
// that moves texel data between the CPU copy and the GPU resource.
//
// Back-buffer surfaces resolve the current swap chain buffer at the point of
// use rather than capturing one at creation. A swap chain rotates through
// several physical buffers while D3D9 presents the game with a single logical
// back buffer, and games cache that pointer for the life of the device -- so a
// captured buffer goes stale immediately, and writing to the buffer currently
// being displayed is rejected outright.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy12/Resources12.h>
#include <d3d9proxy12/D9Device12.h>
#include <core/DeviceContext12.h>
#include <core/FormatConverter.h>
#include <core/Log.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace mwon12 {

namespace {

void FillSurfaceDesc(D3DSURFACE_DESC& d, const TextureStorage12& s,
                     UINT level, D3DRESOURCETYPE type) noexcept
{
    UINT w = 0, h = 0, dep = 0;
    s.LevelSize(level, &w, &h, &dep);
    d.Format             = s.Format();
    d.Type               = type;
    d.Usage              = s.Usage();
    d.Pool               = s.Pool();
    d.MultiSampleType    = D3DMULTISAMPLE_NONE;
    d.MultiSampleQuality = 0;
    d.Width              = w;
    d.Height             = h;
}

}

D9Surface12::D9Surface12(D9Device12* dev) noexcept : m_dev(dev)
{
    for (auto& h : m_bbRtv) h.ptr = SIZE_T(-1);
}

D9Surface12::D9Surface12(D9Device12* dev, IDirect3DBaseTexture9* container,
                         TextureStorage12* storage, UINT face, UINT level) noexcept
    : m_dev(dev), m_container(container), m_storage(storage),
      m_face(face), m_level(level)
{
    if (storage) {
        UINT d = 0;
        storage->LevelSize(level, &m_width, &m_height, &d);
        m_format = storage->Format();
        m_usage  = storage->Usage();
        m_pool   = storage->Pool();
    }
}

D9Surface12::~D9Surface12()
{
    if (m_backBufferIndex >= 0 && m_dev && m_dev->Ctx()) {
        for (auto& h : m_bbRtv) {
            if (h.ptr != SIZE_T(-1)) {
                m_dev->Ctx()->RtvHeap().Free(h);
                h.ptr = SIZE_T(-1);
            }
        }
    }
}

HRESULT D9Surface12::CreateStandalone(D9Device12* dev, UINT w, UINT h,
                                      D3DFORMAT fmt, DWORD usage, D3DPOOL pool,
                                      BOOL lockable, D9Surface12** out) noexcept
{
    if (!out || !dev) return D3DERR_INVALIDCALL;
    *out = nullptr;

    auto* s = new (std::nothrow) D9Surface12(dev);
    if (!s) return E_OUTOFMEMORY;

    s->m_ownStorage = std::make_unique<TextureStorage12>();
    s->m_storage    = s->m_ownStorage.get();
    s->m_lockable   = lockable != FALSE;

    const HRESULT hr = s->m_ownStorage->Init(dev->Ctx(), D3DRTYPE_SURFACE,
                                             w, h, 1, 1, usage, fmt, pool, 1);
    if (FAILED(hr)) { delete s; return hr; }

    s->m_width  = w;
    s->m_height = h;
    s->m_format = fmt;
    s->m_usage  = usage;
    s->m_pool   = pool;
    *out = s;
    return D3D_OK;
}

HRESULT D9Surface12::CreateBackBuffer(D9Device12* dev, D9Surface12** out) noexcept
{
    if (!out || !dev || !dev->Ctx()) return D3DERR_INVALIDCALL;
    *out = nullptr;

    DeviceContext12* ctx = dev->Ctx();

    auto* s = new (std::nothrow) D9Surface12(dev);
    if (!s) return E_OUTOFMEMORY;

    s->m_backBufferIndex = 0;
    s->m_width    = ctx->BackBufferWidth();
    s->m_height   = ctx->BackBufferHeight();
    s->m_format   = ctx->BackBufferD3D9Format();
    s->m_usage    = D3DUSAGE_RENDERTARGET;
    s->m_pool     = D3DPOOL_DEFAULT;
    s->m_lockable = true;

    D3D12_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format             = ctx->BackBufferFormat();
    rd.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
    rd.Texture2D.MipSlice = 0;
    for (UINT i = 0; i < k_dx12FrameCount; ++i) {
        ID3D12Resource* res = ctx->BackBuffer(i);
        if (!res) { delete s; return D3DERR_INVALIDCALL; }
        s->m_bbRtv[i] = ctx->RtvHeap().Alloc();
        if (s->m_bbRtv[i].ptr == SIZE_T(-1)) { delete s; return E_OUTOFMEMORY; }
        ctx->Device()->CreateRenderTargetView(res, &rd, s->m_bbRtv[i]);
    }

    s->SyncBackBuffer();
    *out = s;
    return D3D_OK;
}

UINT D9Surface12::CurrentBackBufferIndex() const noexcept
{
    return (m_dev && m_dev->Ctx()) ? m_dev->Ctx()->BackBufferIndex() : 0u;
}

void D9Surface12::SyncBackBuffer() noexcept
{
    if (m_backBufferIndex < 0 || !m_dev || !m_dev->Ctx()) return;
    DeviceContext12* ctx = m_dev->Ctx();
    const UINT idx = ctx->BackBufferIndex();
    if (idx == m_bbAdopted) return;
    ID3D12Resource* res = ctx->BackBuffer(idx);
    if (!res) return;

    m_backBufferRes.Adopt(ctx, res, ctx->BackBufferState(idx),
                          static_cast<int>(idx));
    m_bbAdopted = idx;
}

HRESULT STDMETHODCALLTYPE D9Surface12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DSurface9)) {
        *ppv = static_cast<IDirect3DSurface9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9Surface12::AddRef()
{

    if (m_container) return m_container->AddRef();
    return m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE D9Surface12::Release()
{
    if (m_container) return m_container->Release();
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        delete this;
    }
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9Surface12::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Surface12::SetPrivateData(REFGUID g, CONST void* d, DWORD s, DWORD f)
{ return m_privateData.Set(g, d, s, f); }
HRESULT STDMETHODCALLTYPE D9Surface12::GetPrivateData(REFGUID g, void* d, DWORD* s)
{ return m_privateData.Get(g, d, s); }
HRESULT STDMETHODCALLTYPE D9Surface12::FreePrivateData(REFGUID g)
{ return m_privateData.Free(g); }
DWORD STDMETHODCALLTYPE D9Surface12::SetPriority(DWORD) { return 0; }
DWORD STDMETHODCALLTYPE D9Surface12::GetPriority() { return 0; }
void  STDMETHODCALLTYPE D9Surface12::PreLoad() {}
D3DRESOURCETYPE STDMETHODCALLTYPE D9Surface12::GetType() { return D3DRTYPE_SURFACE; }

HRESULT STDMETHODCALLTYPE D9Surface12::GetContainer(REFIID riid, void** ppContainer)
{
    if (!ppContainer) return D3DERR_INVALIDCALL;
    if (m_container)
        return m_container->QueryInterface(riid, ppContainer);

    if (m_dev)
        return m_dev->QueryInterface(riid, ppContainer);
    *ppContainer = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D9Surface12::GetDesc(D3DSURFACE_DESC* pDesc)
{
    if (!pDesc) return D3DERR_INVALIDCALL;
    std::memset(pDesc, 0, sizeof(*pDesc));
    pDesc->Format             = m_format;
    pDesc->Type               = D3DRTYPE_SURFACE;
    pDesc->Usage              = m_usage;
    pDesc->Pool               = m_pool;
    pDesc->MultiSampleType    = D3DMULTISAMPLE_NONE;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width              = m_width;
    pDesc->Height             = m_height;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Surface12::LockRect(D3DLOCKED_RECT* pLockedRect,
                                                CONST RECT* pRect, DWORD Flags)
{
    if (!pLockedRect) return D3DERR_INVALIDCALL;
    pLockedRect->pBits = nullptr;
    pLockedRect->Pitch = 0;

    if (m_backBufferIndex >= 0) {

        if (!m_dev || !m_dev->Ctx()) return D3DERR_INVALIDCALL;
        const UINT pitch = FormatConverter::RowPitch(m_format, m_width);
        const UINT rows  = FormatConverter::RowCount(m_format, m_height);
        m_bbStage.rowPitch   = pitch;
        m_bbStage.slicePitch = pitch * rows;
        if (m_bbStage.shadow.size() < size_t(pitch) * rows)
            m_bbStage.shadow.assign(size_t(pitch) * rows, 0);
        if (!(Flags & D3DLOCK_DISCARD)) {
            const HRESULT hr = m_backBufferRes.ReadbackSubresource(
                0, m_bbStage.shadow.data(), pitch, pitch * rows);
            if (FAILED(hr)) return hr;
        }
        m_bbStage.locked    = true;
        m_bbStage.lockFlags = Flags;
        pLockedRect->pBits  = m_bbStage.shadow.data();
        pLockedRect->Pitch  = static_cast<INT>(pitch);
        return D3D_OK;
    }

    if (!m_storage) return D3DERR_INVALIDCALL;
    void* bits = nullptr;
    UINT  rowPitch = 0, slicePitch = 0;
    const HRESULT hr = m_storage->LockRegion(m_face, m_level, pRect, nullptr,
                                             Flags, &bits, &rowPitch, &slicePitch);
    if (FAILED(hr)) return hr;
    pLockedRect->pBits = bits;
    pLockedRect->Pitch = static_cast<INT>(rowPitch);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Surface12::UnlockRect()
{
    if (m_backBufferIndex >= 0) {
        if (!m_bbStage.locked) return D3DERR_INVALIDCALL;
        m_bbStage.locked = false;
        if (!(m_bbStage.lockFlags & D3DLOCK_READONLY)) {

            m_backBufferRes.UploadSubresource(0, m_bbStage.shadow.data(),
                                              m_bbStage.rowPitch,
                                              m_bbStage.slicePitch);
        }
        return D3D_OK;
    }
    if (!m_storage) return D3DERR_INVALIDCALL;
    return m_storage->UnlockRegion(m_face, m_level);
}

HRESULT STDMETHODCALLTYPE D9Surface12::GetDC(HDC*)      { return D3DERR_INVALIDCALL; }
HRESULT STDMETHODCALLTYPE D9Surface12::ReleaseDC(HDC)   { return D3DERR_INVALIDCALL; }

Resource12* D9Surface12::Gpu() const noexcept
{
    if (m_backBufferIndex >= 0) {
        const_cast<D9Surface12*>(this)->SyncBackBuffer();
        return const_cast<Resource12*>(&m_backBufferRes);
    }
    return m_storage && m_storage->HasGpu() ? &m_storage->Gpu() : nullptr;
}

UINT D9Surface12::Subresource() const noexcept
{
    if (m_backBufferIndex >= 0) return 0;
    return m_storage ? m_storage->Subresource(m_face, m_level) : 0;
}

D3D12_CPU_DESCRIPTOR_HANDLE D9Surface12::Rtv(bool srgb) noexcept
{
    if (m_backBufferIndex >= 0) return m_bbRtv[CurrentBackBufferIndex()];
    return m_storage ? m_storage->Rtv(m_face, m_level, srgb)
                     : D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };
}

D3D12_CPU_DESCRIPTOR_HANDLE D9Surface12::Dsv(bool readOnly) noexcept
{
    if (m_backBufferIndex >= 0) return D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };
    return m_storage ? m_storage->Dsv(m_face, m_level, readOnly)
                     : D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };
}

void D9Surface12::AppendTransition(std::vector<D3D12_RESOURCE_BARRIER>& out,
                                   D3D12_RESOURCE_STATES after) noexcept
{
    if (m_backBufferIndex >= 0) {

        if (!m_dev || !m_dev->Ctx()) return;
        DeviceContext12* ctx = m_dev->Ctx();
        SyncBackBuffer();
        const UINT idx = ctx->BackBufferIndex();
        const D3D12_RESOURCE_STATES before = ctx->BackBufferState(idx);
        if (before == after) return;
        out.push_back(dx12::TransitionBarrier(ctx->BackBuffer(idx), before, after));
        ctx->SetBackBufferState(idx, after);
        m_backBufferRes.OverrideState(D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, after);
        return;
    }
    if (m_storage && m_storage->HasGpu())
        m_storage->Gpu().AppendTransitionPlanes(out, Subresource(), after);
}

D9Texture12::D9Texture12(D9Device12* dev) noexcept : m_dev(dev) {}
D9Texture12::~D9Texture12() = default;

HRESULT D9Texture12::Create(D9Device12* dev, UINT w, UINT h, UINT levels,
                            DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                            D9Texture12** out) noexcept
{
    if (!out || !dev) return D3DERR_INVALIDCALL;
    *out = nullptr;
    auto* t = new (std::nothrow) D9Texture12(dev);
    if (!t) return E_OUTOFMEMORY;

    const HRESULT hr = t->m_storage.Init(dev->Ctx(), D3DRTYPE_TEXTURE,
                                         w, h, 1, levels, usage, fmt, pool, 1);
    if (FAILED(hr)) { delete t; return hr; }

    t->m_surfaces.resize(t->m_storage.Levels());
    *out = t;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Texture12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DBaseTexture9) || riid == __uuidof(IDirect3DTexture9)) {
        *ppv = static_cast<IDirect3DTexture9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9Texture12::AddRef()
{ return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE D9Texture12::Release()
{
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9Texture12::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Texture12::SetPrivateData(REFGUID g, CONST void* d, DWORD s, DWORD f)
{ return m_privateData.Set(g, d, s, f); }
HRESULT STDMETHODCALLTYPE D9Texture12::GetPrivateData(REFGUID g, void* d, DWORD* s)
{ return m_privateData.Get(g, d, s); }
HRESULT STDMETHODCALLTYPE D9Texture12::FreePrivateData(REFGUID g)
{ return m_privateData.Free(g); }
DWORD STDMETHODCALLTYPE D9Texture12::SetPriority(DWORD) { return 0; }
DWORD STDMETHODCALLTYPE D9Texture12::GetPriority() { return 0; }
void  STDMETHODCALLTYPE D9Texture12::PreLoad() { m_storage.FlushDirty(); }
D3DRESOURCETYPE STDMETHODCALLTYPE D9Texture12::GetType() { return D3DRTYPE_TEXTURE; }

DWORD STDMETHODCALLTYPE D9Texture12::SetLOD(DWORD lod)
{

    if (m_storage.Pool() != D3DPOOL_MANAGED) return 0;
    const DWORD prev = m_lod;
    m_lod = std::min<DWORD>(lod, m_storage.Levels() ? m_storage.Levels() - 1 : 0);
    return prev;
}
DWORD STDMETHODCALLTYPE D9Texture12::GetLOD()
{ return m_storage.Pool() == D3DPOOL_MANAGED ? m_lod : 0; }
DWORD STDMETHODCALLTYPE D9Texture12::GetLevelCount() { return m_storage.Levels(); }

HRESULT STDMETHODCALLTYPE D9Texture12::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE t)
{ m_autoGenFilter = t; return D3D_OK; }
D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE D9Texture12::GetAutoGenFilterType()
{ return m_autoGenFilter; }

void STDMETHODCALLTYPE D9Texture12::GenerateMipSubLevels()
{
    if (m_dev) m_dev->GenerateMips(&m_storage);
}

HRESULT STDMETHODCALLTYPE D9Texture12::GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc)
{
    if (!pDesc || Level >= m_storage.Levels()) return D3DERR_INVALIDCALL;
    std::memset(pDesc, 0, sizeof(*pDesc));
    FillSurfaceDesc(*pDesc, m_storage, Level, D3DRTYPE_SURFACE);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Texture12::GetSurfaceLevel(UINT Level,
                                                       IDirect3DSurface9** ppSurfaceLevel)
{
    if (!ppSurfaceLevel) return D3DERR_INVALIDCALL;
    *ppSurfaceLevel = nullptr;
    if (Level >= m_storage.Levels()) return D3DERR_INVALIDCALL;

    if (!m_surfaces[Level]) {
        m_surfaces[Level] = D9Surface12::Owned(new (std::nothrow) D9Surface12(
            m_dev, static_cast<IDirect3DBaseTexture9*>(this), &m_storage, 0u, Level));
        if (!m_surfaces[Level]) return E_OUTOFMEMORY;
    }
    *ppSurfaceLevel = m_surfaces[Level].get();
    (*ppSurfaceLevel)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Texture12::LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect,
                                                 CONST RECT* pRect, DWORD Flags)
{
    if (!pLockedRect) return D3DERR_INVALIDCALL;
    pLockedRect->pBits = nullptr;
    pLockedRect->Pitch = 0;
    void* bits = nullptr;
    UINT rowPitch = 0, slicePitch = 0;
    const HRESULT hr = m_storage.LockRegion(0, Level, pRect, nullptr, Flags,
                                            &bits, &rowPitch, &slicePitch);
    if (FAILED(hr)) return hr;
    pLockedRect->pBits = bits;
    pLockedRect->Pitch = static_cast<INT>(rowPitch);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Texture12::UnlockRect(UINT Level)
{ return m_storage.UnlockRegion(0, Level); }

HRESULT STDMETHODCALLTYPE D9Texture12::AddDirtyRect(CONST RECT* pDirtyRect)
{
    m_storage.AddDirty(0, 0, pDirtyRect);
    return D3D_OK;
}

D9CubeTexture12::D9CubeTexture12(D9Device12* dev) noexcept : m_dev(dev) {}
D9CubeTexture12::~D9CubeTexture12() = default;

HRESULT D9CubeTexture12::Create(D9Device12* dev, UINT edge, UINT levels, DWORD usage,
                                D3DFORMAT fmt, D3DPOOL pool,
                                D9CubeTexture12** out) noexcept
{
    if (!out || !dev) return D3DERR_INVALIDCALL;
    *out = nullptr;
    auto* t = new (std::nothrow) D9CubeTexture12(dev);
    if (!t) return E_OUTOFMEMORY;

    const HRESULT hr = t->m_storage.Init(dev->Ctx(), D3DRTYPE_CUBETEXTURE,
                                         edge, edge, 1, levels, usage, fmt, pool, 6);
    if (FAILED(hr)) { delete t; return hr; }

    t->m_surfaces.resize(size_t(t->m_storage.Levels()) * 6);
    *out = t;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DBaseTexture9) || riid == __uuidof(IDirect3DCubeTexture9)) {
        *ppv = static_cast<IDirect3DCubeTexture9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9CubeTexture12::AddRef()
{ return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE D9CubeTexture12::Release()
{
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture12::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture12::SetPrivateData(REFGUID g, CONST void* d, DWORD s, DWORD f)
{ return m_privateData.Set(g, d, s, f); }
HRESULT STDMETHODCALLTYPE D9CubeTexture12::GetPrivateData(REFGUID g, void* d, DWORD* s)
{ return m_privateData.Get(g, d, s); }
HRESULT STDMETHODCALLTYPE D9CubeTexture12::FreePrivateData(REFGUID g)
{ return m_privateData.Free(g); }
DWORD STDMETHODCALLTYPE D9CubeTexture12::SetPriority(DWORD) { return 0; }
DWORD STDMETHODCALLTYPE D9CubeTexture12::GetPriority() { return 0; }
void  STDMETHODCALLTYPE D9CubeTexture12::PreLoad() { m_storage.FlushDirty(); }
D3DRESOURCETYPE STDMETHODCALLTYPE D9CubeTexture12::GetType() { return D3DRTYPE_CUBETEXTURE; }

DWORD STDMETHODCALLTYPE D9CubeTexture12::SetLOD(DWORD lod)
{
    if (m_storage.Pool() != D3DPOOL_MANAGED) return 0;
    const DWORD prev = m_lod;
    m_lod = std::min<DWORD>(lod, m_storage.Levels() ? m_storage.Levels() - 1 : 0);
    return prev;
}
DWORD STDMETHODCALLTYPE D9CubeTexture12::GetLOD()
{ return m_storage.Pool() == D3DPOOL_MANAGED ? m_lod : 0; }
DWORD STDMETHODCALLTYPE D9CubeTexture12::GetLevelCount() { return m_storage.Levels(); }

HRESULT STDMETHODCALLTYPE D9CubeTexture12::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE t)
{ m_autoGenFilter = t; return D3D_OK; }
D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE D9CubeTexture12::GetAutoGenFilterType()
{ return m_autoGenFilter; }
void STDMETHODCALLTYPE D9CubeTexture12::GenerateMipSubLevels()
{ if (m_dev) m_dev->GenerateMips(&m_storage); }

HRESULT STDMETHODCALLTYPE D9CubeTexture12::GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc)
{
    if (!pDesc || Level >= m_storage.Levels()) return D3DERR_INVALIDCALL;
    std::memset(pDesc, 0, sizeof(*pDesc));
    FillSurfaceDesc(*pDesc, m_storage, Level, D3DRTYPE_SURFACE);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture12::GetCubeMapSurface(
    D3DCUBEMAP_FACES Face, UINT Level, IDirect3DSurface9** ppSurfaceLevel)
{
    if (!ppSurfaceLevel) return D3DERR_INVALIDCALL;
    *ppSurfaceLevel = nullptr;
    const UINT face = static_cast<UINT>(Face);
    if (face >= 6 || Level >= m_storage.Levels()) return D3DERR_INVALIDCALL;

    const size_t idx = size_t(face) * m_storage.Levels() + Level;
    if (!m_surfaces[idx]) {
        m_surfaces[idx] = D9Surface12::Owned(new (std::nothrow) D9Surface12(
            m_dev, static_cast<IDirect3DBaseTexture9*>(this), &m_storage, face, Level));
        if (!m_surfaces[idx]) return E_OUTOFMEMORY;
    }
    *ppSurfaceLevel = m_surfaces[idx].get();
    (*ppSurfaceLevel)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture12::LockRect(
    D3DCUBEMAP_FACES Face, UINT Level, D3DLOCKED_RECT* pLockedRect,
    CONST RECT* pRect, DWORD Flags)
{
    if (!pLockedRect) return D3DERR_INVALIDCALL;
    pLockedRect->pBits = nullptr;
    pLockedRect->Pitch = 0;
    void* bits = nullptr;
    UINT rowPitch = 0, slicePitch = 0;
    const HRESULT hr = m_storage.LockRegion(static_cast<UINT>(Face), Level, pRect,
                                            nullptr, Flags, &bits, &rowPitch, &slicePitch);
    if (FAILED(hr)) return hr;
    pLockedRect->pBits = bits;
    pLockedRect->Pitch = static_cast<INT>(rowPitch);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture12::UnlockRect(D3DCUBEMAP_FACES Face, UINT Level)
{ return m_storage.UnlockRegion(static_cast<UINT>(Face), Level); }

HRESULT STDMETHODCALLTYPE D9CubeTexture12::AddDirtyRect(D3DCUBEMAP_FACES Face,
                                                         CONST RECT* pDirtyRect)
{
    m_storage.AddDirty(static_cast<UINT>(Face), 0, pDirtyRect);
    return D3D_OK;
}

D9VolumeTexture12::D9VolumeTexture12(D9Device12* dev) noexcept : m_dev(dev) {}
D9VolumeTexture12::~D9VolumeTexture12() = default;

HRESULT D9VolumeTexture12::Create(D9Device12* dev, UINT w, UINT h, UINT d, UINT levels,
                                  DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                  D9VolumeTexture12** out) noexcept
{
    if (!out || !dev) return D3DERR_INVALIDCALL;
    *out = nullptr;
    auto* t = new (std::nothrow) D9VolumeTexture12(dev);
    if (!t) return E_OUTOFMEMORY;

    const HRESULT hr = t->m_storage.Init(dev->Ctx(), D3DRTYPE_VOLUMETEXTURE,
                                         w, h, d, levels, usage, fmt, pool, 1);
    if (FAILED(hr)) { delete t; return hr; }
    *out = t;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DBaseTexture9) || riid == __uuidof(IDirect3DVolumeTexture9)) {
        *ppv = static_cast<IDirect3DVolumeTexture9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9VolumeTexture12::AddRef()
{ return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE D9VolumeTexture12::Release()
{
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture12::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture12::SetPrivateData(REFGUID g, CONST void* d, DWORD s, DWORD f)
{ return m_privateData.Set(g, d, s, f); }
HRESULT STDMETHODCALLTYPE D9VolumeTexture12::GetPrivateData(REFGUID g, void* d, DWORD* s)
{ return m_privateData.Get(g, d, s); }
HRESULT STDMETHODCALLTYPE D9VolumeTexture12::FreePrivateData(REFGUID g)
{ return m_privateData.Free(g); }
DWORD STDMETHODCALLTYPE D9VolumeTexture12::SetPriority(DWORD) { return 0; }
DWORD STDMETHODCALLTYPE D9VolumeTexture12::GetPriority() { return 0; }
void  STDMETHODCALLTYPE D9VolumeTexture12::PreLoad() { m_storage.FlushDirty(); }
D3DRESOURCETYPE STDMETHODCALLTYPE D9VolumeTexture12::GetType() { return D3DRTYPE_VOLUMETEXTURE; }

DWORD STDMETHODCALLTYPE D9VolumeTexture12::SetLOD(DWORD lod)
{
    if (m_storage.Pool() != D3DPOOL_MANAGED) return 0;
    const DWORD prev = m_lod;
    m_lod = std::min<DWORD>(lod, m_storage.Levels() ? m_storage.Levels() - 1 : 0);
    return prev;
}
DWORD STDMETHODCALLTYPE D9VolumeTexture12::GetLOD()
{ return m_storage.Pool() == D3DPOOL_MANAGED ? m_lod : 0; }
DWORD STDMETHODCALLTYPE D9VolumeTexture12::GetLevelCount() { return m_storage.Levels(); }

HRESULT STDMETHODCALLTYPE D9VolumeTexture12::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE t)
{ m_autoGenFilter = t; return D3D_OK; }
D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE D9VolumeTexture12::GetAutoGenFilterType()
{ return m_autoGenFilter; }
void STDMETHODCALLTYPE D9VolumeTexture12::GenerateMipSubLevels() {}

HRESULT STDMETHODCALLTYPE D9VolumeTexture12::GetLevelDesc(UINT Level, D3DVOLUME_DESC* pDesc)
{
    if (!pDesc || Level >= m_storage.Levels()) return D3DERR_INVALIDCALL;
    UINT w = 0, h = 0, d = 0;
    m_storage.LevelSize(Level, &w, &h, &d);
    std::memset(pDesc, 0, sizeof(*pDesc));
    pDesc->Format = m_storage.Format();
    pDesc->Type   = D3DRTYPE_VOLUME;
    pDesc->Usage  = m_storage.Usage();
    pDesc->Pool   = m_storage.Pool();
    pDesc->Width  = w;
    pDesc->Height = h;
    pDesc->Depth  = d;
    return D3D_OK;
}

D9Volume12::D9Volume12(D9Device12* dev, IDirect3DVolumeTexture9* container,
                       TextureStorage12* storage, UINT level) noexcept
    : m_dev(dev), m_container(container), m_storage(storage), m_level(level)
{
}

D9Volume12::~D9Volume12() = default;

HRESULT STDMETHODCALLTYPE D9Volume12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVolume9)) {
        *ppv = static_cast<IDirect3DVolume9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9Volume12::AddRef()
{
    if (m_container) return m_container->AddRef();
    return m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE D9Volume12::Release()
{
    if (m_container) return m_container->Release();
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9Volume12::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Volume12::SetPrivateData(REFGUID g, CONST void* d, DWORD s, DWORD f)
{ return m_privateData.Set(g, d, s, f); }
HRESULT STDMETHODCALLTYPE D9Volume12::GetPrivateData(REFGUID g, void* d, DWORD* s)
{ return m_privateData.Get(g, d, s); }
HRESULT STDMETHODCALLTYPE D9Volume12::FreePrivateData(REFGUID g)
{ return m_privateData.Free(g); }

HRESULT STDMETHODCALLTYPE D9Volume12::GetContainer(REFIID riid, void** ppContainer)
{
    if (!ppContainer) return D3DERR_INVALIDCALL;
    if (m_container) return m_container->QueryInterface(riid, ppContainer);
    if (m_dev)       return m_dev->QueryInterface(riid, ppContainer);
    *ppContainer = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D9Volume12::GetDesc(D3DVOLUME_DESC* pDesc)
{
    if (!pDesc || !m_storage) return D3DERR_INVALIDCALL;
    UINT w = 0, h = 0, d = 0;
    m_storage->LevelSize(m_level, &w, &h, &d);
    std::memset(pDesc, 0, sizeof(*pDesc));
    pDesc->Format = m_storage->Format();
    pDesc->Type   = D3DRTYPE_VOLUME;
    pDesc->Usage  = m_storage->Usage();
    pDesc->Pool   = m_storage->Pool();
    pDesc->Width  = w;
    pDesc->Height = h;
    pDesc->Depth  = d;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Volume12::LockBox(D3DLOCKED_BOX* pLockedVolume,
                                              CONST D3DBOX* pBox, DWORD Flags)
{
    if (!pLockedVolume || !m_storage) return D3DERR_INVALIDCALL;
    pLockedVolume->pBits      = nullptr;
    pLockedVolume->RowPitch   = 0;
    pLockedVolume->SlicePitch = 0;
    void* bits = nullptr;
    UINT rowPitch = 0, slicePitch = 0;
    const HRESULT hr = m_storage->LockRegion(0, m_level, nullptr, pBox, Flags,
                                             &bits, &rowPitch, &slicePitch);
    if (FAILED(hr)) return hr;
    pLockedVolume->pBits      = bits;
    pLockedVolume->RowPitch   = static_cast<INT>(rowPitch);
    pLockedVolume->SlicePitch = static_cast<INT>(slicePitch);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Volume12::UnlockBox()
{
    if (!m_storage) return D3DERR_INVALIDCALL;
    return m_storage->UnlockRegion(0, m_level);
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture12::GetVolumeLevel(UINT Level,
                                                            IDirect3DVolume9** ppVolumeLevel)
{
    if (!ppVolumeLevel) return D3DERR_INVALIDCALL;
    *ppVolumeLevel = nullptr;
    if (Level >= m_storage.Levels()) return D3DERR_INVALIDCALL;

    if (m_volumes.size() < m_storage.Levels())
        m_volumes.resize(m_storage.Levels());

    if (!m_volumes[Level]) {
        m_volumes[Level] = D9Volume12::Owned(new (std::nothrow) D9Volume12(
            m_dev, static_cast<IDirect3DVolumeTexture9*>(this), &m_storage, Level));
        if (!m_volumes[Level]) return E_OUTOFMEMORY;
    }
    *ppVolumeLevel = m_volumes[Level].get();
    (*ppVolumeLevel)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture12::LockBox(UINT Level, D3DLOCKED_BOX* pLockedVolume,
                                                      CONST D3DBOX* pBox, DWORD Flags)
{
    if (!pLockedVolume) return D3DERR_INVALIDCALL;
    pLockedVolume->pBits      = nullptr;
    pLockedVolume->RowPitch   = 0;
    pLockedVolume->SlicePitch = 0;
    void* bits = nullptr;
    UINT rowPitch = 0, slicePitch = 0;
    const HRESULT hr = m_storage.LockRegion(0, Level, nullptr, pBox, Flags,
                                            &bits, &rowPitch, &slicePitch);
    if (FAILED(hr)) return hr;
    pLockedVolume->pBits      = bits;
    pLockedVolume->RowPitch   = static_cast<INT>(rowPitch);
    pLockedVolume->SlicePitch = static_cast<INT>(slicePitch);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture12::UnlockBox(UINT Level)
{ return m_storage.UnlockRegion(0, Level); }

HRESULT STDMETHODCALLTYPE D9VolumeTexture12::AddDirtyBox(CONST D3DBOX*)
{
    m_storage.AddDirty(0, 0, nullptr);
    return D3D_OK;
}

}
