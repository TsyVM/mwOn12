// Passthrough wrapper around the real IDirect3D9 (Backend=1).
//
// When the user selects the original renderer, MWOn12 still sits in the middle:
// the game has already linked against this DLL, so every call has to arrive
// here and be forwarded to the system runtime. Almost every method below is a
// one-line delegation.
//
// The one thing this class does beyond forwarding is keep the wrapper hierarchy
// consistent. CreateDevice must hand back a WrappedIDirect3DDevice9 rather than
// the real device, because anything the game later obtains from that device
// must also come back through us. Returning a raw interface anywhere breaks the
// chain: the game holds a pointer we never see again, and calls on it bypass
// MWOn12 entirely.
//
// The reference count is our own, not the real object's. COM identity has to
// hold for the wrapper — the game may AddRef and Release this pointer
// independently of whatever the real runtime is doing with its own count.

#pragma once
#include <d3d9.h>
#include <atomic>

class WrappedIDirect3DDevice9;

class WrappedIDirect3D9 : public IDirect3D9 {
public:
    explicit WrappedIDirect3D9(IDirect3D9* pReal);
    ~WrappedIDirect3D9();

    HRESULT __stdcall QueryInterface(REFIID, void**) override;
    ULONG   __stdcall AddRef() override;
    ULONG   __stdcall Release() override;

    HRESULT __stdcall RegisterSoftwareDevice(void*) override;
    UINT    __stdcall GetAdapterCount() override;
    HRESULT __stdcall GetAdapterIdentifier(UINT,DWORD,D3DADAPTER_IDENTIFIER9*) override;
    UINT    __stdcall GetAdapterModeCount(UINT,D3DFORMAT) override;
    HRESULT __stdcall EnumAdapterModes(UINT,D3DFORMAT,UINT,D3DDISPLAYMODE*) override;
    HRESULT __stdcall GetAdapterDisplayMode(UINT,D3DDISPLAYMODE*) override;
    HRESULT __stdcall CheckDeviceType(UINT,D3DDEVTYPE,D3DFORMAT,D3DFORMAT,BOOL) override;
    HRESULT __stdcall CheckDeviceFormat(UINT,D3DDEVTYPE,D3DFORMAT,DWORD,D3DRESOURCETYPE,D3DFORMAT) override;
    HRESULT __stdcall CheckDeviceMultiSampleType(UINT,D3DDEVTYPE,D3DFORMAT,BOOL,D3DMULTISAMPLE_TYPE,DWORD*) override;
    HRESULT __stdcall CheckDepthStencilMatch(UINT,D3DDEVTYPE,D3DFORMAT,D3DFORMAT,D3DFORMAT) override;
    HRESULT __stdcall CheckDeviceFormatConversion(UINT,D3DDEVTYPE,D3DFORMAT,D3DFORMAT) override;
    HRESULT __stdcall GetDeviceCaps(UINT,D3DDEVTYPE,D3DCAPS9*) override;
    HMONITOR __stdcall GetAdapterMonitor(UINT) override;
    HRESULT __stdcall CreateDevice(UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**) override;

private:
    IDirect3D9*       m_pReal;
    std::atomic<ULONG> m_refCount;
};
