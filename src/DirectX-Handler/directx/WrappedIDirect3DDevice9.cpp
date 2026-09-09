// Passthrough device: forwards every IDirect3DDevice9 call to the real runtime.
//
// Resources are returned unwrapped, deliberately. Nothing here inspects or
// modifies resource contents, so interposing on every texture and buffer would
// add an allocation and an indirection each for no benefit.

#include "WrappedIDirect3DDevice9.h"
#include "WrappedIDirect3D9.h"
#include "../Logger.h"

WrappedIDirect3DDevice9::WrappedIDirect3DDevice9(
    IDirect3DDevice9*      pReal,
    WrappedIDirect3D9*     pParent,
    HWND                   hFocusWindow,
    D3DPRESENT_PARAMETERS* pPP)
    : m_pReal(pReal)
    , m_pParent(pParent)
    , m_hFocus(hFocusWindow)
    , m_refCount(1)
{
    if (pPP) m_pp = *pPP;
    Logger::Log("WrappedIDirect3DDevice9: created");
}

WrappedIDirect3DDevice9::~WrappedIDirect3DDevice9()
{
    Logger::Log("WrappedIDirect3DDevice9: destroyed");
}

HRESULT __stdcall WrappedIDirect3DDevice9::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9) {
        *ppvObj = static_cast<IDirect3DDevice9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG __stdcall WrappedIDirect3DDevice9::AddRef()  { return ++m_refCount; }
ULONG __stdcall WrappedIDirect3DDevice9::Release()
{
    ULONG ref = --m_refCount;
    if (ref == 0) { m_pReal->Release(); delete this; }
    return ref;
}

HRESULT __stdcall WrappedIDirect3DDevice9::GetDirect3D(IDirect3D9** d)
{
    if (!d) return D3DERR_INVALIDCALL;
    *d = static_cast<IDirect3D9*>(m_pParent);
    m_pParent->AddRef();
    return S_OK;
}

HRESULT __stdcall WrappedIDirect3DDevice9::BeginScene()
{
    return m_pReal->BeginScene();
}

HRESULT __stdcall WrappedIDirect3DDevice9::EndScene()
{
    return m_pReal->EndScene();
}

HRESULT __stdcall WrappedIDirect3DDevice9::Present(
    const RECT*    pSrcRect,
    const RECT*    pDestRect,
    HWND           hDestWindowOverride,
    const RGNDATA* pDirtyRegion)
{
    return m_pReal->Present(pSrcRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT __stdcall WrappedIDirect3DDevice9::Reset(D3DPRESENT_PARAMETERS* pPP)
{
    HRESULT hr = m_pReal->Reset(pPP);
    if (SUCCEEDED(hr) && pPP) m_pp = *pPP;
    return hr;
}
