// Owns the D3D12 device, the command queue, and the machinery a D3D12 renderer
// needs that D3D11 provided for free.
//
// Frames are pipelined: several are in flight at once, each with its own
// command allocator and its own slice of upload memory, and a fence tracks
// which have completed. Reusing either before the GPU has finished with it
// corrupts work already recorded, so every per-frame resource is indexed by
// the frame in flight rather than shared.
//
// Descriptor heaps are managed in two parts, which is the reason for the two
// classes below.
//
//   CpuDescriptorHeap is staging. Descriptors are created here when a resource
//   is created and live as long as it does. The heap is not shader-visible.
//
//   GpuDescriptorArena is what the shader sees. Descriptors are copied into it
//   in the layout a draw needs, in blocks, and it is refilled per frame.
//
// Copying a block per draw is too slow, so blocks are cached. That cache is
// keyed on the descriptor handles it was built from, plus a per-slot
// generation counter -- because a freed staging slot is reissued immediately,
// so handles alone are not unique over time and a new texture would otherwise
// match a cached block describing a destroyed one.
//
// The debug layer's messages are pumped into the log rather than left for a
// debugger. Without that, validation output only reaches an attached debugger,
// which makes any failure during startup undiagnosable in a normal run.

#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3d9.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include <core/Dx12Common.h>

namespace mwon12 {

using Microsoft::WRL::ComPtr;

class Resource12;

static constexpr UINT k_dx12FrameCount = 3;

class CpuDescriptorHeap {
public:
    bool Init(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity) noexcept;

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Alloc() noexcept;
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept;

    [[nodiscard]] uint32_t GenerationOf(D3D12_CPU_DESCRIPTOR_HANDLE h) const noexcept;

    [[nodiscard]] bool  Valid() const noexcept { return m_heap != nullptr; }
    [[nodiscard]] UINT  Stride() const noexcept { return m_stride; }

private:
    ComPtr<ID3D12DescriptorHeap>             m_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE              m_base{};
    UINT                                     m_stride{ 0 };
    UINT                                     m_capacity{ 0 };
    UINT                                     m_used{ 0 };
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_free;
    std::vector<uint32_t>                    m_generation;
};

class GpuDescriptorArena {
public:
    bool Init(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type,
              UINT blockSize, UINT blockCount) noexcept;

    [[nodiscard]] ID3D12DescriptorHeap* Heap() const noexcept { return m_heap.Get(); }
    [[nodiscard]] bool Valid() const noexcept { return m_heap != nullptr; }
    [[nodiscard]] UINT Stride() const noexcept { return m_stride; }

    bool GetOrCreate(ID3D12Device* dev, uint64_t key,
                     const D3D12_CPU_DESCRIPTOR_HANDLE* srcHandles,
                     D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) noexcept;

    void Reset() noexcept;

    [[nodiscard]] bool Full() const noexcept { return m_used >= m_blockCount; }

private:
    ComPtr<ID3D12DescriptorHeap>                     m_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE                      m_cpuBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE                      m_gpuBase{};
    D3D12_DESCRIPTOR_HEAP_TYPE                       m_type{};
    UINT                                             m_stride{ 0 };
    UINT                                             m_blockSize{ 0 };
    UINT                                             m_blockCount{ 0 };
    UINT                                             m_used{ 0 };
    std::unordered_map<uint64_t, D3D12_GPU_DESCRIPTOR_HANDLE> m_cache;
};

// A frame that overruns its slice of the ring grows into extra chunks rather
// than taking a committed buffer per allocation. The chunks are kept and
// reused on every later visit to that frame index, so a streaming burst pays
// for its peak once instead of allocating thousands of buffers -- which in a
// 32-bit process is not a performance question but an address-space one: a
// mapped 2 MB buffer per allocation exhausts the 2 GB user range in under a
// thousand allocations, and every allocation after that fails.
class UploadRing {
public:
    bool Init(ID3D12Device* dev, UINT bytesPerFrame, UINT64 growthCap) noexcept;
    void Shutdown() noexcept;

    struct Allocation {
        uint8_t*                  cpu{ nullptr };
        D3D12_GPU_VIRTUAL_ADDRESS gpu{ 0 };
        ID3D12Resource*           resource{ nullptr };
        UINT64                    offset{ 0 };
        [[nodiscard]] bool Valid() const noexcept { return cpu != nullptr; }
    };

    // `completedFence` lets a growth chunk last used by a different frame slot
    // be taken over once the GPU has finished with it, instead of every slot
    // growing its own copy of the peak.
    [[nodiscard]] Allocation Alloc(UINT frameIndex, UINT64 bytes, UINT64 align,
                                   UINT64 completedFence, UINT64 useFence) noexcept;

    void ResetFrame(UINT frameIndex) noexcept;

    [[nodiscard]] UINT64 BytesPerFrame()  const noexcept { return m_perFrame; }
    [[nodiscard]] UINT64 GrowthCap()      const noexcept { return m_growthCap; }
    [[nodiscard]] UINT64 GrownBytes()     const noexcept { return m_grownBytes; }
    [[nodiscard]] UINT   GrownChunks()    const noexcept { return m_grownChunks; }
    [[nodiscard]] UINT64 PeakFrameBytes() const noexcept { return m_peakFrameBytes; }
    [[nodiscard]] bool   GrowthCapHit()   const noexcept { return m_growthCapHit; }

private:
    struct Chunk {
        ComPtr<ID3D12Resource>    buffer;
        uint8_t*                  cpu{ nullptr };
        D3D12_GPU_VIRTUAL_ADDRESS gpu{ 0 };
        UINT64                    size{ 0 };
        UINT64                    cursor{ 0 };
        UINT                      owner{ UINT(-1) };  // frame slot currently using it
        UINT64                    fence{ 0 };         // GPU must pass this to reuse
    };

    [[nodiscard]] static Allocation Carve(Chunk& c, UINT64 bytes, UINT64 align) noexcept;
    [[nodiscard]] bool CreateChunk(UINT64 bytes, Chunk& out) noexcept;
    void NotePeak(UINT frameIndex) noexcept;

    // Allocation is locked unconditionally. The cursor bump was already racy
    // when a game created the device D3DCREATE_MULTITHREADED -- DeviceContext12
    // has a LockContext(), but nothing has ever called it -- and a race there
    // only produced overlapping allocations. Growth chunks live in a vector,
    // and a torn push_back hands out a wild GPU address, which the driver
    // answers by removing the device. Uncontended SRW acquisition is a few
    // nanoseconds against several thousand allocations a frame.
    SRWLOCK                   m_lock = SRWLOCK_INIT;
    ID3D12Device*             m_device{ nullptr };
    ComPtr<ID3D12Resource>    m_buffer;
    uint8_t*                  m_cpuPtr{ nullptr };
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuBase{ 0 };
    UINT64                    m_perFrame{ 0 };
    UINT64                    m_cursor[k_dx12FrameCount]{};
    // One shared pool, not one list per frame slot. Per-slot lists meant the
    // same peak was paid for up to three times over: a field log grew 248 MB
    // of chunks to serve a 110 MB frame.
    std::vector<Chunk>        m_overflow;
    UINT64                    m_growthCap{ 0 };
    UINT64                    m_grownBytes{ 0 };
    UINT                      m_grownChunks{ 0 };
    UINT64                    m_peakFrameBytes{ 0 };
    bool                      m_growthCapHit{ false };
};

enum RootParam12 : UINT {
    kRP_VSConst = 0,
    kRP_PSConst = 1,
    kRP_VSEmu   = 2,
    kRP_PSEmu   = 3,
    kRP_SRVs    = 4,
    kRP_Samplers= 5,
    kRP_Count   = 6
};

inline constexpr UINT kMaxTextures12 = 16;
inline constexpr UINT kMaxSamplers12 = 16;

class DeviceContext12 {
public:
    static HRESULT Create(HWND hwnd, const D3DPRESENT_PARAMETERS& pp,
                          bool multiThreaded, UINT adapterOrdinal,
                          DeviceContext12** ppOut) noexcept;

    void Destroy() noexcept;
    static DeviceContext12* Current() noexcept;

    [[nodiscard]] ID3D12Device*              Device()   const noexcept { return m_device.Get(); }
    [[nodiscard]] ID3D12CommandQueue*        CmdQueue() const noexcept { return m_cmdQueue.Get(); }
    [[nodiscard]] IDXGISwapChain4*           SwapChain()const noexcept { return m_swapChain.Get(); }
    [[nodiscard]] IDXGIFactory4*             Factory()  const noexcept { return m_factory.Get(); }
    [[nodiscard]] ID3D12GraphicsCommandList* CmdList()  const noexcept { return m_cmdList.Get(); }
    [[nodiscard]] ID3D12RootSignature*       RootSignature() const noexcept { return m_rootSig.Get(); }
    [[nodiscard]] HWND                       TargetHwnd() const noexcept { return m_targetHwnd; }

    HRESULT PresentFrame(UINT syncInterval) noexcept;

    HRESULT FlushAndWait() noexcept;

    void WaitForGPU() noexcept;

    [[nodiscard]] UINT   FrameIndex() const noexcept { return m_frameIndex; }
    [[nodiscard]] UINT64 CurrentFence() const noexcept { return m_globalFenceValue; }

    [[nodiscard]] UINT64 CompletedFence() const noexcept
    {
        return m_fence ? m_fence->GetCompletedValue() : 0;
    }

    [[nodiscard]] UINT64 FrameCounter() const noexcept { return m_frameCounter; }

    [[nodiscard]] bool CommandStateDirty() const noexcept { return m_cmdStateDirty; }
    void ClearCommandStateDirty() noexcept { m_cmdStateDirty = false; }
    void MarkCommandStateDirty() noexcept { m_cmdStateDirty = true; }

    [[nodiscard]] UINT            BackBufferCount()  const noexcept { return k_dx12FrameCount; }
    [[nodiscard]] UINT            BackBufferIndex()  const noexcept { return m_backBufferIndex; }
    [[nodiscard]] ID3D12Resource* BackBuffer(UINT i) const noexcept
    {
        return i < k_dx12FrameCount ? m_backBuffers[i].Get() : nullptr;
    }
    [[nodiscard]] DXGI_FORMAT BackBufferFormat() const noexcept { return m_bbFormat; }
    [[nodiscard]] UINT        BackBufferWidth()  const noexcept { return m_bbWidth; }
    [[nodiscard]] UINT        BackBufferHeight() const noexcept { return m_bbHeight; }
    [[nodiscard]] D3DFORMAT   BackBufferD3D9Format() const noexcept { return m_bbD3D9Format; }

    [[nodiscard]] D3D12_RESOURCE_STATES BackBufferState(UINT i) const noexcept
    {
        return i < k_dx12FrameCount ? m_bbState[i] : D3D12_RESOURCE_STATE_COMMON;
    }
    void SetBackBufferState(UINT i, D3D12_RESOURCE_STATES s) noexcept
    {
        if (i < k_dx12FrameCount) m_bbState[i] = s;
    }

    [[nodiscard]] CpuDescriptorHeap&   RtvHeap()      noexcept { return m_rtvHeap; }
    [[nodiscard]] CpuDescriptorHeap&   DsvHeap()      noexcept { return m_dsvHeap; }

    [[nodiscard]] CpuDescriptorHeap&   SrvStaging()   noexcept { return m_srvStaging; }

    [[nodiscard]] CpuDescriptorHeap&   SamplerStaging() noexcept { return m_sampStaging; }
    [[nodiscard]] GpuDescriptorArena&  SrvArena()     noexcept { return m_srvArena; }
    [[nodiscard]] GpuDescriptorArena&  SamplerArena() noexcept { return m_sampArena; }

    void BindDescriptorHeaps() noexcept;

    [[nodiscard]] UploadRing& Upload() noexcept { return m_upload; }

    [[nodiscard]] UploadRing::Allocation AllocUpload(UINT64 bytes, UINT64 align) noexcept;

    // Video-memory budget and 32-bit process address space, written to the log
    // alongside any allocation failure. A resource creation that fails on a
    // player's machine and not on ours is nearly always one of these two, and
    // without them the report is just an HRESULT.
    // Resource-state journal.
    //
    // AppendTransition advances a resource's tracked state as soon as the
    // barrier is *recorded*, which is only correct if the list is then
    // submitted. Drop the list and the tracker believes in transitions the GPU
    // never performed, so every later barrier carries a wrong StateBefore --
    // which D3D12 rejects at ExecuteCommandLists, and that removes the device.
    // Recording what each barrier changed lets a dropped list be undone.
    void JournalState(Resource12* res, UINT sub,
                      D3D12_RESOURCE_STATES before) noexcept;
    void JournalBackBufferState(UINT index, D3D12_RESOURCE_STATES before) noexcept;
    void ForgetInStateJournal(const Resource12* res) noexcept;

    void ReportMemoryState(const char* why) noexcept;

    void ReportDredOutput(HRESULT reason) noexcept;

    void Retire(ID3D12Resource* res) noexcept;
    void Retire(ID3D12PipelineState* pso) noexcept;
    void Retire(ID3D12GraphicsCommandList* list) noexcept;

    void CollectGarbage() noexcept;

    HRESULT ResizeSwapChain(UINT w, UINT h, const D3DPRESENT_PARAMETERS& pp) noexcept;
    void    ApplyWindowMode(const D3DPRESENT_PARAMETERS& pp) noexcept;
    [[nodiscard]] DXGI_FORMAT ResolveBackBufferFormat(const D3DPRESENT_PARAMETERS& pp) const noexcept;

    void LockContext()   noexcept;
    void UnlockContext() noexcept;

    [[nodiscard]] bool IsDeviceLost() const noexcept { return m_deviceLost; }

    // Feed any failing HRESULT through here before logging it. Once the device
    // is gone every call fails the same way, and the caller that reports it
    // loudest is rarely the one that caused it -- one field log carried sixty
    // identical PSO-creation errors above the removal that produced them.
    // Returns true when the device is lost, meaning the caller should give up
    // quietly rather than log.
    bool NoteDeviceRemoved(HRESULT hr, const char* where) noexcept;
    void NotePresentResult(HRESULT hr) noexcept;

    void ReportDeviceRemoved(const char* where) noexcept;

    void DrainInfoQueue() noexcept;

    ~DeviceContext12();

private:
    DeviceContext12() = default;

    HRESULT InitD3D12Device(IDXGIAdapter1* adapter) noexcept;
    HRESULT InitCommandInfra() noexcept;
    HRESULT InitSwapChain(HWND hwnd, const D3DPRESENT_PARAMETERS& pp) noexcept;
    HRESULT InitDescriptorHeaps() noexcept;
    HRESULT InitUploadRing() noexcept;

    // Close the open list and submit it, refusing to submit one that failed to
    // close. Returns whether anything was actually executed.
    bool    CloseAndSubmit(const char* where) noexcept;
    HRESULT RecreateCommandList() noexcept;
    HRESULT InitRootSignature() noexcept;
    HRESULT AcquireBackBuffers() noexcept;
    void    ReleaseBackBuffers() noexcept;
    HRESULT OpenCommandList() noexcept;

    ComPtr<IDXGIFactory4>             m_factory;
    ComPtr<IDXGIAdapter1>             m_adapter;
    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12CommandQueue>        m_cmdQueue;
    ComPtr<IDXGISwapChain4>           m_swapChain;

    ComPtr<ID3D12CommandAllocator>    m_cmdAlloc[k_dx12FrameCount];
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;

    ComPtr<ID3D12Fence>               m_fence;
    UINT64                            m_frameFence[k_dx12FrameCount]{};
    UINT64                            m_globalFenceValue{ 0 };
    HANDLE                            m_fenceEvent{ nullptr };
    UINT                              m_frameIndex{ 0 };
    UINT64                            m_frameCounter{ 0 };

    ComPtr<ID3D12Resource>            m_backBuffers[k_dx12FrameCount];
    D3D12_RESOURCE_STATES             m_bbState[k_dx12FrameCount]{};
    UINT                              m_backBufferIndex{ 0 };
    DXGI_FORMAT                       m_bbFormat{ DXGI_FORMAT_B8G8R8A8_UNORM };
    D3DFORMAT                         m_bbD3D9Format{ D3DFMT_X8R8G8B8 };
    UINT                              m_bbWidth{ 0 };
    UINT                              m_bbHeight{ 0 };

    ComPtr<ID3D12RootSignature>       m_rootSig;

    CpuDescriptorHeap                 m_rtvHeap;
    CpuDescriptorHeap                 m_dsvHeap;
    CpuDescriptorHeap                 m_srvStaging;
    CpuDescriptorHeap                 m_sampStaging;
    GpuDescriptorArena                m_srvArena;
    GpuDescriptorArena                m_sampArena;

    UploadRing                        m_upload;

    struct Retired {
        ComPtr<IUnknown> object;
        UINT64           fence;
    };
    std::deque<Retired>               m_retired;

    struct StateJournalEntry {
        Resource12*           res;      // null means the back buffer named by index
        UINT                  sub;
        UINT                  bbIndex;
        D3D12_RESOURCE_STATES before;
    };
    std::vector<StateJournalEntry>    m_stateJournal;

    void RollbackStateJournal() noexcept;

    HWND    m_targetHwnd{ nullptr };
    bool    m_multiThreaded{ false };
    SRWLOCK m_ctxLock = SRWLOCK_INIT;
    bool    m_deviceLost{ false };
    bool    m_removedReported{ false };
    ComPtr<ID3D12InfoQueue> m_infoQueue;
    bool    m_dredEnabled{ false };
    bool    m_dredBreadcrumbs{ false };
    bool    m_cmdListOpen{ false };
    bool    m_cmdStateDirty{ true };
    bool    m_uploadCapReported{ false };
    bool    m_resetFailReported{ false };
    bool    m_cmdListBroken{ false };
    unsigned m_closeFailures{ 0 };

    static std::atomic<DeviceContext12*> s_current;
};

}
