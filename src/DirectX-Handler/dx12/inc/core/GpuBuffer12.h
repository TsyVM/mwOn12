// Internal interface for reaching the D3D12 buffer behind a D3D9 proxy.
//
// The draw path needs the underlying buffer and its GPU address from an
// IDirect3DVertexBuffer9 the game handed back. Casting the D3D9 pointer to a
// concrete type would be wrong -- nothing guarantees it is one of ours -- so
// this is exposed through QueryInterface with a private GUID instead. An
// object that does not implement it simply fails the query, which is the
// answer we want rather than undefined behaviour.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

namespace mwon12 {

struct __declspec(uuid("B1C2D3E4-F5A6-7890-ABCD-EF1234567890"))
IGpuBuffer12 : IUnknown {
    virtual void      STDMETHODCALLTYPE GetCpuData(void** ppData, UINT* pSize) noexcept = 0;

    virtual D3DFORMAT STDMETHODCALLTYPE GetBufferFmt() noexcept = 0;
};

}
