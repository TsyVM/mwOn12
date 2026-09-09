// Copies and rescales images on the GPU for the D3D12 backend.
//
// D3D12 offers a raw buffer-to-buffer or texture-to-texture copy, which is
// only usable when the two resources match exactly in size and format. D3D9
// requires more than that: StretchRect rescales and filters between formats,
// and generating a mip chain means repeatedly halving an image. Neither can be
// expressed as a copy.
//
// Both are therefore done by drawing: a full-screen triangle samples the
// source and writes the destination, with a pipeline state per destination
// format. That is also how the mip chain is produced, one level at a time, each
// level rendering from the one above it.

#pragma once

#ifndef MWON12_DX12_BLITTER12_H
#define MWON12_DX12_BLITTER12_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <core/Resource12.h>

namespace mwon12 {

using Microsoft::WRL::ComPtr;

class DeviceContext12;
class PSOCache12;

class Blitter12 {
public:
    Blitter12(DeviceContext12* ctx, PSOCache12* pso) noexcept
        : m_ctx(ctx), m_pso(pso) {}

    HRESULT Blit(D3D12_CPU_DESCRIPTOR_HANDLE dstRtv, DXGI_FORMAT dstFormat,
                 UINT dstWidth, UINT dstHeight, const RECT& dstRect,
                 D3D12_CPU_DESCRIPTOR_HANDLE srcSrv,
                 UINT srcWidth, UINT srcHeight, const RECT& srcRect,
                 bool linearFilter) noexcept;

    HRESULT GenerateMipChain(Resource12& resource, DXGI_FORMAT rtvFormat,
                             UINT width, UINT height, UINT mipLevels,
                             UINT arraySlice, UINT arraySize) noexcept;

    void Clear() noexcept { m_pipelines.clear(); }

private:
    HRESULT EnsureShaders() noexcept;
    ID3D12PipelineState* PipelineFor(DXGI_FORMAT dstFormat) noexcept;

    DeviceContext12* m_ctx{ nullptr };
    PSOCache12*      m_pso{ nullptr };

    std::vector<uint8_t> m_vsDxbc;
    std::vector<uint8_t> m_psDxbc;
    std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> m_pipelines;

    D3D12_CPU_DESCRIPTOR_HANDLE m_pointSampler{ SIZE_T(-1) };
    D3D12_CPU_DESCRIPTOR_HANDLE m_linearSampler{ SIZE_T(-1) };
};

}

#endif
