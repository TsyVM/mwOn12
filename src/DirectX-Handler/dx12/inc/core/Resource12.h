// A D3D12 resource plus the state tracking D3D12 requires around it.
//
// Every D3D12 resource is in exactly one state -- render target, copy
// destination, shader resource and so on -- and using it in another state
// requires an explicit barrier. There is no validation of this at runtime
// outside the debug layer: a missing barrier gives undefined contents, and a
// wrong one can leave the GPU reading memory being written. Tracking the state
// on the resource itself, and emitting barriers by comparing against the state
// a use requires, is what keeps that manageable.
//
// Placement12 records where the resource lives and therefore how it is reached:
//
//   GpuOnly      default heap, no CPU access
//   GpuShadowed  default heap with a CPU-side copy, for resources the game
//                may lock
//   CpuOnly      system memory only
//   UploadHeap   CPU-writable and GPU-readable, for data written every frame
//
// The classification is derived from the D3D9 pool and usage flags, since
// those are what the game actually declares.

#pragma once

#ifndef MWON12_DX12_RESOURCE12_H
#define MWON12_DX12_RESOURCE12_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

#include <core/Dx12Common.h>

namespace mwon12 {

using Microsoft::WRL::ComPtr;

class DeviceContext12;

enum class Placement12 : uint8_t {
    GpuOnly,
    GpuShadowed,
    CpuOnly,
    UploadHeap,
};

[[nodiscard]] Placement12 ClassifyPlacement(D3DPOOL pool, DWORD usage,
                                            bool isBuffer) noexcept;

class Resource12 {
public:
    Resource12() noexcept = default;
    ~Resource12();

    Resource12(const Resource12&)            = delete;
    Resource12& operator=(const Resource12&) = delete;

    HRESULT Init(DeviceContext12* ctx,
                 const D3D12_RESOURCE_DESC& desc,
                 D3D12_HEAP_TYPE heapType,
                 D3D12_RESOURCE_STATES initState,
                 const D3D12_CLEAR_VALUE* optClear,
                 const wchar_t* debugName) noexcept;

    void Adopt(DeviceContext12* ctx, ID3D12Resource* res,
               D3D12_RESOURCE_STATES initState,
               int backBufferIndex = -1) noexcept;

    // Releases the underlying resource and forgets the context, exactly as the
    // destructor would. Idempotent.
    //
    // For a Resource12 owned by something that outlives the DeviceContext12 --
    // a member of D9Device12, say, whose destructor body deletes the context
    // before member destructors run -- this has to be called while the context
    // is still alive. ~Resource12 dereferences it.
    void Shutdown() noexcept;

    [[nodiscard]] bool Valid() const noexcept { return m_res != nullptr; }
    [[nodiscard]] ID3D12Resource* Native() const noexcept { return m_res.Get(); }
    [[nodiscard]] const D3D12_RESOURCE_DESC& Desc() const noexcept { return m_desc; }
    [[nodiscard]] UINT SubresourceCount() const noexcept { return m_subCount; }
    [[nodiscard]] UINT PlaneCount() const noexcept { return m_planeCount; }
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS Gpu() const noexcept
    {
        return m_res ? m_res->GetGPUVirtualAddress() : 0;
    }

    [[nodiscard]] D3D12_RESOURCE_STATES StateOf(UINT sub) const noexcept;

    [[nodiscard]] bool CanTransitionTo(D3D12_RESOURCE_STATES after) const noexcept;

    // Put one subresource's tracked state back, used when a command list is
    // dropped and the barriers recorded into it never executed.
    void RestoreState(UINT sub, D3D12_RESOURCE_STATES state) noexcept;

    void AppendTransition(std::vector<D3D12_RESOURCE_BARRIER>& out,
                          UINT sub, D3D12_RESOURCE_STATES after) noexcept;

    // Transition every plane of the mip/slice that `sub` names. A depth-stencil
    // format has two, and a view covers both.
    void AppendTransitionPlanes(std::vector<D3D12_RESOURCE_BARRIER>& out,
                                UINT sub, D3D12_RESOURCE_STATES after) noexcept;

    void Transition(ID3D12GraphicsCommandList* cmd,
                    UINT sub, D3D12_RESOURCE_STATES after) noexcept;

    void OverrideState(UINT sub, D3D12_RESOURCE_STATES state) noexcept;

private:

    void MirrorBackBufferState() noexcept;

public:

    HRESULT UploadSubresource(UINT sub,
                              const void* src, UINT srcRowPitch,
                              UINT srcSlicePitch) noexcept;

    HRESULT UploadSubresourceRegion(UINT sub,
                                    UINT dstX, UINT dstY, UINT dstZ,
                                    UINT width, UINT height, UINT depth,
                                    const void* src, UINT srcRowPitch,
                                    UINT srcSlicePitch) noexcept;

    HRESULT UploadBufferRange(UINT64 dstOffset, UINT64 bytes, const void* src) noexcept;

    HRESULT ReadbackSubresource(UINT sub, void* dst,
                                UINT dstRowPitch, UINT dstSlicePitch) noexcept;

    HRESULT CopySubresourceFrom(Resource12& src, UINT srcSub, UINT dstSub,
                                const D3D12_BOX* srcBox,
                                UINT dstX, UINT dstY, UINT dstZ) noexcept;

    void GetFootprint(UINT sub,
                      D3D12_PLACED_SUBRESOURCE_FOOTPRINT* outFootprint,
                      UINT* outNumRows, UINT64* outRowSizeBytes,
                      UINT64* outTotalBytes) const noexcept;

    [[nodiscard]] uint8_t* MappedPtr() const noexcept { return m_mapped; }

private:
    void ReleaseToRetirement() noexcept;

    DeviceContext12*       m_ctx{ nullptr };
    ComPtr<ID3D12Resource> m_res;
    D3D12_RESOURCE_DESC    m_desc{};
    D3D12_HEAP_TYPE        m_heapType{ D3D12_HEAP_TYPE_DEFAULT };
    UINT                   m_subCount{ 0 };
    UINT                   m_planeCount{ 1 };
    uint8_t*               m_mapped{ nullptr };

    D3D12_RESOURCE_STATES               m_uniformState{ D3D12_RESOURCE_STATE_COMMON };
    bool                                m_uniform{ true };
    int  m_mirrorBackBuffer{ -1 };
    std::vector<D3D12_RESOURCE_STATES>  m_states;

    void Split() noexcept;
};

}

#endif
