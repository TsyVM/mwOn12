// Passthrough factory: forwards every IDirect3D9 call to the real runtime.
//
// The only method that does more than delegate is CreateDevice, which has to
// wrap the device it gets back. Everything the game later obtains from that
// device must keep arriving through MWOn12; handing back the runtime's own
// device pointer would break the chain permanently.

#include "WrappedIDirect3D9.h"
#include "WrappedIDirect3DDevice9.h"
#include "../Logger.h"

WrappedIDirect3D9::WrappedIDirect3D9(IDirect3D9* pReal)
    : m_pReal(pReal), m_refCount(1)
{
    Logger::Log("WrappedIDirect3D9: created");
}

WrappedIDirect3D9::~WrappedIDirect3D9()
{
    Logger::Log("WrappedIDirect3D9: destroyed");
}

HRESULT __stdcall WrappedIDirect3D9::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
        *ppvObj = static_cast<IDirect3D9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG __stdcall WrappedIDirect3D9::AddRef()  { return ++m_refCount; }
ULONG __stdcall WrappedIDirect3D9::Release()
{
    ULONG ref = --m_refCount;
    if (ref == 0) { m_pReal->Release(); delete this; }
    return ref;
}

HRESULT __stdcall WrappedIDirect3D9::RegisterSoftwareDevice(void* p)   { return m_pReal->RegisterSoftwareDevice(p); }
UINT    __stdcall WrappedIDirect3D9::GetAdapterCount()                  { return m_pReal->GetAdapterCount(); }
HRESULT __stdcall WrappedIDirect3D9::GetAdapterIdentifier(UINT a,DWORD f,D3DADAPTER_IDENTIFIER9* i) { return m_pReal->GetAdapterIdentifier(a,f,i); }
UINT    __stdcall WrappedIDirect3D9::GetAdapterModeCount(UINT a,D3DFORMAT f) { return m_pReal->GetAdapterModeCount(a,f); }
HRESULT __stdcall WrappedIDirect3D9::EnumAdapterModes(UINT a,D3DFORMAT f,UINT m,D3DDISPLAYMODE* d) { return m_pReal->EnumAdapterModes(a,f,m,d); }
HRESULT __stdcall WrappedIDirect3D9::GetAdapterDisplayMode(UINT a,D3DDISPLAYMODE* d) { return m_pReal->GetAdapterDisplayMode(a,d); }
HRESULT __stdcall WrappedIDirect3D9::CheckDeviceType(UINT a,D3DDEVTYPE t,D3DFORMAT af,D3DFORMAT bf,BOOL w) { return m_pReal->CheckDeviceType(a,t,af,bf,w); }
HRESULT __stdcall WrappedIDirect3D9::CheckDeviceFormat(UINT a,D3DDEVTYPE t,D3DFORMAT af,DWORD u,D3DRESOURCETYPE r,D3DFORMAT cf) { return m_pReal->CheckDeviceFormat(a,t,af,u,r,cf); }
HRESULT __stdcall WrappedIDirect3D9::CheckDeviceMultiSampleType(UINT a,D3DDEVTYPE t,D3DFORMAT f,BOOL w,D3DMULTISAMPLE_TYPE ms,DWORD* q) { return m_pReal->CheckDeviceMultiSampleType(a,t,f,w,ms,q); }
HRESULT __stdcall WrappedIDirect3D9::CheckDepthStencilMatch(UINT a,D3DDEVTYPE t,D3DFORMAT af,D3DFORMAT rf,D3DFORMAT df) { return m_pReal->CheckDepthStencilMatch(a,t,af,rf,df); }
HRESULT __stdcall WrappedIDirect3D9::CheckDeviceFormatConversion(UINT a,D3DDEVTYPE t,D3DFORMAT sf,D3DFORMAT df) { return m_pReal->CheckDeviceFormatConversion(a,t,sf,df); }
HRESULT __stdcall WrappedIDirect3D9::GetDeviceCaps(UINT a,D3DDEVTYPE t,D3DCAPS9* c) { return m_pReal->GetDeviceCaps(a,t,c); }
HMONITOR __stdcall WrappedIDirect3D9::GetAdapterMonitor(UINT a) { return m_pReal->GetAdapterMonitor(a); }

HRESULT __stdcall WrappedIDirect3D9::CreateDevice(
    UINT                  Adapter,
    D3DDEVTYPE            DeviceType,
    HWND                  hFocusWindow,
    DWORD                 BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9**    ppReturnedDeviceInterface)
{
    IDirect3DDevice9* pReal = nullptr;
    HRESULT hr = m_pReal->CreateDevice(
        Adapter, DeviceType, hFocusWindow,
        BehaviorFlags, pPresentationParameters, &pReal);

    if (SUCCEEDED(hr) && pReal) {
        auto* wrapped = new WrappedIDirect3DDevice9(pReal, this, hFocusWindow, pPresentationParameters);
        *ppReturnedDeviceInterface = wrapped;
        Logger::Log("WrappedIDirect3D9::CreateDevice: wrapped device created (%ux%u)",
                    pPresentationParameters ? pPresentationParameters->BackBufferWidth  : 0,
                    pPresentationParameters ? pPresentationParameters->BackBufferHeight : 0);
    } else {
        Logger::Log("WrappedIDirect3D9::CreateDevice: real CreateDevice failed hr=0x%08X", hr);
        *ppReturnedDeviceInterface = nullptr;
    }
    return hr;
}
