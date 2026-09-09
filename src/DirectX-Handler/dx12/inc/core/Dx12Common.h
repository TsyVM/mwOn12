// Alignment constants and small helpers shared across the D3D12 backend.
//
// D3D12 exposes alignment rules that D3D11 handled internally, and violating
// one is not reported -- it produces wrong data or a device removal, not an
// error at the call. The three that matter constantly are gathered here so
// they are named rather than written as literals:
//
//   constant buffer offsets and sizes must be 256-byte aligned;
//   a row of texture data being uploaded must start on a 256-byte boundary;
//   a texture's placement within an upload buffer must be 512-byte aligned.
//
// The row-pitch rule is the one that catches people out: the pitch of data in
// an upload buffer is almost never the pitch of the texture it came from, so
// an upload has to copy row by row rather than in one block.

#pragma once

#ifndef MWON12_DX12_COMMON_H
#define MWON12_DX12_COMMON_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <cstdint>

namespace mwon12 {
namespace dx12 {

template <typename T>
[[nodiscard]] constexpr T AlignUp(T v, T a) noexcept
{
    return (v + (a - 1)) & ~(a - 1);
}

inline constexpr UINT kCBAlign = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

inline constexpr UINT kTexRowAlign    = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
inline constexpr UINT kTexPlaceAlign  = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

[[nodiscard]] inline D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) noexcept
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type                 = type;
    hp.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask     = 1;
    hp.VisibleNodeMask      = 1;
    return hp;
}

[[nodiscard]] inline D3D12_RESOURCE_DESC BufferDesc(
    UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) noexcept
{
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment        = 0;
    rd.Width            = bytes;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc       = { 1, 0 };
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = flags;
    return rd;
}

[[nodiscard]] inline D3D12_RESOURCE_DESC Tex2DDesc(
    DXGI_FORMAT fmt, UINT width, UINT height, UINT16 mips, UINT16 arraySize,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
    UINT sampleCount = 1) noexcept
{
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Alignment        = 0;
    rd.Width            = width;
    rd.Height           = height;
    rd.DepthOrArraySize = arraySize;
    rd.MipLevels        = mips;
    rd.Format           = fmt;
    rd.SampleDesc       = { sampleCount, 0 };
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = flags;
    return rd;
}

[[nodiscard]] inline D3D12_RESOURCE_DESC Tex3DDesc(
    DXGI_FORMAT fmt, UINT width, UINT height, UINT16 depth, UINT16 mips,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) noexcept
{
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    rd.Alignment        = 0;
    rd.Width            = width;
    rd.Height           = height;
    rd.DepthOrArraySize = depth;
    rd.MipLevels        = mips;
    rd.Format           = fmt;
    rd.SampleDesc       = { 1, 0 };
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = flags;
    return rd;
}

[[nodiscard]] inline D3D12_RESOURCE_BARRIER TransitionBarrier(
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after,
    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) noexcept
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter  = after;
    b.Transition.Subresource = subresource;
    return b;
}

[[nodiscard]] inline D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* res) noexcept
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.Flags         = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.UAV.pResource = res;
    return b;
}

[[nodiscard]] constexpr UINT CalcSubresource(
    UINT mip, UINT arraySlice, UINT planeSlice,
    UINT mipLevels, UINT arraySize) noexcept
{
    return mip + arraySlice * mipLevels + planeSlice * mipLevels * arraySize;
}

[[nodiscard]] constexpr D3D12_CPU_DESCRIPTOR_HANDLE Offset(
    D3D12_CPU_DESCRIPTOR_HANDLE h, UINT index, UINT stride) noexcept
{
    return D3D12_CPU_DESCRIPTOR_HANDLE{ h.ptr + SIZE_T(index) * SIZE_T(stride) };
}

[[nodiscard]] constexpr D3D12_GPU_DESCRIPTOR_HANDLE Offset(
    D3D12_GPU_DESCRIPTOR_HANDLE h, UINT index, UINT stride) noexcept
{
    return D3D12_GPU_DESCRIPTOR_HANDLE{ h.ptr + UINT64(index) * UINT64(stride) };
}

[[nodiscard]] inline uint64_t HashBytes(const void* data, size_t bytes,
                                        uint64_t seed = 0xcbf29ce484222325ull) noexcept
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

[[nodiscard]] inline UINT FullMipCount(UINT w, UINT h, UINT d = 1) noexcept
{
    UINT levels = 1;
    while (w > 1 || h > 1 || d > 1) {
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
        d = d > 1 ? d >> 1 : 1;
        ++levels;
    }
    return levels;
}

}
}

#endif
