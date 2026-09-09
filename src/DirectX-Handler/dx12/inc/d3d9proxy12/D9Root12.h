// The D3D12 implementation of IDirect3D9 -- the factory the game starts from.
//
// Answers adapter, mode, format and capability queries, then creates the
// device. Capabilities come from core/CapsTable, which is the single place
// they are assembled.
//
// One capability is genuinely absent here: multisampling is not implemented,
// so multisample checks report unavailable and a request for a multisampled
// target falls back to single-sampled with a log line. Reporting that honestly
// is the point -- a game told multisampling exists and then given a
// single-sampled target has no way to notice.

#pragma once

#include <d3d9.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace mwon12 {

class D9Root12 final : public IDirect3D9Ex {
public:
    explicit D9Root12(UINT sdkVersion) noexcept;
    D9Root12(const D9Root12&)            = delete;
    D9Root12& operator=(const D9Root12&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void*) override;
    UINT    STDMETHODCALLTYPE GetAdapterCount() override;
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9*) override;
    UINT    STDMETHODCALLTYPE GetAdapterModeCount(UINT, D3DFORMAT) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT, D3DDISPLAYMODE*) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE, DWORD*) override;
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS9*) override;
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT) override;
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD,
                                           D3DPRESENT_PARAMETERS*, IDirect3DDevice9**) override;

    UINT    STDMETHODCALLTYPE GetAdapterModeCountEx(UINT, CONST D3DDISPLAYMODEFILTER*) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT, CONST D3DDISPLAYMODEFILTER*, UINT, D3DDISPLAYMODEEX*) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT, D3DDISPLAYMODEEX*, D3DDISPLAYROTATION*) override;
    HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT, D3DDEVTYPE, HWND, DWORD,
                                             D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*,
                                             IDirect3DDevice9Ex**) override;
    HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT, LUID*) override;

private:
    ~D9Root12();

    HRESULT CreateDeviceInternal(UINT Adapter, D3DDEVTYPE DeviceType,
                                 HWND hFocusWindow, DWORD BehaviorFlags,
                                 D3DPRESENT_PARAMETERS* pPP, bool isEx,
                                 IDirect3DDevice9** ppDevice) noexcept;

    HRESULT EnsureAdapters() noexcept;

    std::atomic<ULONG> m_refCount{ 1 };
    UINT               m_sdkVersion;
    ComPtr<IDXGIFactory4>              m_factory;
    std::vector<ComPtr<IDXGIAdapter1>> m_adapters;
};

}
