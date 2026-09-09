// D3D12 shader and vertex declaration proxies.
//
// Hold the game's original D3D9 bytecode and defer translation to first use,
// for the same reasons as the D3D11 backend: a game may create shaders it
// never draws with, pixel shader translation depends on render state that is
// not known at creation, and the original bytecode is both the cache key and
// what GetFunction is required to return.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy12/Shaders12.h>
#include <d3d9proxy12/D9Device12.h>
#include <core/D3D9ShaderTranslator.h>
#include <core/DeviceContext12.h>
#include <core/Log.h>

#include <cstring>
#include <new>

namespace mwon12 {

namespace {

constexpr size_t kMaxShaderScanBytes = 1u << 20;

bool CopyShaderBlob(const DWORD* function, std::vector<DWORD>& out) noexcept
{
    const size_t bytes = MeasureD3D9Shader(
        reinterpret_cast<const uint32_t*>(function), kMaxShaderScanBytes);
    if (bytes < 8 || (bytes % sizeof(DWORD)) != 0)
        return false;

    const size_t dwords = bytes / sizeof(DWORD);
    out.assign(function, function + dwords);
    return true;
}

struct D3DPIPELINETIMINGS_LOCAL {
    float VertexProcessingTimePercent;
    float PixelProcessingTimePercent;
    float OtherGPUProcessingTimePercent;
    float GPUIdleTimePercent;
};
struct D3DDEVINFO_VCACHE_LOCAL {
    DWORD Pattern;
    DWORD OptMethod;
    DWORD CacheSize;
    DWORD MagicNumber;
};
struct D3DDEVINFO_D3DVERTEXSTATS_LOCAL {
    DWORD NumRenderedTriangles;
    DWORD NumExtraClippingTriangles;
};

DWORD QueryDataSize12(D3DQUERYTYPE type) noexcept
{
    switch (type) {
    case D3DQUERYTYPE_VCACHE:            return sizeof(D3DDEVINFO_VCACHE_LOCAL);
    case D3DQUERYTYPE_RESOURCEMANAGER:   return 7 * sizeof(DWORD);
    case D3DQUERYTYPE_VERTEXSTATS:       return sizeof(D3DDEVINFO_D3DVERTEXSTATS_LOCAL);
    case D3DQUERYTYPE_EVENT:             return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION:         return sizeof(DWORD);
    case D3DQUERYTYPE_TIMESTAMP:         return sizeof(UINT64);
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: return sizeof(BOOL);
    case D3DQUERYTYPE_TIMESTAMPFREQ:     return sizeof(UINT64);
    case D3DQUERYTYPE_PIPELINETIMINGS:   return sizeof(D3DPIPELINETIMINGS_LOCAL);
    default:                             return 0;
    }
}

}

HRESULT D9VertexShader12::Create(D9Device12* dev, const DWORD* function,
                                 D9VertexShader12** out) noexcept
{
    if (!dev || !function || !out) return D3DERR_INVALIDCALL;
    *out = nullptr;

    auto* s = new (std::nothrow) D9VertexShader12(dev);
    if (!s) return E_OUTOFMEMORY;

    if (!CopyShaderBlob(function, s->m_code)) {
        DXLOG_WARN("[dx12] CreateVertexShader: unrecognised D3D9 bytecode "
                   "(version token 0x%08X)", function[0]);
        delete s;
        return D3DERR_INVALIDCALL;
    }

    *out = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VertexShader12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVertexShader9)) {
        *ppv = static_cast<IDirect3DVertexShader9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9VertexShader12::AddRef()
{ return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE D9VertexShader12::Release()
{
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9VertexShader12::GetDevice(IDirect3DDevice9** ppDevice)
{

    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VertexShader12::GetFunction(void* pData, UINT* pSizeOfData)
{

    if (!pSizeOfData) return D3DERR_INVALIDCALL;
    const UINT byteLen = static_cast<UINT>(ByteSize());
    if (!pData) { *pSizeOfData = byteLen; return D3D_OK; }
    if (*pSizeOfData < byteLen) { *pSizeOfData = byteLen; return D3DERR_INVALIDCALL; }
    std::memcpy(pData, m_code.data(), byteLen);
    *pSizeOfData = byteLen;
    return D3D_OK;
}

HRESULT D9PixelShader12::Create(D9Device12* dev, const DWORD* function,
                                D9PixelShader12** out) noexcept
{
    if (!dev || !function || !out) return D3DERR_INVALIDCALL;
    *out = nullptr;

    auto* s = new (std::nothrow) D9PixelShader12(dev);
    if (!s) return E_OUTOFMEMORY;

    if (!CopyShaderBlob(function, s->m_code)) {
        DXLOG_WARN("[dx12] CreatePixelShader: unrecognised D3D9 bytecode "
                   "(version token 0x%08X)", function[0]);
        delete s;
        return D3DERR_INVALIDCALL;
    }

    *out = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9PixelShader12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DPixelShader9)) {
        *ppv = static_cast<IDirect3DPixelShader9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9PixelShader12::AddRef()
{ return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE D9PixelShader12::Release()
{
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9PixelShader12::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9PixelShader12::GetFunction(void* pData, UINT* pSizeOfData)
{
    if (!pSizeOfData) return D3DERR_INVALIDCALL;
    const UINT byteLen = static_cast<UINT>(ByteSize());
    if (!pData) { *pSizeOfData = byteLen; return D3D_OK; }
    if (*pSizeOfData < byteLen) { *pSizeOfData = byteLen; return D3DERR_INVALIDCALL; }
    std::memcpy(pData, m_code.data(), byteLen);
    *pSizeOfData = byteLen;
    return D3D_OK;
}

D9StateBlock12::~D9StateBlock12()
{
    ReleaseHeld();
}

HRESULT D9StateBlock12::CreateCaptured(D9Device12* dev, D3DSTATEBLOCKTYPE type,
                                       D9StateBlock12** out) noexcept
{
    if (!dev || !out) return D3DERR_INVALIDCALL;
    *out = nullptr;

    if (type != D3DSBT_ALL && type != D3DSBT_PIXELSTATE && type != D3DSBT_VERTEXSTATE)
        return D3DERR_INVALIDCALL;

    auto* sb = new (std::nothrow) D9StateBlock12(dev);
    if (!sb) return E_OUTOFMEMORY;

    sb->m_type = type;
    sb->CaptureAll();
    *out = sb;
    return D3D_OK;
}

HRESULT D9StateBlock12::CreateRecording(D9Device12* dev,
                                       D9StateBlock12** out) noexcept
{
    if (!dev || !out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    auto* sb = new (std::nothrow) D9StateBlock12(dev);
    if (!sb) return E_OUTOFMEMORY;
    sb->m_type      = D3DSBT_ALL;
    sb->m_recording = true;
    *out = sb;
    return D3D_OK;
}

void D9StateBlock12::RecordRenderState(D3DRENDERSTATETYPE type, DWORD value) noexcept
{
    if (static_cast<UINT>(type) > D3DRS_BLENDOPALPHA) return;
    m_snap.rs.rs[type]   = value;
    m_recorded.rs[type]  = true;
}

void D9StateBlock12::RecordTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type,
                                             DWORD value) noexcept
{
    if (stage >= 8 || static_cast<UINT>(type) > D3DTSS_CONSTANT) return;
    m_snap.rs.tss[stage][type]  = value;
    m_recorded.tss[stage][type] = true;
}

void D9StateBlock12::RecordSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type,
                                        DWORD value) noexcept
{
    if (sampler >= 8 || static_cast<UINT>(type) > D3DSAMP_DMAPOFFSET) return;
    m_snap.rs.samp[sampler][type]  = value;
    m_recorded.samp[sampler][type] = true;
}

void D9StateBlock12::RecordTexture(DWORD stage, IDirect3DBaseTexture9* tex) noexcept
{
    if (stage >= 8) return;
    if (tex) tex->AddRef();
    if (m_snap.rs.textures[stage]) m_snap.rs.textures[stage]->Release();
    m_snap.rs.textures[stage]  = tex;
    m_recorded.textures[stage] = true;
}

void D9StateBlock12::RecordStreamSource(UINT stream, IDirect3DVertexBuffer9* vb,
                                        UINT offset, UINT stride) noexcept
{
    if (stream >= 16) return;
    if (vb) vb->AddRef();
    if (m_snap.rs.streams[stream]) m_snap.rs.streams[stream]->Release();
    m_snap.rs.streams[stream]       = vb;
    m_snap.rs.streamOffsets[stream] = offset;
    m_snap.rs.streamStrides[stream] = stride;
    m_recorded.streams[stream]      = true;
}

void D9StateBlock12::RecordIndices(IDirect3DIndexBuffer9* ib) noexcept
{
    if (ib) ib->AddRef();
    if (m_snap.rs.indexBuffer) m_snap.rs.indexBuffer->Release();
    m_snap.rs.indexBuffer  = ib;
    m_recorded.indexBuffer = true;
}

void D9StateBlock12::RecordVertexShader(IDirect3DVertexShader9* vs) noexcept
{
    if (vs) vs->AddRef();
    if (m_snap.vs) m_snap.vs->Release();
    m_snap.vs = vs;
    m_recorded.vertexShader = true;
}

void D9StateBlock12::RecordPixelShader(IDirect3DPixelShader9* ps) noexcept
{
    if (ps) ps->AddRef();
    if (m_snap.ps) m_snap.ps->Release();
    m_snap.ps = ps;
    m_recorded.pixelShader = true;
}

void D9StateBlock12::RecordVertexDeclaration(IDirect3DVertexDeclaration9* decl) noexcept
{
    if (decl) decl->AddRef();
    if (m_snap.decl) m_snap.decl->Release();
    m_snap.decl = decl;
    m_recorded.vertexDecl = true;
}

void D9StateBlock12::RecordFVF(DWORD fvf) noexcept
{
    m_snap.fvf = fvf;
    m_recorded.fvf = true;
}

void D9StateBlock12::RecordViewport(const D3DVIEWPORT9& vp) noexcept
{
    m_snap.viewport = vp;
    m_recorded.viewport = true;
}

void D9StateBlock12::RecordScissor(const RECT& r) noexcept
{
    m_snap.scissor = r;
    m_recorded.scissor = true;
}

void D9StateBlock12::RecordVSConstF(UINT start, const float* v, UINT count) noexcept
{
    for (UINT i = 0; i < count && start + i < 256; ++i) {
        std::memcpy(m_snap.vsF[start + i], v + i * 4, 4 * sizeof(float));
        m_recorded.vsConstF[start + i] = true;
    }
}

void D9StateBlock12::RecordPSConstF(UINT start, const float* v, UINT count) noexcept
{
    for (UINT i = 0; i < count && start + i < 224; ++i) {
        std::memcpy(m_snap.psF[start + i], v + i * 4, 4 * sizeof(float));
        m_recorded.psConstF[start + i] = true;
    }
}

void D9StateBlock12::RecordVSConstI(UINT start, const int* v, UINT count) noexcept
{
    for (UINT i = 0; i < count && start + i < 16; ++i) {
        std::memcpy(m_snap.vsI[start + i], v + i * 4, 4 * sizeof(int));
        m_recorded.vsConstI[start + i] = true;
    }
}

void D9StateBlock12::RecordPSConstI(UINT start, const int* v, UINT count) noexcept
{
    for (UINT i = 0; i < count && start + i < 16; ++i) {
        std::memcpy(m_snap.psI[start + i], v + i * 4, 4 * sizeof(int));
        m_recorded.psConstI[start + i] = true;
    }
}

void D9StateBlock12::RecordVSConstB(UINT start, const BOOL* v, UINT count) noexcept
{
    for (UINT i = 0; i < count && start + i < 16; ++i) {
        m_snap.vsB[start + i] = v[i];
        m_recorded.vsConstB[start + i] = true;
    }
}

void D9StateBlock12::RecordPSConstB(UINT start, const BOOL* v, UINT count) noexcept
{
    for (UINT i = 0; i < count && start + i < 16; ++i) {
        m_snap.psB[start + i] = v[i];
        m_recorded.psConstB[start + i] = true;
    }
}

void D9StateBlock12::ReleaseHeld() noexcept
{

    auto drop = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    for (auto*& t : m_snap.rs.textures) drop(t);
    for (auto*& v : m_snap.rs.streams)  drop(v);
    drop(m_snap.rs.indexBuffer);
    drop(m_snap.decl);
    drop(m_snap.vs);
    drop(m_snap.ps);

    m_snap.rs.vertexDecl   = nullptr;
    m_snap.rs.vertexShader = nullptr;
    m_snap.rs.pixelShader  = nullptr;
}

void D9StateBlock12::CaptureAll() noexcept
{

    ReleaseHeld();
    if (m_dev) m_dev->CaptureInto(m_snap);
}

HRESULT STDMETHODCALLTYPE D9StateBlock12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DStateBlock9)) {
        *ppv = static_cast<IDirect3DStateBlock9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9StateBlock12::AddRef()
{ return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE D9StateBlock12::Release()
{
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9StateBlock12::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9StateBlock12::Capture()
{
    if (!m_dev) return D3DERR_INVALIDCALL;
    if (!m_recording) { CaptureAll(); return D3D_OK; }

    const auto& live = m_dev->m_rst.State();
    for (UINT i = 0; i <= D3DRS_BLENDOPALPHA; ++i)
        if (m_recorded.rs[i]) m_snap.rs.rs[i] = live.rs[i];
    for (UINT st = 0; st < 8; ++st) {
        for (UINT t = 0; t <= D3DTSS_CONSTANT; ++t)
            if (m_recorded.tss[st][t]) m_snap.rs.tss[st][t] = live.tss[st][t];
        for (UINT t = 0; t <= D3DSAMP_DMAPOFFSET; ++t)
            if (m_recorded.samp[st][t]) m_snap.rs.samp[st][t] = live.samp[st][t];
        if (m_recorded.textures[st]) RecordTexture(st, live.textures[st]);
    }
    for (UINT i = 0; i < 16; ++i)
        if (m_recorded.streams[i])
            RecordStreamSource(i, live.streams[i], live.streamOffsets[i],
                               live.streamStrides[i]);
    if (m_recorded.indexBuffer)  RecordIndices(live.indexBuffer);
    if (m_recorded.vertexShader) RecordVertexShader(live.vertexShader);
    if (m_recorded.pixelShader)  RecordPixelShader(live.pixelShader);
    if (m_recorded.vertexDecl)   RecordVertexDeclaration(live.vertexDecl);
    if (m_recorded.fvf)          m_snap.fvf      = m_dev->m_fvf;
    if (m_recorded.viewport)     m_snap.viewport = m_dev->m_viewport;
    if (m_recorded.scissor)      m_snap.scissor  = m_dev->m_scissor;
    return D3D_OK;
}

void D9StateBlock12::ApplyRecorded() noexcept
{
    D9Device12* d = m_dev;
    auto& live = d->m_rst.State();

    auto swapRef = [](auto*& slot, auto* want) {
        if (slot == want) return;
        if (want) want->AddRef();
        if (slot) slot->Release();
        slot = want;
    };

    for (UINT i = 0; i <= D3DRS_BLENDOPALPHA; ++i)
        if (m_recorded.rs[i]) live.rs[i] = m_snap.rs.rs[i];

    for (UINT st = 0; st < 8; ++st) {
        for (UINT t = 0; t <= D3DTSS_CONSTANT; ++t)
            if (m_recorded.tss[st][t]) live.tss[st][t] = m_snap.rs.tss[st][t];
        for (UINT t = 0; t <= D3DSAMP_DMAPOFFSET; ++t)
            if (m_recorded.samp[st][t]) live.samp[st][t] = m_snap.rs.samp[st][t];
        if (m_recorded.textures[st])
            swapRef(live.textures[st], m_snap.rs.textures[st]);
    }

    for (UINT i = 0; i < 16; ++i) {
        if (!m_recorded.streams[i]) continue;
        swapRef(live.streams[i], m_snap.rs.streams[i]);
        live.streamOffsets[i] = m_snap.rs.streamOffsets[i];
        live.streamStrides[i] = m_snap.rs.streamStrides[i];
    }

    if (m_recorded.indexBuffer) swapRef(live.indexBuffer, m_snap.rs.indexBuffer);
    if (m_recorded.vertexShader) {
        swapRef(live.vertexShader, m_snap.vs);
        d->m_vs = static_cast<D9VertexShader12*>(live.vertexShader);
    }
    if (m_recorded.pixelShader) {
        swapRef(live.pixelShader, m_snap.ps);
        d->m_ps = static_cast<D9PixelShader12*>(live.pixelShader);
    }
    if (m_recorded.vertexDecl) {
        swapRef(live.vertexDecl, m_snap.decl);
        d->m_decl = static_cast<D9VertexDecl12*>(live.vertexDecl);
    }
    if (m_recorded.fvf) { d->m_fvf = m_snap.fvf; d->m_fvfDeclCached = 0xFFFFFFFF; }
    if (m_recorded.viewport) d->m_viewport = m_snap.viewport;
    if (m_recorded.scissor)  d->m_scissor  = m_snap.scissor;

    bool vsDirty = false, psDirty = false;
    for (UINT i = 0; i < 256; ++i)
        if (m_recorded.vsConstF[i]) {
            std::memcpy(d->m_vsConst.c[i], m_snap.vsF[i], 4 * sizeof(float));
            vsDirty = true;
        }
    for (UINT i = 0; i < 224; ++i)
        if (m_recorded.psConstF[i]) {
            std::memcpy(d->m_psConst.c[i], m_snap.psF[i], 4 * sizeof(float));
            psDirty = true;
        }
    for (UINT i = 0; i < 16; ++i) {
        if (m_recorded.vsConstI[i]) {
            std::memcpy(d->m_vsConst.ic[i], m_snap.vsI[i], 4 * sizeof(int));
            vsDirty = true;
        }
        if (m_recorded.psConstI[i]) {
            std::memcpy(d->m_psConst.ic[i], m_snap.psI[i], 4 * sizeof(int));
            psDirty = true;
        }
        if (m_recorded.vsConstB[i]) {
            d->m_vsConst.bc[i / 4][i % 4] = m_snap.vsB[i] ? 1u : 0u;
            vsDirty = true;
        }
        if (m_recorded.psConstB[i]) {
            d->m_psConst.bc[i / 4][i % 4] = m_snap.psB[i] ? 1u : 0u;
            psDirty = true;
        }
    }
    if (vsDirty) d->m_vsConstDirty = true;
    if (psDirty) d->m_psConstDirty = true;

    d->m_rst.MarkAllDirty();
}

HRESULT STDMETHODCALLTYPE D9StateBlock12::Apply()
{
    if (!m_dev) return D3DERR_INVALIDCALL;
    if (m_recording) ApplyRecorded();
    else             m_dev->ApplyFrom(m_snap, m_type);
    return D3D_OK;
}

D9Query12::~D9Query12() = default;

HRESULT D9Query12::Supported(D3DQUERYTYPE type) noexcept
{
    switch (type) {
    case D3DQUERYTYPE_EVENT:
    case D3DQUERYTYPE_OCCLUSION:
    case D3DQUERYTYPE_TIMESTAMP:
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
    case D3DQUERYTYPE_TIMESTAMPFREQ:
    case D3DQUERYTYPE_VCACHE:
    case D3DQUERYTYPE_RESOURCEMANAGER:
    case D3DQUERYTYPE_VERTEXSTATS:
    case D3DQUERYTYPE_PIPELINETIMINGS:
        return D3D_OK;
    default:
        return D3DERR_NOTAVAILABLE;
    }
}

HRESULT D9Query12::Create(D9Device12* dev, D3DQUERYTYPE type,
                          D9Query12** out) noexcept
{
    if (!dev || !out) return D3DERR_INVALIDCALL;
    *out = nullptr;

    const HRESULT sup = Supported(type);
    if (FAILED(sup)) return sup;

    DeviceContext12* ctx = dev->Ctx();
    if (!ctx || !ctx->Device()) return D3DERR_INVALIDCALL;

    auto* q = new (std::nothrow) D9Query12(dev, type);
    if (!q) return E_OUTOFMEMORY;

    const bool needsHeap = (type == D3DQUERYTYPE_OCCLUSION ||
                            type == D3DQUERYTYPE_TIMESTAMP);
    if (needsHeap) {
        D3D12_QUERY_HEAP_DESC hd{};
        hd.Type  = (type == D3DQUERYTYPE_OCCLUSION)
                     ? D3D12_QUERY_HEAP_TYPE_OCCLUSION
                     : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        hd.Count = 1;
        HRESULT hr = ctx->Device()->CreateQueryHeap(&hd, IID_PPV_ARGS(&q->m_heap));
        if (FAILED(hr)) {
            DXLOG_WARN_HR(hr, "[dx12] CreateQueryHeap failed for D3DQUERYTYPE %d",
                          static_cast<int>(type));
            delete q;
            return D3DERR_NOTAVAILABLE;
        }

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = sizeof(UINT64);
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = ctx->Device()->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&q->m_readback));
        if (FAILED(hr)) {
            DXLOG_WARN_HR(hr, "[dx12] query readback buffer allocation failed");
            delete q;
            return E_OUTOFMEMORY;
        }
    }

    *out = q;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Query12::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DQuery9)) {
        *ppv = static_cast<IDirect3DQuery9*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9Query12::AddRef()
{ return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG STDMETHODCALLTYPE D9Query12::Release()
{
    const ULONG prev = m_ref.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9Query12::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_dev);
    if (m_dev) m_dev->AddRef();
    return D3D_OK;
}

DWORD STDMETHODCALLTYPE D9Query12::GetDataSize()
{
    return QueryDataSize12(m_type);
}

HRESULT STDMETHODCALLTYPE D9Query12::Issue(DWORD dwIssueFlags)
{
    DeviceContext12* ctx = m_dev ? m_dev->Ctx() : nullptr;
    if (!ctx) return D3DERR_INVALIDCALL;

    ID3D12GraphicsCommandList* cmd = ctx->CmdList();

    if (dwIssueFlags & D3DISSUE_BEGIN) {
        if (m_type == D3DQUERYTYPE_OCCLUSION && m_heap && cmd)
            cmd->BeginQuery(m_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        m_resolved = false;
        m_issued   = false;
        return D3D_OK;
    }

    if (!(dwIssueFlags & D3DISSUE_END))
        return D3DERR_INVALIDCALL;

    if (cmd && m_heap && m_readback) {
        const D3D12_QUERY_TYPE qt = (m_type == D3DQUERYTYPE_OCCLUSION)
                                      ? D3D12_QUERY_TYPE_OCCLUSION
                                      : D3D12_QUERY_TYPE_TIMESTAMP;
        cmd->EndQuery(m_heap.Get(), qt, 0);
        cmd->ResolveQueryData(m_heap.Get(), qt, 0, 1, m_readback.Get(), 0);
    }

    m_fenceValue = ctx->CurrentFence() + 1;
    m_issued     = true;
    m_resolved   = false;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Query12::GetData(void* pData, DWORD dwSize,
                                             DWORD dwGetDataFlags)
{
    DeviceContext12* ctx = m_dev ? m_dev->Ctx() : nullptr;
    if (!ctx) return D3DERR_INVALIDCALL;

    switch (m_type) {
    case D3DQUERYTYPE_TIMESTAMPFREQ: {
        if (!pData) return S_OK;
        if (dwSize < sizeof(UINT64)) return D3DERR_INVALIDCALL;
        UINT64 freq = 0;
        if (ctx->CmdQueue()) ctx->CmdQueue()->GetTimestampFrequency(&freq);
        std::memcpy(pData, &freq, sizeof(freq));
        return S_OK;
    }
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: {

        if (!pData) return S_OK;
        if (dwSize < sizeof(BOOL)) return D3DERR_INVALIDCALL;
        const BOOL disjoint = FALSE;
        std::memcpy(pData, &disjoint, sizeof(disjoint));
        return S_OK;
    }
    case D3DQUERYTYPE_VCACHE:
    case D3DQUERYTYPE_RESOURCEMANAGER:
    case D3DQUERYTYPE_VERTEXSTATS:
    case D3DQUERYTYPE_PIPELINETIMINGS: {

        const DWORD need = QueryDataSize12(m_type);
        if (!pData) return S_OK;
        if (dwSize < need) return D3DERR_INVALIDCALL;
        std::memset(pData, 0, need);
        return S_OK;
    }
    default:
        break;
    }

    if (!m_issued) return D3DERR_INVALIDCALL;

    bool ready = ctx->CompletedFence() >= m_fenceValue;
    if (!ready && (dwGetDataFlags & D3DGETDATA_FLUSH)) {

        ctx->FlushAndWait();
        ready = ctx->CompletedFence() >= m_fenceValue;
    }
    if (!ready) return S_FALSE;

    if (!pData) return S_OK;

    switch (m_type) {
    case D3DQUERYTYPE_EVENT: {
        if (dwSize < sizeof(BOOL)) return D3DERR_INVALIDCALL;
        const BOOL done = TRUE;
        std::memcpy(pData, &done, sizeof(done));
        return S_OK;
    }
    case D3DQUERYTYPE_OCCLUSION:
    case D3DQUERYTYPE_TIMESTAMP: {
        const DWORD need = QueryDataSize12(m_type);
        if (dwSize < need) return D3DERR_INVALIDCALL;
        if (!m_readback) return D3DERR_INVALIDCALL;

        UINT64 raw = 0;
        void*  mapped = nullptr;
        const D3D12_RANGE readRange{ 0, sizeof(UINT64) };
        if (SUCCEEDED(m_readback->Map(0, &readRange, &mapped)) && mapped) {
            std::memcpy(&raw, mapped, sizeof(raw));
            const D3D12_RANGE noWrite{ 0, 0 };
            m_readback->Unmap(0, &noWrite);
        }
        m_resolved = true;

        if (m_type == D3DQUERYTYPE_TIMESTAMP) {
            std::memcpy(pData, &raw, sizeof(UINT64));
        } else {

            const DWORD pixels = (raw > 0xFFFFFFFFull)
                                   ? 0xFFFFFFFFu
                                   : static_cast<DWORD>(raw);
            std::memcpy(pData, &pixels, sizeof(pixels));
        }
        return S_OK;
    }
    default:
        return D3DERR_INVALIDCALL;
    }
}

}
