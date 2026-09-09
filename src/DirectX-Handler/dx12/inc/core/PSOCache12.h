// Caches D3D12 pipeline state objects.
//
// D3D12 collapses almost the whole pipeline into one immutable object: both
// shaders, the input layout, the blend, rasteriser and depth-stencil state,
// the primitive topology type, and the render target formats. Change any one
// and a different object is required.
//
// Creating one is expensive enough that doing it during a frame is visible as
// a stall, and a D3D9 game changes the underlying state constantly. So the
// full description is hashed into a key and the resulting object is cached and
// reused. In practice a game uses a small number of distinct combinations, and
// the cache reaches steady state within a few seconds of play.
//
// Note that the render target formats are part of the key. A pipeline state
// object built for one target format cannot be used with another, which is
// easy to overlook until a render-to-texture pass fails validation.

#pragma once

#ifndef MWON12_DX12_PSOCACHE12_H
#define MWON12_DX12_PSOCACHE12_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <synchapi.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <core/Dx12Common.h>

namespace mwon12 {

using Microsoft::WRL::ComPtr;

class DeviceContext12;

inline constexpr UINT kMaxRenderTargets12 = 4;

struct PipelineKey12 {

    uint64_t vsHash{ 0 };
    uint64_t psHash{ 0 };
    uint64_t inputLayoutHash{ 0 };

    uint8_t blendEnable{ 0 };
    uint8_t srcBlend{ D3DBLEND_ONE };
    uint8_t destBlend{ D3DBLEND_ZERO };
    uint8_t blendOp{ D3DBLENDOP_ADD };
    uint8_t separateAlpha{ 0 };
    uint8_t srcBlendAlpha{ D3DBLEND_ONE };
    uint8_t destBlendAlpha{ D3DBLEND_ZERO };
    uint8_t blendOpAlpha{ D3DBLENDOP_ADD };
    uint8_t colorWrite[kMaxRenderTargets12]{ 0xF, 0xF, 0xF, 0xF };
    uint8_t independentBlend{ 0 };
    uint8_t alphaToCoverage{ 0 };

    uint8_t  fillMode{ D3DFILL_SOLID };
    uint8_t  cullMode{ D3DCULL_CCW };
    uint8_t  multisampleEnable{ 0 };
    uint8_t  antialiasedLine{ 0 };
    uint32_t depthBiasBits{ 0 };
    uint32_t slopeScaleBiasBits{ 0 };

    uint8_t depthEnable{ 1 };
    uint8_t depthWrite{ 1 };
    uint8_t depthFunc{ D3DCMP_LESSEQUAL };
    uint8_t stencilEnable{ 0 };
    uint8_t stencilReadMask{ 0xFF };
    uint8_t stencilWriteMask{ 0xFF };
    uint8_t stencilFail{ D3DSTENCILOP_KEEP };
    uint8_t stencilZFail{ D3DSTENCILOP_KEEP };
    uint8_t stencilPass{ D3DSTENCILOP_KEEP };
    uint8_t stencilFunc{ D3DCMP_ALWAYS };
    uint8_t twoSidedStencil{ 0 };
    uint8_t ccwFail{ D3DSTENCILOP_KEEP };
    uint8_t ccwZFail{ D3DSTENCILOP_KEEP };
    uint8_t ccwPass{ D3DSTENCILOP_KEEP };
    uint8_t ccwFunc{ D3DCMP_ALWAYS };

    uint8_t     numRenderTargets{ 1 };
    uint8_t     topologyType{ D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE };
    uint8_t     sampleCount{ 1 };
    uint8_t     _pad0{ 0 };
    DXGI_FORMAT rtvFormats[kMaxRenderTargets12]{ DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
                                                 DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
    DXGI_FORMAT dsvFormat{ DXGI_FORMAT_UNKNOWN };

    bool operator==(const PipelineKey12& o) const noexcept
    {
        return std::memcmp(this, &o, sizeof(PipelineKey12)) == 0;
    }
};

struct PipelineKey12Hash {
    size_t operator()(const PipelineKey12& k) const noexcept
    {
        return static_cast<size_t>(dx12::HashBytes(&k, sizeof(k)));
    }
};

struct SamplerKey12 {
    uint8_t  minFilter{ D3DTEXF_POINT };
    uint8_t  magFilter{ D3DTEXF_POINT };
    uint8_t  mipFilter{ D3DTEXF_NONE };
    uint8_t  addressU{ D3DTADDRESS_WRAP };
    uint8_t  addressV{ D3DTADDRESS_WRAP };
    uint8_t  addressW{ D3DTADDRESS_WRAP };
    uint8_t  maxAnisotropy{ 1 };
    uint8_t  maxMipLevel{ 0 };
    uint32_t mipLodBiasBits{ 0 };
    uint32_t borderColor{ 0 };

    bool operator==(const SamplerKey12& o) const noexcept
    {
        return std::memcmp(this, &o, sizeof(SamplerKey12)) == 0;
    }
};

struct SamplerKey12Hash {
    size_t operator()(const SamplerKey12& k) const noexcept
    {
        return static_cast<size_t>(dx12::HashBytes(&k, sizeof(k)));
    }
};

class PSOCache12 {
public:
    explicit PSOCache12(DeviceContext12* ctx) noexcept : m_ctx(ctx) {}

    ID3D12PipelineState* GetOrCreate(
        const PipelineKey12& key,
        const void* vsDxbc, size_t vsBytes,
        const void* psDxbc, size_t psBytes,
        const D3D12_INPUT_ELEMENT_DESC* inputElements, UINT inputCount) noexcept;

    D3D12_CPU_DESCRIPTOR_HANDLE GetOrCreateSampler(const SamplerKey12& key) noexcept;

    D3D12_CPU_DESCRIPTOR_HANDLE NullSrv(D3D12_SRV_DIMENSION dim) noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE DefaultSampler() noexcept;

    [[nodiscard]] size_t PipelineCount() const noexcept { return m_pipelines.size(); }
    [[nodiscard]] size_t SamplerCount()  const noexcept { return m_samplers.size(); }
    [[nodiscard]] uint64_t Misses() const noexcept { return m_misses; }

    void Clear() noexcept;

private:
    DeviceContext12* m_ctx{ nullptr };

    mutable SRWLOCK m_psoLock = SRWLOCK_INIT;
    std::unordered_map<PipelineKey12, ComPtr<ID3D12PipelineState>, PipelineKey12Hash> m_pipelines;

    mutable SRWLOCK m_sampLock = SRWLOCK_INIT;
    std::unordered_map<SamplerKey12, D3D12_CPU_DESCRIPTOR_HANDLE, SamplerKey12Hash> m_samplers;

    D3D12_CPU_DESCRIPTOR_HANDLE m_nullSrv2D{ SIZE_T(-1) };
    D3D12_CPU_DESCRIPTOR_HANDLE m_nullSrvCube{ SIZE_T(-1) };
    D3D12_CPU_DESCRIPTOR_HANDLE m_nullSrv3D{ SIZE_T(-1) };
    D3D12_CPU_DESCRIPTOR_HANDLE m_defaultSampler{ SIZE_T(-1) };

    uint64_t m_misses{ 0 };
};

[[nodiscard]] D3D12_BLEND            TranslateBlend12(D3DBLEND b) noexcept;
[[nodiscard]] D3D12_BLEND            SanitizeAlphaBlend12(D3D12_BLEND b) noexcept;
[[nodiscard]] D3D12_BLEND_OP         TranslateBlendOp12(D3DBLENDOP op) noexcept;
[[nodiscard]] D3D12_COMPARISON_FUNC  TranslateCmpFunc12(D3DCMPFUNC f) noexcept;
[[nodiscard]] D3D12_STENCIL_OP       TranslateStencilOp12(D3DSTENCILOP op) noexcept;
[[nodiscard]] D3D12_CULL_MODE        TranslateCullMode12(D3DCULL c) noexcept;
[[nodiscard]] D3D12_FILL_MODE        TranslateFillMode12(D3DFILLMODE f) noexcept;
[[nodiscard]] D3D12_TEXTURE_ADDRESS_MODE TranslateAddress12(D3DTEXTUREADDRESS a) noexcept;
[[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyTypeOf12(D3DPRIMITIVETYPE p) noexcept;
[[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY      TopologyOf12(D3DPRIMITIVETYPE p) noexcept;

[[nodiscard]] UINT VertexCountFor12(D3DPRIMITIVETYPE type, UINT primCount) noexcept;

}

#endif
