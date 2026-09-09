// D3D12 vertex and index buffers.
//
// Both share a base class, since the difference between them is little more
// than the index format. Locking uses a CPU-side copy that is uploaded on
// unlock, because a default-heap resource cannot be mapped.
//
// The GPU address is resolved on demand rather than cached. A buffer whose
// contents were updated this frame may have been re-suballocated within the
// upload memory, so an address captured earlier can name the previous
// location.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy12/Resources12.h>
#include <d3d9proxy12/D9Device12.h>
#include <core/DeviceContext12.h>
#include <core/Log.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace mwon12 {

HRESULT D9BufferBase12::InitBuffer(DeviceContext12* ctx, UINT bytes, DWORD usage,
                                   D3DPOOL pool, const wchar_t* name) noexcept
{
    if (!ctx || bytes == 0) return D3DERR_INVALIDCALL;
    m_ctx       = ctx;
    m_size      = bytes;
    m_usage     = usage;
    m_pool      = pool;
    m_placement = ClassifyPlacement(pool, usage,   true);

    if (m_placement == Placement12::CpuOnly) {
        m_shadow.assign(bytes, 0);
        return D3D_OK;
    }

    const bool upload = (m_placement == Placement12::UploadHeap);
    const auto rd     = dx12::BufferDesc(bytes);
    const HRESULT hr  = m_gpu.Init(
        ctx, rd,
        upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT,
        upload ? D3D12_RESOURCE_STATE_GENERIC_READ
               : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, name);
    if (FAILED(hr)) return hr;

    if (!upload)
        m_shadow.assign(bytes, 0);
    return D3D_OK;
}

D3D12_GPU_VIRTUAL_ADDRESS D9BufferBase12::CurrentGpuAddress() const noexcept
{

    if (m_renameGpu && m_ctx && m_renameFrame == m_ctx->FrameCounter())
        return m_renameGpu;
    return m_gpu.Gpu();
}

HRESULT D9BufferBase12::FlushPending() noexcept
{
    return D3D_OK;
}

HRESULT D9BufferBase12::LockImpl(UINT offset, UINT size, void** ppbData,
                                 DWORD flags) noexcept
{
    if (!ppbData) return D3DERR_INVALIDCALL;
    *ppbData = nullptr;
    if (m_locked) {
        DXLOG_WARN("[dx12] nested buffer Lock rejected (size=%u)", m_size);
        return D3DERR_INVALIDCALL;
    }
    if (offset >= m_size) return D3DERR_INVALIDCALL;

    if (size == 0 || offset + size > m_size)
        size = m_size - offset;

    m_lockOffset = offset;
    m_lockSize   = size;
    m_lockFlags  = flags;
    m_locked     = true;

    if (m_placement == Placement12::UploadHeap) {
        const bool discard     = (flags & D3DLOCK_DISCARD) != 0;
        const bool noOverwrite = (flags & D3DLOCK_NOOVERWRITE) != 0;
        const UINT64 frame     = m_ctx ? m_ctx->FrameCounter() : 0;
        const bool renameLive  = m_renameCpu && m_renameFrame == frame;

        if (discard) {
            auto a = m_ctx->AllocUpload(m_size, 16);
            if (a.Valid()) {
                m_renameCpu   = a.cpu;
                m_renameGpu   = a.gpu;
                m_renameFrame = frame;
                *ppbData      = a.cpu + offset;
                return D3D_OK;
            }

            DXLOG_WARN("[dx12] DISCARD rename fell back to the persistent buffer "
                       "(frame upload budget exhausted, %u bytes)", m_size);
            m_renameCpu = nullptr;
            m_renameGpu = 0;
        } else if (noOverwrite && renameLive) {
            *ppbData = m_renameCpu + offset;
            return D3D_OK;
        } else if (!noOverwrite) {

            m_renameCpu = nullptr;
            m_renameGpu = 0;
        }

        uint8_t* base = m_gpu.MappedPtr();
        if (!base) return D3DERR_INVALIDCALL;
        *ppbData = base + offset;
        return D3D_OK;
    }

    if (m_shadow.size() < m_size)
        m_shadow.assign(m_size, 0);
    *ppbData = m_shadow.data() + offset;
    return D3D_OK;
}

HRESULT D9BufferBase12::UnlockImpl() noexcept
{
    if (!m_locked) return D3DERR_INVALIDCALL;
    m_locked = false;

    if (m_placement == Placement12::UploadHeap || m_placement == Placement12::CpuOnly)
        return D3D_OK;

    if (m_lockFlags & D3DLOCK_READONLY)
        return D3D_OK;
    if (!m_gpu.Valid() || m_shadow.empty())
        return D3D_OK;

    return m_gpu.UploadBufferRange(m_lockOffset, m_lockSize,
                                   m_shadow.data() + m_lockOffset);
}

D9VertexBuffer12::D9VertexBuffer12(D9Device12* dev) noexcept : m_dev(dev) {}
D9VertexBuffer12::~D9VertexBuffer12() = default;

HRESULT D9VertexBuffer12::Create(D9Device12* dev, UINT length, DWORD usage, DWORD fvf,
                                 D3DPOOL pool, D9VertexBuffer12** out) noexcept
{
    if (!out || !dev) return D3DERR_INVALIDCALL;
    *out = nullptr;
    auto* b = new (std::nothrow) D9VertexBuffer12(dev);
    if (!b) return E_OUTOFMEMORY;
    b->m_fvf = fvf;

    wchar_t name[48];
    _snwprintf_s(name, _TRUNCATE, L"D3D9VB_%u", length);
    const HRESULT hr = b->InitBuffer(dev->Ctx(), length, usage, pool, name);
    if (FAILED(hr)) { delete b; return hr; }
    *out = b;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VertexBuffer12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DVertexBuffer9)) {
        *ppv = static_cast<IDirect3DVertexBuffer9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9VertexBuffer12::AddRef()
{ return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE D9VertexBuffer12::Release()
{
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9VertexBuffer12::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VertexBuffer12::SetPrivateData(REFGUID g, CONST void* d, DWORD s, DWORD f)
{ return m_privateData.Set(g, d, s, f); }
HRESULT STDMETHODCALLTYPE D9VertexBuffer12::GetPrivateData(REFGUID g, void* d, DWORD* s)
{ return m_privateData.Get(g, d, s); }
HRESULT STDMETHODCALLTYPE D9VertexBuffer12::FreePrivateData(REFGUID g)
{ return m_privateData.Free(g); }
DWORD STDMETHODCALLTYPE D9VertexBuffer12::SetPriority(DWORD) { return 0; }
DWORD STDMETHODCALLTYPE D9VertexBuffer12::GetPriority() { return 0; }
void  STDMETHODCALLTYPE D9VertexBuffer12::PreLoad() {}
D3DRESOURCETYPE STDMETHODCALLTYPE D9VertexBuffer12::GetType() { return D3DRTYPE_VERTEXBUFFER; }

HRESULT STDMETHODCALLTYPE D9VertexBuffer12::Lock(UINT OffsetToLock, UINT SizeToLock,
                                                  void** ppbData, DWORD Flags)
{ return LockImpl(OffsetToLock, SizeToLock, ppbData, Flags); }

HRESULT STDMETHODCALLTYPE D9VertexBuffer12::Unlock() { return UnlockImpl(); }

HRESULT STDMETHODCALLTYPE D9VertexBuffer12::GetDesc(D3DVERTEXBUFFER_DESC* pDesc)
{
    if (!pDesc) return D3DERR_INVALIDCALL;
    std::memset(pDesc, 0, sizeof(*pDesc));
    pDesc->Format = D3DFMT_VERTEXDATA;
    pDesc->Type   = D3DRTYPE_VERTEXBUFFER;
    pDesc->Usage  = m_usage;
    pDesc->Pool   = m_pool;
    pDesc->Size   = m_size;
    pDesc->FVF    = m_fvf;
    return D3D_OK;
}

D9IndexBuffer12::D9IndexBuffer12(D9Device12* dev) noexcept : m_dev(dev) {}
D9IndexBuffer12::~D9IndexBuffer12() = default;

HRESULT D9IndexBuffer12::Create(D9Device12* dev, UINT length, DWORD usage,
                                D3DFORMAT fmt, D3DPOOL pool,
                                D9IndexBuffer12** out) noexcept
{
    if (!out || !dev) return D3DERR_INVALIDCALL;
    *out = nullptr;
    if (fmt != D3DFMT_INDEX16 && fmt != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;

    auto* b = new (std::nothrow) D9IndexBuffer12(dev);
    if (!b) return E_OUTOFMEMORY;
    b->m_fmt = fmt;

    wchar_t name[48];
    _snwprintf_s(name, _TRUNCATE, L"D3D9IB_%u", length);
    const HRESULT hr = b->InitBuffer(dev->Ctx(), length, usage, pool, name);
    if (FAILED(hr)) { delete b; return hr; }
    *out = b;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9IndexBuffer12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DIndexBuffer9)) {
        *ppv = static_cast<IDirect3DIndexBuffer9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9IndexBuffer12::AddRef()
{ return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE D9IndexBuffer12::Release()
{
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9IndexBuffer12::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9IndexBuffer12::SetPrivateData(REFGUID g, CONST void* d, DWORD s, DWORD f)
{ return m_privateData.Set(g, d, s, f); }
HRESULT STDMETHODCALLTYPE D9IndexBuffer12::GetPrivateData(REFGUID g, void* d, DWORD* s)
{ return m_privateData.Get(g, d, s); }
HRESULT STDMETHODCALLTYPE D9IndexBuffer12::FreePrivateData(REFGUID g)
{ return m_privateData.Free(g); }
DWORD STDMETHODCALLTYPE D9IndexBuffer12::SetPriority(DWORD) { return 0; }
DWORD STDMETHODCALLTYPE D9IndexBuffer12::GetPriority() { return 0; }
void  STDMETHODCALLTYPE D9IndexBuffer12::PreLoad() {}
D3DRESOURCETYPE STDMETHODCALLTYPE D9IndexBuffer12::GetType() { return D3DRTYPE_INDEXBUFFER; }

HRESULT STDMETHODCALLTYPE D9IndexBuffer12::Lock(UINT OffsetToLock, UINT SizeToLock,
                                                 void** ppbData, DWORD Flags)
{ return LockImpl(OffsetToLock, SizeToLock, ppbData, Flags); }

HRESULT STDMETHODCALLTYPE D9IndexBuffer12::Unlock() { return UnlockImpl(); }

HRESULT STDMETHODCALLTYPE D9IndexBuffer12::GetDesc(D3DINDEXBUFFER_DESC* pDesc)
{
    if (!pDesc) return D3DERR_INVALIDCALL;
    std::memset(pDesc, 0, sizeof(*pDesc));
    pDesc->Format = m_fmt;
    pDesc->Type   = D3DRTYPE_INDEXBUFFER;
    pDesc->Usage  = m_usage;
    pDesc->Pool   = m_pool;
    pDesc->Size   = m_size;
    return D3D_OK;
}

D9VertexDecl12::D9VertexDecl12(D9Device12* dev) noexcept : m_dev(dev) {}
D9VertexDecl12::~D9VertexDecl12() = default;

HRESULT D9VertexDecl12::Create(D9Device12* dev, const D3DVERTEXELEMENT9* elements,
                               D9VertexDecl12** out) noexcept
{
    if (!out || !dev || !elements) return D3DERR_INVALIDCALL;
    *out = nullptr;

    auto* d = new (std::nothrow) D9VertexDecl12(dev);
    if (!d) return E_OUTOFMEMORY;

    const D3DVERTEXELEMENT9 end = D3DDECL_END();
    for (UINT i = 0; i < MAXD3DDECLLENGTH + 1; ++i) {
        d->m_elements.push_back(elements[i]);
        if (elements[i].Stream == end.Stream && elements[i].Type == end.Type)
            break;
    }
    if (d->m_elements.empty() || d->m_elements.back().Stream != end.Stream) {
        delete d;
        return D3DERR_INVALIDCALL;
    }

    d->m_hash = dx12::HashBytes(d->m_elements.data(),
                                d->m_elements.size() * sizeof(D3DVERTEXELEMENT9));
    *out = d;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VertexDecl12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVertexDeclaration9)) {
        *ppv = static_cast<IDirect3DVertexDeclaration9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9VertexDecl12::AddRef()
{ return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE D9VertexDecl12::Release()
{
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9VertexDecl12::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VertexDecl12::GetDeclaration(D3DVERTEXELEMENT9* pElement,
                                                          UINT* pNumElements)
{
    if (!pNumElements) return D3DERR_INVALIDCALL;
    const UINT count = static_cast<UINT>(m_elements.size());
    if (!pElement) { *pNumElements = count; return D3D_OK; }
    if (*pNumElements < count) { *pNumElements = count; return D3DERR_INVALIDCALL; }
    std::memcpy(pElement, m_elements.data(), count * sizeof(D3DVERTEXELEMENT9));
    *pNumElements = count;
    return D3D_OK;
}

}
