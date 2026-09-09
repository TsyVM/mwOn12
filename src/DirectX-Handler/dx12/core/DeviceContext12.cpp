
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/DeviceContext12.h>
#include <core/Resource12.h>
#include <core/BackendSelect.h>
#include <core/FormatConverter.h>
#include <core/Log.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <new>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace mwon12 {

std::atomic<DeviceContext12*> DeviceContext12::s_current{ nullptr };

namespace {

constexpr UINT kRtvHeapSize      = 512;
constexpr UINT kDsvHeapSize      = 256;
constexpr UINT kSrvStagingSize   = 8192;
constexpr UINT kSampStagingSize  = 512;

constexpr UINT kSrvArenaBlocks   = 8192;
constexpr UINT kSampArenaBlocks  = 128;

constexpr UINT   kUploadBytesPerFrameDefault = 24u * 1024u * 1024u;
constexpr UINT64 kUploadChunkBytes           = 8ull * 1024ull * 1024ull;
constexpr UINT64 kUploadGrowthCapDefault     = 192ull * 1024ull * 1024ull;

}

bool CpuDescriptorHeap::Init(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type,
                             UINT capacity) noexcept
{
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.Type           = type;
    d.NumDescriptors = capacity;
    d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    d.NodeMask       = 0;
    if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(m_heap.GetAddressOf()))))
        return false;
    m_base     = m_heap->GetCPUDescriptorHandleForHeapStart();
    m_stride   = dev->GetDescriptorHandleIncrementSize(type);
    m_capacity = capacity;
    m_used     = 0;
    m_free.clear();
    m_free.reserve(64);
    m_generation.assign(capacity, 0u);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorHeap::Alloc() noexcept
{
    if (!m_free.empty()) {
        const auto h = m_free.back();
        m_free.pop_back();
        return h;
    }
    if (m_used >= m_capacity)
        return D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };
    return dx12::Offset(m_base, m_used++, m_stride);
}

// Freeing bumps the slot's generation counter, and callers that cache
// anything keyed on a descriptor handle must include that counter in the key.
//
// This is not defensive coding — it fixes a real and thoroughly disguised bug.
// The descriptor-table cache was originally keyed on the raw CPU handle. Free
// pushes a slot straight back onto the free list, so the very next Alloc
// reissues the same handle to an unrelated texture, which then found a cached
// descriptor table still describing the dead texture's memory. It surfaced as
// a GPU page fault that DRED attributed to a *recently freed* resource, which
// reads like a lifetime bug in the texture rather than a stale key.
//
// A single global epoch was tried first. It is equally correct but invalidates
// every cached table on every resource retirement, which during texture
// streaming is a continuous stutter. Per-slot generations invalidate only what
// actually changed.
void CpuDescriptorHeap::Free(D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept
{
    if (h.ptr == SIZE_T(-1) || h.ptr == 0 || !m_heap)
        return;

    const SIZE_T slot = (h.ptr - m_base.ptr) / (m_stride ? m_stride : 1);
    if (slot < m_generation.size()) ++m_generation[slot];
    m_free.push_back(h);
}

uint32_t CpuDescriptorHeap::GenerationOf(D3D12_CPU_DESCRIPTOR_HANDLE h) const noexcept
{
    if (h.ptr == SIZE_T(-1) || h.ptr == 0 || !m_heap || h.ptr < m_base.ptr)
        return 0u;
    const SIZE_T slot = (h.ptr - m_base.ptr) / (m_stride ? m_stride : 1);
    return slot < m_generation.size() ? m_generation[slot] : 0u;
}

bool GpuDescriptorArena::Init(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type,
                              UINT blockSize, UINT blockCount) noexcept
{
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.Type           = type;
    d.NumDescriptors = blockSize * blockCount;
    d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    d.NodeMask       = 0;
    if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(m_heap.GetAddressOf()))))
        return false;
    m_cpuBase    = m_heap->GetCPUDescriptorHandleForHeapStart();
    m_gpuBase    = m_heap->GetGPUDescriptorHandleForHeapStart();
    m_type       = type;
    m_stride     = dev->GetDescriptorHandleIncrementSize(type);
    m_blockSize  = blockSize;
    m_blockCount = blockCount;
    m_used       = 0;
    m_cache.clear();
    m_cache.reserve(blockCount);
    return true;
}

bool GpuDescriptorArena::GetOrCreate(ID3D12Device* dev, uint64_t key,
                                     const D3D12_CPU_DESCRIPTOR_HANDLE* srcHandles,
                                     D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) noexcept
{
    if (const auto it = m_cache.find(key); it != m_cache.end()) {
        *outGpu = it->second;
        return true;
    }
    if (m_used >= m_blockCount)
        return false;

    const UINT blockIndex = m_used++;
    const D3D12_CPU_DESCRIPTOR_HANDLE dstBase =
        dx12::Offset(m_cpuBase, blockIndex * m_blockSize, m_stride);

    for (UINT i = 0; i < m_blockSize; ++i) {
        if (srcHandles[i].ptr == 0 || srcHandles[i].ptr == SIZE_T(-1))
            continue;
        dev->CopyDescriptorsSimple(1, dx12::Offset(dstBase, i, m_stride),
                                   srcHandles[i], m_type);
    }

    const D3D12_GPU_DESCRIPTOR_HANDLE gpu =
        dx12::Offset(m_gpuBase, blockIndex * m_blockSize, m_stride);
    m_cache.emplace(key, gpu);
    *outGpu = gpu;
    return true;
}

void GpuDescriptorArena::Reset() noexcept
{
    m_used = 0;
    m_cache.clear();
}

bool UploadRing::Init(ID3D12Device* dev, UINT bytesPerFrame, UINT64 growthCap) noexcept
{
    m_device = dev;
    const auto hp = dx12::HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const auto rd = dx12::BufferDesc(UINT64(bytesPerFrame) * k_dx12FrameCount);

    if (FAILED(dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(m_buffer.GetAddressOf()))))
        return false;

    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(m_buffer->Map(0, &noRead, reinterpret_cast<void**>(&m_cpuPtr))))
        return false;

    m_buffer->SetName(L"MWOn12 Upload Ring");
    m_overflow.clear();
    m_grownBytes   = 0;
    m_grownChunks  = 0;
    m_gpuBase    = m_buffer->GetGPUVirtualAddress();
    m_perFrame   = bytesPerFrame;
    m_growthCap  = growthCap;
    for (auto& c : m_cursor) c = 0;
    return m_cpuPtr != nullptr;
}

void UploadRing::Shutdown() noexcept
{
    AcquireSRWLockExclusive(&m_lock);
    for (auto& c : m_overflow) {
        if (c.buffer && c.cpu) {
            D3D12_RANGE noWrite{ 0, 0 };
            c.buffer->Unmap(0, &noWrite);
        }
    }
    m_overflow.clear();
    m_grownBytes  = 0;
    m_grownChunks = 0;

    if (m_buffer && m_cpuPtr) {
        D3D12_RANGE noWrite{ 0, 0 };
        m_buffer->Unmap(0, &noWrite);
        m_cpuPtr = nullptr;
    }
    m_buffer.Reset();
    m_device = nullptr;
    ReleaseSRWLockExclusive(&m_lock);
}

UploadRing::Allocation UploadRing::Carve(Chunk& c, UINT64 bytes, UINT64 align) noexcept
{
    Allocation a{};
    if (!c.cpu) return a;
    const UINT64 at = dx12::AlignUp(c.cursor, align);
    if (at + bytes > c.size) return a;
    c.cursor   = at + bytes;
    a.cpu      = c.cpu + at;
    a.gpu      = c.gpu + at;
    a.resource = c.buffer.Get();
    a.offset   = at;
    return a;
}

bool UploadRing::CreateChunk(UINT64 bytes, Chunk& out) noexcept
{
    if (!m_device) return false;
    const auto hp = dx12::HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const auto rd = dx12::BufferDesc(bytes);
    if (FAILED(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(out.buffer.GetAddressOf()))))
        return false;

    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(out.buffer->Map(0, &noRead, reinterpret_cast<void**>(&out.cpu)))) {
        out.buffer.Reset();
        return false;
    }
    out.buffer->SetName(L"MWOn12 Upload Ring Growth");
    out.gpu    = out.buffer->GetGPUVirtualAddress();
    out.size   = bytes;
    out.cursor = 0;
    return true;
}

void UploadRing::NotePeak(UINT frameIndex) noexcept
{
    UINT64 used = m_cursor[frameIndex];
    for (const auto& c : m_overflow)
        if (c.owner == frameIndex) used += c.cursor;
    if (used > m_peakFrameBytes) m_peakFrameBytes = used;
}

UploadRing::Allocation UploadRing::Alloc(UINT frameIndex, UINT64 bytes, UINT64 align,
                                         UINT64 completedFence, UINT64 useFence) noexcept
{
    Allocation a{};
    if (!m_cpuPtr || frameIndex >= k_dx12FrameCount || bytes == 0)
        return a;
    if (align == 0) align = 1;

    AcquireSRWLockExclusive(&m_lock);
    struct Unlock {
        SRWLOCK* l;
        ~Unlock() { ReleaseSRWLockExclusive(l); }
    } unlock{ &m_lock };

    const UINT64 base   = UINT64(frameIndex) * m_perFrame;
    const UINT64 cursor = dx12::AlignUp(m_cursor[frameIndex], align);
    if (cursor + bytes <= m_perFrame) {
        m_cursor[frameIndex] = cursor + bytes;
        a.cpu      = m_cpuPtr + base + cursor;
        a.gpu      = m_gpuBase + base + cursor;
        a.resource = m_buffer.Get();
        a.offset   = base + cursor;
        NotePeak(frameIndex);
        return a;
    }

    // Chunks this frame slot already owns.
    for (auto& c : m_overflow) {
        if (c.owner != frameIndex) continue;
        a = Carve(c, bytes, align);
        if (a.Valid()) { c.fence = useFence; NotePeak(frameIndex); return a; }
    }

    // Then any chunk the GPU has finished with, whoever used it last. This is
    // what keeps total growth near the single-frame peak instead of the sum of
    // every slot's peak.
    for (auto& c : m_overflow) {
        if (c.owner == frameIndex || completedFence < c.fence) continue;
        c.owner  = frameIndex;
        c.cursor = 0;
        a = Carve(c, bytes, align);
        if (a.Valid()) { c.fence = useFence; NotePeak(frameIndex); return a; }
    }

    if (m_grownBytes >= m_growthCap) {
        m_growthCapHit = true;
        return a;
    }

    // Each chunk is bigger than the last, capped at 8x the base: a level load
    // that needed eleven 8 MB chunks settles in four at 8/16/32/64. The size is
    // then clamped to what is left of the allowance, so growth stops *at* the
    // cap rather than one chunk past it.
    const UINT64 grow = kUploadChunkBytes << (m_grownChunks < 3u ? m_grownChunks : 3u);
    const UINT64 room = m_growthCap - m_grownBytes;
    UINT64 want = std::max<UINT64>(dx12::AlignUp(bytes + align, grow), grow);
    if (want > room) want = std::max<UINT64>(dx12::AlignUp(bytes + align, 65536ull), 65536ull);
    if (want > room) { m_growthCapHit = true; return a; }

    Chunk c{};
    if (!CreateChunk(want, c)) return a;

    c.owner = frameIndex;
    c.fence = useFence;
    m_grownBytes += c.size;
    ++m_grownChunks;
    DXLOG_INFO("[dx12] upload ring grew: chunk #%u of %llu MB (%llu MB of growth "
               "in use, cap %llu MB)",
               m_grownChunks, (unsigned long long)(c.size / (1024ull * 1024ull)),
               (unsigned long long)(m_grownBytes / (1024ull * 1024ull)),
               (unsigned long long)(m_growthCap / (1024ull * 1024ull)));

    m_overflow.push_back(std::move(c));
    a = Carve(m_overflow.back(), bytes, align);
    if (a.Valid()) NotePeak(frameIndex);
    return a;
}

void UploadRing::ResetFrame(UINT frameIndex) noexcept
{
    if (frameIndex >= k_dx12FrameCount) return;
    AcquireSRWLockExclusive(&m_lock);
    m_cursor[frameIndex] = 0;
    for (auto& c : m_overflow)
        if (c.owner == frameIndex) c.cursor = 0;
    ReleaseSRWLockExclusive(&m_lock);
}

HRESULT DeviceContext12::Create(
    HWND hwnd, const D3DPRESENT_PARAMETERS& pp, bool multiThreaded,
    UINT adapterOrdinal, DeviceContext12** ppOut) noexcept
{
    if (!ppOut) return E_POINTER;
    *ppOut = nullptr;
    log::Init();

    auto* self = new (std::nothrow) DeviceContext12();
    if (!self) return E_OUTOFMEMORY;

    self->m_multiThreaded = multiThreaded;
    self->m_targetHwnd    = hwnd ? hwnd : pp.hDeviceWindow;

    UINT factoryFlags = 0;
    if (BackendSelect::DebugLayerEnabled())
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;

    if (FAILED(CreateDXGIFactory2(factoryFlags,
                                  IID_PPV_ARGS(self->m_factory.GetAddressOf())))) {
        DXLOG_FATAL_HR(E_FAIL, "[dx12] CreateDXGIFactory2 failed");
        delete self; return D3DERR_NOTAVAILABLE;
    }

    if (FAILED(self->m_factory->EnumAdapters1(adapterOrdinal,
                                              self->m_adapter.GetAddressOf()))) {
        DXLOG_WARN("[dx12] adapter ordinal %u not present - using adapter 0",
                   adapterOrdinal);
        self->m_adapter.Reset();
        self->m_factory->EnumAdapters1(0, self->m_adapter.GetAddressOf());
    }

    if (FAILED(self->InitD3D12Device(self->m_adapter.Get()))) {
        delete self; return D3DERR_NOTAVAILABLE;
    }
    if (FAILED(self->InitCommandInfra())) {
        DXLOG_FATAL_HR(E_FAIL, "[dx12] command infrastructure init failed");
        delete self; return D3DERR_NOTAVAILABLE;
    }
    if (FAILED(self->InitDescriptorHeaps())) {
        DXLOG_FATAL_HR(E_FAIL, "[dx12] descriptor heap init failed");
        delete self; return D3DERR_NOTAVAILABLE;
    }
    if (FAILED(self->InitUploadRing())) {
        DXLOG_FATAL_HR(E_FAIL, "[dx12] upload ring init failed");
        delete self; return D3DERR_NOTAVAILABLE;
    }
    if (FAILED(self->InitRootSignature())) {
        DXLOG_FATAL_HR(E_FAIL, "[dx12] root signature init failed");
        delete self; return D3DERR_NOTAVAILABLE;
    }

    self->ApplyWindowMode(pp);

    if (FAILED(self->InitSwapChain(self->m_targetHwnd, pp))) {
        DXLOG_FATAL_HR(E_FAIL, "[dx12] swap chain init failed");
        delete self; return D3DERR_NOTAVAILABLE;
    }
    if (FAILED(self->AcquireBackBuffers())) {
        DXLOG_FATAL_HR(E_FAIL, "[dx12] back-buffer acquisition failed");
        delete self; return D3DERR_NOTAVAILABLE;
    }

    if (FAILED(self->OpenCommandList())) {
        delete self; return D3DERR_NOTAVAILABLE;
    }

    self->m_factory->MakeWindowAssociation(self->m_targetHwnd, DXGI_MWA_NO_ALT_ENTER);

    DXLOG_INFO("[dx12] DeviceContext12 ready - %ux%u dxgiFmt=%d d3d9Fmt=%d frames=%u",
               self->m_bbWidth, self->m_bbHeight, (int)self->m_bbFormat,
               (int)self->m_bbD3D9Format, k_dx12FrameCount);

    s_current.store(self, std::memory_order_release);
    *ppOut = self;
    return S_OK;
}

HRESULT DeviceContext12::InitD3D12Device(IDXGIAdapter1* adapter) noexcept
{
    if (BackendSelect::DebugLayerEnabled()) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dbg.GetAddressOf())))) {
            dbg->EnableDebugLayer();
            DXLOG_INFO("[dx12] D3D12 debug layer enabled");
            if (BackendSelect::GpuValidationEnabled()) {
                ComPtr<ID3D12Debug1> dbg1;
                if (SUCCEEDED(dbg.As(&dbg1))) {
                    dbg1->SetEnableGPUBasedValidation(TRUE);
                    DXLOG_INFO("[dx12] GPU-based validation enabled "
                               "(expect a very large slowdown)");
                }
            }
        } else {
            DXLOG_WARN("[dx12] DebugLayer=1 but D3D12GetDebugInterface failed "
                       "(install the Graphics Tools optional feature)");
        }

    }

    // DRED is not part of the debug layer and needs no Graphics Tools install,
    // so it is armed on its own key. Page-fault reporting only changes what the
    // runtime keeps for a post-mortem and is on by default; auto-breadcrumbs
    // write a marker per command and cost real time, so they wait to be asked
    // for. Tying both to DebugLayer=1 meant every field report of a device
    // removal arrived with no breadcrumbs and an instruction the reporter could
    // not follow.
    {
        const bool wantBreadcrumbs = BackendSelect::DredBreadcrumbsEnabled() ||
                                     BackendSelect::DebugLayerEnabled();
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dred.GetAddressOf())))) {
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            if (wantBreadcrumbs)
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            m_dredEnabled     = true;
            m_dredBreadcrumbs = wantBreadcrumbs;
            DXLOG_INFO("[dx12] DRED armed: page-fault reporting on, "
                       "auto-breadcrumbs %s",
                       wantBreadcrumbs ? "on" : "off (set DredBreadcrumbs=1 to enable)");
        } else {
            // Not a fault and not the player's problem, so it is not a warning:
            // it only means a removal, if one ever happens, is reported without
            // allocation detail. ReportDeviceRemoved says so at the point it
            // actually matters.
            DXLOG_INFO("[dx12] DRED is unavailable on this system; a device "
                       "removal will be reported without allocation detail");
        }
    }

    static const D3D_FEATURE_LEVEL kFLs[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };

    for (auto fl : kFLs) {
        if (SUCCEEDED(D3D12CreateDevice(adapter, fl,
                                        IID_PPV_ARGS(m_device.GetAddressOf())))) {
            DXGI_ADAPTER_DESC1 desc{};
            if (adapter) adapter->GetDesc1(&desc);
            DXLOG_INFO("[dx12] device created | adapter: %ls | feature level 0x%04X",
                       desc.Description, static_cast<UINT>(fl));

            if (BackendSelect::DebugLayerEnabled() &&
                SUCCEEDED(m_device.As(&m_infoQueue))) {
                m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
                m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,      FALSE);
                m_infoQueue->SetMuteDebugOutput(FALSE);
                DXLOG_INFO("[dx12] debug-layer messages will be written to this log");
            }
            return S_OK;
        }
    }
    DXLOG_FATAL_HR(E_FAIL, "[dx12] D3D12CreateDevice failed on every feature level");
    return D3DERR_NOTAVAILABLE;
}

HRESULT DeviceContext12::InitCommandInfra() noexcept
{
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(m_cmdQueue.GetAddressOf()))))
        return E_FAIL;

    m_cmdQueue->SetName(L"MWOn12 Direct Queue");

    for (UINT i = 0; i < k_dx12FrameCount; ++i)
        if (FAILED(m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(m_cmdAlloc[i].GetAddressOf()))))
            return E_FAIL;

    if (FAILED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAlloc[0].Get(), nullptr,
            IID_PPV_ARGS(m_cmdList.GetAddressOf()))))
        return E_FAIL;
    m_cmdList->Close();
    m_cmdListOpen = false;

    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(m_fence.GetAddressOf()))))
        return E_FAIL;

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return m_fenceEvent ? S_OK : E_FAIL;
}

HRESULT DeviceContext12::InitDescriptorHeaps() noexcept
{
    if (!m_rtvHeap.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapSize))
        return E_FAIL;
    if (!m_dsvHeap.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapSize))
        return E_FAIL;
    if (!m_srvStaging.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                           kSrvStagingSize))
        return E_FAIL;
    if (!m_sampStaging.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                            kSampStagingSize))
        return E_FAIL;
    if (!m_srvArena.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                         kMaxTextures12, kSrvArenaBlocks))
        return E_FAIL;
    if (!m_sampArena.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                          kMaxSamplers12, kSampArenaBlocks))
        return E_FAIL;
    return S_OK;
}

HRESULT DeviceContext12::InitUploadRing() noexcept
{
    UINT bytes = kUploadBytesPerFrameDefault;
    if (const unsigned mb = BackendSelect::UploadRingMB(); mb != 0)
        bytes = mb * 1024u * 1024u;
    if (!m_upload.Init(m_device.Get(), bytes, kUploadGrowthCapDefault)) {
        DXLOG_WARN("[dx12] upload ring of %u MB per frame could not be created; "
                   "retrying at 12 MB", bytes / (1024u * 1024u));
        bytes = 12u * 1024u * 1024u;
        if (!m_upload.Init(m_device.Get(), bytes, kUploadGrowthCapDefault)) return E_FAIL;
    }
    DXLOG_INFO("[dx12] upload ring: %u MB per frame x %u frames, growing on "
               "demand up to %llu MB",
               bytes / (1024u * 1024u), k_dx12FrameCount,
               (unsigned long long)(kUploadGrowthCapDefault / (1024ull * 1024ull)));
    return S_OK;
}

HRESULT DeviceContext12::InitRootSignature() noexcept
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = kMaxTextures12;
    srvRange.BaseShaderRegister                = 0;
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE sampRange{};
    sampRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampRange.NumDescriptors                    = kMaxSamplers12;
    sampRange.BaseShaderRegister                = 0;
    sampRange.RegisterSpace                     = 0;
    sampRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER p[kRP_Count]{};

    auto rootCbv = [](D3D12_ROOT_PARAMETER& rp, UINT reg,
                      D3D12_SHADER_VISIBILITY vis) {
        rp.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp.Descriptor.ShaderRegister = reg;
        rp.Descriptor.RegisterSpace  = 0;
        rp.ShaderVisibility          = vis;
    };

    rootCbv(p[kRP_VSConst], 0, D3D12_SHADER_VISIBILITY_VERTEX);
    rootCbv(p[kRP_PSConst], 0, D3D12_SHADER_VISIBILITY_PIXEL);
    rootCbv(p[kRP_VSEmu],   1, D3D12_SHADER_VISIBILITY_VERTEX);
    rootCbv(p[kRP_PSEmu],   1, D3D12_SHADER_VISIBILITY_PIXEL);

    p[kRP_SRVs].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[kRP_SRVs].DescriptorTable.NumDescriptorRanges = 1;
    p[kRP_SRVs].DescriptorTable.pDescriptorRanges   = &srvRange;
    p[kRP_SRVs].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    p[kRP_Samplers].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[kRP_Samplers].DescriptorTable.NumDescriptorRanges = 1;
    p[kRP_Samplers].DescriptorTable.pDescriptorRanges   = &sampRange;
    p[kRP_Samplers].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = kRP_Count;
    rs.pParameters   = p;
    rs.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS       |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS     |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                             blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        if (err)
            DXLOG_ERROR("[dx12] root signature serialize: %s",
                        static_cast<const char*>(err->GetBufferPointer()));
        return hr;
    }
    return m_device->CreateRootSignature(0, blob->GetBufferPointer(),
                                         blob->GetBufferSize(),
                                         IID_PPV_ARGS(m_rootSig.GetAddressOf()));
}

DXGI_FORMAT DeviceContext12::ResolveBackBufferFormat(
    const D3DPRESENT_PARAMETERS& pp) const noexcept
{
    return FormatConverter::BackBufferFormat(pp.BackBufferFormat);
}

HRESULT DeviceContext12::InitSwapChain(HWND hwnd, const D3DPRESENT_PARAMETERS& pp) noexcept
{
    m_bbFormat      = ResolveBackBufferFormat(pp);
    m_bbD3D9Format  = pp.BackBufferFormat != D3DFMT_UNKNOWN ? pp.BackBufferFormat
                                                            : D3DFMT_X8R8G8B8;
    m_bbWidth       = pp.BackBufferWidth  ? pp.BackBufferWidth  : 1280u;
    m_bbHeight      = pp.BackBufferHeight ? pp.BackBufferHeight : 1024u;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width       = m_bbWidth;
    sd.Height      = m_bbHeight;
    sd.Format      = m_bbFormat;
    sd.Stereo      = FALSE;
    sd.SampleDesc  = { 1, 0 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = k_dx12FrameCount;
    sd.Scaling     = DXGI_SCALING_STRETCH;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags       = 0;

    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = m_factory->CreateSwapChainForHwnd(
        m_cmdQueue.Get(), hwnd, &sd, nullptr, nullptr, sc1.GetAddressOf());
    if (FAILED(hr)) {
        DXLOG_HR(hr, "[dx12] CreateSwapChainForHwnd %ux%u fmt=%d",
                 m_bbWidth, m_bbHeight, (int)m_bbFormat);
        return hr;
    }
    return sc1.As(&m_swapChain);
}

HRESULT DeviceContext12::AcquireBackBuffers() noexcept
{
    for (UINT i = 0; i < k_dx12FrameCount; ++i) {
        if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(m_backBuffers[i].GetAddressOf()))))
            return E_FAIL;
        m_bbState[i] = D3D12_RESOURCE_STATE_PRESENT;
        wchar_t name[32];
        _snwprintf_s(name, _TRUNCATE, L"BackBuffer%u", i);
        m_backBuffers[i]->SetName(name);
    }
    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    return S_OK;
}

void DeviceContext12::ReleaseBackBuffers() noexcept
{
    for (auto& bb : m_backBuffers)
        bb.Reset();
}

HRESULT DeviceContext12::RecreateCommandList() noexcept
{
    ComPtr<ID3D12GraphicsCommandList> fresh;
    const HRESULT hr = m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAlloc[m_frameIndex].Get(),
        nullptr, IID_PPV_ARGS(fresh.GetAddressOf()));
    if (FAILED(hr)) {
        DXLOG_HR(hr, "[dx12] could not replace the command list after a failed "
                     "Close; rendering cannot continue");
        return hr;
    }

    // The old list may still be referenced by work submitted in earlier frames,
    // so it retires on the fence rather than being released here.
    if (m_cmdList) Retire(m_cmdList.Get());
    m_cmdList = fresh;
    m_cmdList->SetName(L"MWOn12 Direct List");

    // CreateCommandList hands the list back already recording.
    m_cmdListOpen   = true;
    m_cmdStateDirty = true;
    return S_OK;
}

HRESULT DeviceContext12::OpenCommandList() noexcept
{
    if (m_cmdListOpen) return S_OK;

    // Once the device is gone every Reset fails for the same reason, and the
    // interesting log line is the removal itself, several hundred lines up.
    // Report the consequence once and then answer with the cause.
    if (m_deviceLost) {
        if (!m_resetFailReported) {
            m_resetFailReported = true;
            DXLOG_WARN("[dx12] the command list cannot be reopened because the "
                       "device was removed; no further rendering will be "
                       "attempted this session");
        }
        return DXGI_ERROR_DEVICE_REMOVED;
    }

    if (m_cmdListBroken) {
        const HRESULT hrNew = RecreateCommandList();
        if (SUCCEEDED(hrNew)) {
            m_cmdListBroken = false;
            DXLOG_WARN("[dx12] command list replaced after a failed Close; "
                       "rendering resumes from this frame");
            return S_OK;
        }
        return hrNew;
    }

    HRESULT hr = m_cmdList->Reset(m_cmdAlloc[m_frameIndex].Get(), nullptr);
    if (FAILED(hr)) {
        // If the device is already gone, this Reset is a consequence, not the
        // fault. Say so at ERROR and leave the removal to be reported by a site
        // that can attribute it -- the fatal dialog is shown once per session,
        // so letting a symptom claim it buries the useful message.
        const bool removed = m_device &&
                             m_device->GetDeviceRemovedReason() != S_OK;
        if (!m_resetFailReported) {
            m_resetFailReported = true;
            DXLOG_HR(hr, "[dx12] command list Reset failed for frame slot %u "
                         "(allocator %p)%s; reported once",
                     m_frameIndex, (void*)m_cmdAlloc[m_frameIndex].Get(),
                     removed ? " because the device was already removed - the "
                               "removal itself is the fault, not this call"
                             : "");
        }
        if (removed) {
            m_deviceLost = true;
            ReportDeviceRemoved("GPU work submitted before this frame");
        }
        return hr;
    }
    m_cmdList->SetName(L"MWOn12 Direct List");
    m_cmdListOpen   = true;
    m_cmdStateDirty = true;
    return S_OK;
}

void DeviceContext12::BindDescriptorHeaps() noexcept
{
    if (!m_cmdListOpen) return;
    ID3D12DescriptorHeap* heaps[2] = { m_srvArena.Heap(), m_sampArena.Heap() };
    m_cmdList->SetDescriptorHeaps(2, heaps);
}

// Closes the open command list and submits it -- but only if it closed
// cleanly.
//
// ID3D12GraphicsCommandList::Close returns the first error that occurred at any
// point during recording. D3D12 does not fail the offending call; it poisons
// the list and tells you at Close. Submitting a list that failed to close is
// undefined, and in practice the driver answers it by removing the device with
// DXGI_ERROR_INVALID_CALL -- after which Reset on that same list returns
// E_INVALIDARG, because a list that did not close cannot be reset.
//
// That is one silently-discarded HRESULT producing three separate symptoms,
// none of which names the recording error that started it. Dropping the frame
// costs a black flash; submitting it costs the device.
bool DeviceContext12::CloseAndSubmit(const char* where) noexcept
{
    if (!m_cmdListOpen) return false;
    m_cmdListOpen = false;

    const HRESULT hrClose = m_cmdList->Close();
    if (FAILED(hrClose)) {
        // A list that failed to close also cannot be reset, so it has to be
        // replaced rather than reused. OpenCommandList does that, once the
        // frame's allocator has been reset.
        m_cmdListBroken = true;
        ++m_closeFailures;
        if (m_closeFailures <= 4u) {
            DXLOG_ERROR("[dx12] command list Close failed at %s, frame %llu "
                        "(failure #%u)  ->  hr=0x%08X. An error occurred while "
                        "recording this frame; D3D12 reports it here rather than "
                        "at the call that caused it. The frame is being DROPPED "
                        "rather than submitted - submitting a list that failed to "
                        "close removes the device.",
                        where, (unsigned long long)m_frameCounter,
                        m_closeFailures, (unsigned)hrClose);
            DrainInfoQueue();
            if (!BackendSelect::DebugLayerEnabled())
                DXLOG_ERROR("[dx12] set DebugLayer=1 in MWOn12.ini to have the "
                            "runtime name the recording call that failed");
        }
        // The barriers in this list never ran, so every tracked state they
        // advanced has to go back. Skipping this was what turned one dropped
        // frame into a cascade of "before state does not match" errors on the
        // following frames, and then a device removal.
        RollbackStateJournal();
        return false;
    }

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
    m_stateJournal.clear();
    return true;
}

HRESULT DeviceContext12::FlushAndWait() noexcept
{
    if (!m_cmdListOpen)
        return S_OK;

    CloseAndSubmit("FlushAndWait");

    const UINT64 fenceVal = ++m_globalFenceValue;
    m_cmdQueue->Signal(m_fence.Get(), fenceVal);
    if (m_fence->GetCompletedValue() < fenceVal) {
        m_fence->SetEventOnCompletion(fenceVal, m_fenceEvent);
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }

    return OpenCommandList();
}

HRESULT DeviceContext12::PresentFrame(UINT syncInterval) noexcept
{
    if (!m_cmdListOpen) {
        HRESULT hr = OpenCommandList();
        if (FAILED(hr)) return hr;
    }

    const UINT bb = m_backBufferIndex;
    if (m_bbState[bb] != D3D12_RESOURCE_STATE_PRESENT) {
        const auto b = dx12::TransitionBarrier(m_backBuffers[bb].Get(),
                                               m_bbState[bb],
                                               D3D12_RESOURCE_STATE_PRESENT);
        m_cmdList->ResourceBarrier(1, &b);
        JournalBackBufferState(bb, m_bbState[bb]);
        m_bbState[bb] = D3D12_RESOURCE_STATE_PRESENT;
    }

    CloseAndSubmit("PresentFrame");

    const HRESULT hrPresent = m_swapChain->Present(syncInterval, 0);
    NotePresentResult(hrPresent);
    DrainInfoQueue();
    if (FAILED(hrPresent)) {
        DXLOG_FATAL_HR(hrPresent, "[dx12] IDXGISwapChain::Present failed "
                                  "(syncInterval=%u frame=%llu)",
                       syncInterval,
                       static_cast<unsigned long long>(m_frameCounter));
        ReportDeviceRemoved("Present");
    }

    const UINT64 fenceVal = ++m_globalFenceValue;
    m_cmdQueue->Signal(m_fence.Get(), fenceVal);
    m_frameFence[m_frameIndex] = fenceVal;

    m_frameIndex      = (m_frameIndex + 1) % k_dx12FrameCount;
    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    ++m_frameCounter;

    if (m_fence->GetCompletedValue() < m_frameFence[m_frameIndex]) {
        m_fence->SetEventOnCompletion(m_frameFence[m_frameIndex], m_fenceEvent);
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }

    // An allocator still referenced by in-flight work refuses to reset, and the
    // failure only shows up later as an E_INVALIDARG from the command list's
    // own Reset. Catching it here says which of the two actually went wrong,
    // and a full wait makes the retry legal.
    if (HRESULT hrAlloc = m_cmdAlloc[m_frameIndex]->Reset(); FAILED(hrAlloc)) {
        DXLOG_WARN_HR(hrAlloc, "[dx12] command allocator for frame slot %u would "
                               "not reset at frame %llu; waiting for the GPU and "
                               "retrying", m_frameIndex,
                      (unsigned long long)m_frameCounter);
        WaitForGPU();
        if (HRESULT hrRetry = m_cmdAlloc[m_frameIndex]->Reset(); FAILED(hrRetry))
            DXLOG_HR(hrRetry, "[dx12] command allocator for frame slot %u still "
                              "will not reset after a full GPU wait", m_frameIndex);
    }
    m_upload.ResetFrame(m_frameIndex);
    CollectGarbage();

    if (m_srvArena.Full() || m_sampArena.Full()) {
        const bool srvFull  = m_srvArena.Full();
        const bool sampFull = m_sampArena.Full();
        WaitForGPU();
        m_srvArena.Reset();
        m_sampArena.Reset();

        static unsigned s_recycles = 0;
        ++s_recycles;
        DXLOG_INFO("[dx12] descriptor arena recycled (%s%s full) - recycle #%u at "
                   "frame %llu; this is a full GPU stall",
                   srvFull ? "SRV" : "", sampFull ? " sampler" : "",
                   s_recycles, (unsigned long long)m_frameCounter);
    }

    const HRESULT hrOpen = OpenCommandList();
    if (FAILED(hrOpen)) return hrOpen;
    return hrPresent;
}

void DeviceContext12::WaitForGPU() noexcept
{
    if (!m_cmdQueue || !m_fence || !m_fenceEvent) return;
    const UINT64 v = ++m_globalFenceValue;
    if (FAILED(m_cmdQueue->Signal(m_fence.Get(), v))) return;
    if (m_fence->GetCompletedValue() < v) {
        m_fence->SetEventOnCompletion(v, m_fenceEvent);
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }
    for (auto& f : m_frameFence) f = std::min(f, v);
}

UploadRing::Allocation DeviceContext12::AllocUpload(UINT64 bytes, UINT64 align) noexcept
{
    auto a = m_upload.Alloc(m_frameIndex, bytes, align, CompletedFence(),
                            m_globalFenceValue + 1);
    if (a.Valid())
        return a;

    // The ring's per-frame slice and its growth allowance are both spent. Each
    // buffer taken here is mapped and held until the frame retires, and this
    // is a 32-bit process: roughly 800 of them exhaust the 2 GB user address
    // space outright, after which every allocation in the process fails. The
    // growth chunks above exist so this path is reached only when a single
    // frame genuinely needs more than the cap, and the message says what to
    // change rather than only what happened.
    if (!m_uploadCapReported) {
        m_uploadCapReported = true;
        DXLOG_WARN("[dx12] upload ring and its %llu MB growth allowance are both "
                   "spent at frame %llu (peak %llu MB in one frame, this request "
                   "%llu bytes). Falling back to committed buffers, which is slow "
                   "and consumes address space in this 32-bit process - raise "
                   "UploadRingMB in MWOn12.ini to at least %llu.",
                   (unsigned long long)(m_upload.GrowthCap() / (1024ull * 1024ull)),
                   (unsigned long long)m_frameCounter,
                   (unsigned long long)(m_upload.PeakFrameBytes() / (1024ull * 1024ull)),
                   (unsigned long long)bytes,
                   (unsigned long long)((m_upload.PeakFrameBytes() / (1024ull * 1024ull)) + 8ull));
    }
    const auto hp = dx12::HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const auto rd = dx12::BufferDesc(dx12::AlignUp<UINT64>(bytes, align ? align : 1));
    ComPtr<ID3D12Resource> tmp;
    if (FAILED(m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(tmp.GetAddressOf())))) {
        DXLOG_ERROR("[dx12] upload fallback allocation of %llu bytes failed",
                    (unsigned long long)bytes);
        return {};
    }
    uint8_t* cpu = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(tmp->Map(0, &noRead, reinterpret_cast<void**>(&cpu))))
        return {};

    UploadRing::Allocation out{};
    out.cpu      = cpu;
    out.gpu      = tmp->GetGPUVirtualAddress();
    out.resource = tmp.Get();
    out.offset   = 0;
    Retire(tmp.Get());
    return out;
}

void DeviceContext12::Retire(ID3D12Resource* res) noexcept
{
    if (!res) return;
    Retired r;
    r.object = res;
    r.fence  = m_globalFenceValue + 1;
    m_retired.push_back(std::move(r));
}

void DeviceContext12::Retire(ID3D12PipelineState* pso) noexcept
{
    if (!pso) return;
    Retired r;
    r.object = pso;
    r.fence  = m_globalFenceValue + 1;
    m_retired.push_back(std::move(r));
}

void DeviceContext12::Retire(ID3D12GraphicsCommandList* list) noexcept
{
    if (!list) return;
    Retired r;
    r.object = list;
    r.fence  = m_globalFenceValue + 1;
    m_retired.push_back(std::move(r));
}

void DeviceContext12::CollectGarbage() noexcept
{
    if (m_retired.empty() || !m_fence) return;
    const UINT64 completed = m_fence->GetCompletedValue();
    while (!m_retired.empty() && m_retired.front().fence <= completed)
        m_retired.pop_front();
}

HRESULT DeviceContext12::ResizeSwapChain(UINT w, UINT h,
                                         const D3DPRESENT_PARAMETERS& pp) noexcept
{

    if (m_deviceLost) return DXGI_ERROR_DEVICE_REMOVED;

    CloseAndSubmit("ResizeSwapChain");
    WaitForGPU();
    CollectGarbage();
    ReleaseBackBuffers();

    m_bbWidth      = w ? w : 1;
    m_bbHeight     = h ? h : 1;
    m_bbFormat     = ResolveBackBufferFormat(pp);
    m_bbD3D9Format = pp.BackBufferFormat != D3DFMT_UNKNOWN ? pp.BackBufferFormat
                                                           : m_bbD3D9Format;

    HRESULT hr = m_swapChain->ResizeBuffers(k_dx12FrameCount, m_bbWidth, m_bbHeight,
                                            m_bbFormat, 0);
    if (FAILED(hr)) {

        static UINT s_lastW = 0, s_lastH = 0;
        if (w != s_lastW || h != s_lastH) {
            s_lastW = w; s_lastH = h;
            DXLOG_HR(hr, "[dx12] ResizeBuffers %ux%u", m_bbWidth, m_bbHeight);
        }
        ReportDeviceRemoved("ResizeBuffers");
        return hr;
    }

    hr = AcquireBackBuffers();
    if (FAILED(hr)) return hr;

    for (UINT i = 0; i < k_dx12FrameCount; ++i)
        m_cmdAlloc[i]->Reset();
    m_frameIndex = 0;
    for (UINT i = 0; i < k_dx12FrameCount; ++i)
        m_upload.ResetFrame(i);
    m_srvArena.Reset();
    m_sampArena.Reset();

    return OpenCommandList();
}

void DeviceContext12::ApplyWindowMode(const D3DPRESENT_PARAMETERS& pp) noexcept
{
    if (!m_targetHwnd || !BackendSelect::BorderlessFullscreenEnabled())
        return;
    if (pp.Windowed)
        return;

    HMONITOR mon = MonitorFromWindow(m_targetHwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!mon || !GetMonitorInfoW(mon, &mi))
        return;

    LONG_PTR style = GetWindowLongPtrW(m_targetHwnd, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW);
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongPtrW(m_targetHwnd, GWL_STYLE, style);
    SetWindowPos(m_targetHwnd, HWND_TOP,
                 mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right  - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    static bool s_borderlessLogged = false;
    if (!s_borderlessLogged) {
        s_borderlessLogged = true;
        DXLOG_INFO("[dx12] exclusive fullscreen request served as borderless window");
    }
}

void DeviceContext12::LockContext() noexcept
{
    if (m_multiThreaded) AcquireSRWLockExclusive(&m_ctxLock);
}
void DeviceContext12::UnlockContext() noexcept
{
    if (m_multiThreaded) ReleaseSRWLockExclusive(&m_ctxLock);
}

void DeviceContext12::NotePresentResult(HRESULT hr) noexcept
{
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ||
        hr == DXGI_ERROR_DEVICE_RESET)
        m_deviceLost = true;
}

void DeviceContext12::DrainInfoQueue() noexcept
{
    if (!m_infoQueue) return;

    static std::unordered_map<int, unsigned> s_seen;
    constexpr unsigned kPerIdCap = 8u;

    const UINT64 n = m_infoQueue->GetNumStoredMessages();
    std::vector<uint8_t> buf;
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(m_infoQueue->GetMessage(i, nullptr, &len)) || len == 0) continue;
        buf.resize(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(m_infoQueue->GetMessage(i, msg, &len))) continue;

        const char* sev = "INFO";
        switch (msg->Severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: sev = "CORRUPTION"; break;
        case D3D12_MESSAGE_SEVERITY_ERROR:      sev = "ERROR";      break;
        case D3D12_MESSAGE_SEVERITY_WARNING:    sev = "WARNING";    break;
        default: break;
        }
        const unsigned seen = ++s_seen[static_cast<int>(msg->ID)];
        if (seen > kPerIdCap) {
            if (seen == kPerIdCap + 1u)
                DXLOG_WARN("[d3d12-debug][id=%d] further occurrences suppressed",
                           static_cast<int>(msg->ID));
            continue;
        }

        const int chars = static_cast<int>(msg->DescriptionByteLength);
        if (msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            DXLOG_WARN("[d3d12-debug][%s][id=%d] %.*s", sev,
                       static_cast<int>(msg->ID), chars, msg->pDescription);
        else
            DXLOG_TRACE("[d3d12-debug][%s][id=%d] %.*s", sev,
                        static_cast<int>(msg->ID), chars, msg->pDescription);
    }
    m_infoQueue->ClearStoredMessages();
}

bool DeviceContext12::NoteDeviceRemoved(HRESULT hr, const char* where) noexcept
{
    if (m_deviceLost) return true;

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ||
        hr == DXGI_ERROR_DEVICE_RESET) {
        ReportDeviceRemoved(where);
        return true;
    }
    return false;
}

void DeviceContext12::JournalState(Resource12* res, UINT sub,
                                   D3D12_RESOURCE_STATES before) noexcept
{
    if (!res) return;
    m_stateJournal.push_back(StateJournalEntry{ res, sub, 0u, before });
}

void DeviceContext12::JournalBackBufferState(UINT index,
                                             D3D12_RESOURCE_STATES before) noexcept
{
    m_stateJournal.push_back(StateJournalEntry{
        nullptr, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, index, before });
}

void DeviceContext12::ForgetInStateJournal(const Resource12* res) noexcept
{
    if (!res || m_stateJournal.empty()) return;
    for (auto& e : m_stateJournal)
        if (e.res == res) e.res = nullptr, e.bbIndex = UINT(-1);
}

void DeviceContext12::RollbackStateJournal() noexcept
{
    // Reverse order, so a subresource touched more than once in the dropped
    // list ends on the state it held when the list was opened.
    for (auto it = m_stateJournal.rbegin(); it != m_stateJournal.rend(); ++it) {
        if (it->res) {
            it->res->RestoreState(it->sub, it->before);
        } else if (it->bbIndex < k_dx12FrameCount) {
            m_bbState[it->bbIndex] = it->before;
        }
    }
    if (!m_stateJournal.empty())
        DXLOG_WARN("[dx12] %zu recorded resource transitions were rolled back "
                   "with the dropped command list, so state tracking still "
                   "matches what the GPU actually did",
                   m_stateJournal.size());
    m_stateJournal.clear();
}

void DeviceContext12::ReportMemoryState(const char* why) noexcept
{
    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        DXLOG_WARN("[dx12] %s | process address space: %llu MB free of %llu MB "
                   "(%u-bit process) | system RAM %llu MB free of %llu MB",
                   why,
                   (unsigned long long)(ms.ullAvailVirtual / (1024ull * 1024ull)),
                   (unsigned long long)(ms.ullTotalVirtual / (1024ull * 1024ull)),
                   (unsigned)(sizeof(void*) * 8),
                   (unsigned long long)(ms.ullAvailPhys / (1024ull * 1024ull)),
                   (unsigned long long)(ms.ullTotalPhys / (1024ull * 1024ull)));
    }

    ComPtr<IDXGIAdapter3> adapter3;
    if (m_adapter && SUCCEEDED(m_adapter.As(&adapter3))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
            DXLOG_WARN("[dx12] %s | video memory: %llu MB in use of %llu MB "
                       "budget (%llu MB reserved)", why,
                       (unsigned long long)(local.CurrentUsage / (1024ull * 1024ull)),
                       (unsigned long long)(local.Budget / (1024ull * 1024ull)),
                       (unsigned long long)(local.CurrentReservation / (1024ull * 1024ull)));
        }
    }

    DXLOG_WARN("[dx12] %s | upload ring: %llu MB per frame, %u growth chunks "
               "(%llu MB), peak %llu MB in one frame%s", why,
               (unsigned long long)(m_upload.BytesPerFrame() / (1024ull * 1024ull)),
               m_upload.GrownChunks(),
               (unsigned long long)(m_upload.GrownBytes() / (1024ull * 1024ull)),
               (unsigned long long)(m_upload.PeakFrameBytes() / (1024ull * 1024ull)),
               m_upload.GrowthCapHit() ? " - GROWTH CAP WAS HIT" : "");
}

void DeviceContext12::ReportDeviceRemoved(const char* where) noexcept
{
    if (m_removedReported || !m_device) return;
    const HRESULT reason = m_device->GetDeviceRemovedReason();
    if (reason == S_OK) return;
    m_removedReported = true;
    m_deviceLost      = true;

    // Everything diagnostic is captured BEFORE the fatal line, because the
    // fatal line raises a modal message box and blocks there until the player
    // dismisses it. In the first field report that gap was 39 seconds, during
    // which the log held the error and none of the evidence for it; a player
    // who kills the process from the dialog would have sent a useless log.
    ReportMemoryState("device removal");
    DrainInfoQueue();
    ReportDredOutput(reason);

    // This text is what the player sees in the error box, so it has to say what
    // actually happened rather than where we happened to notice. A removal is
    // always caused by GPU work that has already been submitted; the call that
    // returns the error is downstream of it. Naming that call as the cause sent
    // one debugging session after the wrong subsystem.
    DXLOG_FATAL_HR(reason,
                   "[dx12] The graphics device was removed by the driver, so "
                   "rendering cannot continue.\n"
                   "Detected at: %s (the fault is in GPU work already submitted, "
                   "not in this call).\n"
                   "%s",
                   where,
                   m_dredBreadcrumbs
                       ? "Breadcrumbs were armed - the faulting command is named "
                         "further down this log."
                       : "To find the cause: set DredBreadcrumbs=1 in MWOn12.ini, "
                         "reproduce this, and send the log. That names the exact "
                         "GPU command that faulted and needs nothing installed.");

}

void DeviceContext12::ReportDredOutput(HRESULT reason) noexcept
{
    if (!m_dredEnabled) {
        DXLOG_WARN("[dx12] DRED is not armed, so there is no allocation or "
                   "breadcrumb detail for this removal - re-run with "
                   "DredBreadcrumbs=1 in MWOn12.ini");
        return;
    }

    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(m_device.As(&dred))) return;

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    if (m_dredBreadcrumbs && SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
        unsigned nodes = 0;
        for (const auto* node = crumbs.pHeadAutoBreadcrumbNode;
             node && nodes < 8u; node = node->pNext, ++nodes) {
            const UINT done  = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            const UINT total = node->BreadcrumbCount;
            DXLOG_FATAL_HR(reason,
                           "[dx12][DRED] list '%ls' queue '%ls': %u of %u ops completed",
                           node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"(unnamed)",
                           node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"(unnamed)",
                           done, total);

            if (node->pCommandHistory && done < total)
                DXLOG_FATAL_HR(reason,
                               "[dx12][DRED]   faulting op index %u = AUTO_BREADCRUMB_OP %u",
                               done, static_cast<unsigned>(node->pCommandHistory[done]));
        }
        if (nodes == 0)
            DXLOG_WARN("[dx12][DRED] no breadcrumb nodes were recorded");
    } else if (!m_dredBreadcrumbs) {
        DXLOG_WARN("[dx12][DRED] auto-breadcrumbs were off, so the faulting "
                   "command is not identified - re-run with DredBreadcrumbs=1 "
                   "in MWOn12.ini to get it");
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    const bool havePf = SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf));
    if (!havePf || pf.PageFaultVA == 0) {
        // Worth stating rather than leaving blank. Page-fault reporting was
        // armed, so "nothing recorded" means the GPU did not fault on an
        // address: the removal came from an invalid command or parameter, not
        // from reading freed or unmapped memory. That halves the suspect list.
        DXLOG_WARN("[dx12][DRED] no GPU page fault was recorded. The device was "
                   "not removed by a bad address, so look for an invalid "
                   "command or parameter rather than a lifetime bug.");
        return;
    }
    {
        DXLOG_FATAL_HR(reason, "[dx12][DRED] page fault at GPU VA 0x%llX",
                       static_cast<unsigned long long>(pf.PageFaultVA));
        for (const auto* a = pf.pHeadExistingAllocationNode; a; a = a->pNext)
            DXLOG_FATAL_HR(reason, "[dx12][DRED]   live allocation '%ls' type %u",
                           a->ObjectNameW ? a->ObjectNameW : L"(unnamed)",
                           static_cast<unsigned>(a->AllocationType));
        for (const auto* a = pf.pHeadRecentFreedAllocationNode; a; a = a->pNext)
            DXLOG_FATAL_HR(reason, "[dx12][DRED]   RECENTLY FREED '%ls' type %u",
                           a->ObjectNameW ? a->ObjectNameW : L"(unnamed)",
                           static_cast<unsigned>(a->AllocationType));
    }
}

DeviceContext12::~DeviceContext12()
{
    if (m_cmdListOpen && m_cmdList) {
        m_cmdList->Close();
        m_cmdListOpen = false;
    }
    WaitForGPU();
    m_retired.clear();
    m_upload.Shutdown();

    DeviceContext12* expected = this;
    s_current.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);

    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
}

void DeviceContext12::Destroy() noexcept { delete this; }

DeviceContext12* DeviceContext12::Current() noexcept
{
    return s_current.load(std::memory_order_acquire);
}

}
