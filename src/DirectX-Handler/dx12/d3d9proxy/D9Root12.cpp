// D3D12 adapter enumeration, capability reporting and device creation.
//
// Capabilities are reported from core/CapsTable rather than assembled here, so
// there is exactly one answer to what the renderer supports.
//
// Multisampling is the one capability genuinely absent here: it is reported as
// unavailable, and a request for a multisampled target logs and falls back to
// single-sampled. Reporting that honestly is deliberate -- a game told
// multisampling works and then handed a single-sampled target has no way to
// detect the substitution.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <cstring>
#include <algorithm>
#include <float.h>
#include <vector>

#include <d3d9proxy12/D9Root12.h>
#include <d3d9proxy12/D9Device12.h>
#include <core/DeviceContext12.h>
#include <core/CapsTable.h>
#include <core/FormatConverter.h>
#include <core/Log.h>

namespace mwon12 {

D9Root12::D9Root12(UINT sdkVersion) noexcept : m_sdkVersion(sdkVersion) {}
D9Root12::~D9Root12() = default;

HRESULT STDMETHODCALLTYPE D9Root12::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == __uuidof(IUnknown)   ||
        riid == __uuidof(IDirect3D9) ||
        riid == __uuidof(IDirect3D9Ex)) {
        *ppvObj = static_cast<IDirect3D9Ex*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9Root12::AddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE D9Root12::Release()
{
    ULONG prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT D9Root12::EnsureAdapters() noexcept
{
    if (m_factory) return S_OK;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(m_factory.GetAddressOf()));
    if (FAILED(hr)) return hr;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> a;
        hr = m_factory->EnumAdapters1(i, a.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return hr;
        m_adapters.push_back(std::move(a));
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D9Root12::RegisterSoftwareDevice(void*) { return D3DERR_INVALIDCALL; }

UINT STDMETHODCALLTYPE D9Root12::GetAdapterCount()
{
    if (FAILED(EnsureAdapters())) return 0;
    return static_cast<UINT>(m_adapters.size());
}

HRESULT STDMETHODCALLTYPE D9Root12::GetAdapterIdentifier(
    UINT Adapter, DWORD, D3DADAPTER_IDENTIFIER9* pId)
{
    if (!pId || FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(m_adapters[Adapter]->GetDesc1(&desc))) return D3DERR_INVALIDCALL;
    std::memset(pId, 0, sizeof(*pId));
    WideCharToMultiByte(CP_ACP, 0, desc.Description, -1,
                        pId->Driver, MAX_DEVICE_IDENTIFIER_STRING, nullptr, nullptr);
    strncpy_s(pId->Description, MAX_DEVICE_IDENTIFIER_STRING, pId->Driver, _TRUNCATE);
    pId->VendorId  = desc.VendorId;
    pId->DeviceId  = desc.DeviceId;
    pId->SubSysId  = desc.SubSysId;
    pId->Revision  = desc.Revision;
    pId->WHQLLevel = 1;
    return D3D_OK;
}

UINT STDMETHODCALLTYPE D9Root12::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
{
    if (FAILED(EnsureAdapters()) || Adapter >= m_adapters.size()) return 0;
    if (Format != D3DFMT_A8R8G8B8 && Format != D3DFMT_X8R8G8B8 &&
        Format != D3DFMT_R5G6B5   && Format != D3DFMT_A2R10G10B10)
        return 0;
    ComPtr<IDXGIOutput> output;
    if (FAILED(m_adapters[Adapter]->EnumOutputs(0, output.GetAddressOf()))) return 0;
    DXGI_FORMAT dxgiFmt = (Format == D3DFMT_R5G6B5)      ? DXGI_FORMAT_B5G6R5_UNORM :
                          (Format == D3DFMT_A2R10G10B10)  ? DXGI_FORMAT_R10G10B10A2_UNORM :
                                                            DXGI_FORMAT_B8G8R8A8_UNORM;
    UINT count = 0;
    output->GetDisplayModeList(dxgiFmt, 0, &count, nullptr);
    return count;
}

HRESULT STDMETHODCALLTYPE D9Root12::EnumAdapterModes(
    UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
{
    if (!pMode || FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;
    ComPtr<IDXGIOutput> output;
    if (FAILED(m_adapters[Adapter]->EnumOutputs(0, output.GetAddressOf())))
        return D3DERR_INVALIDCALL;
    DXGI_FORMAT dxgiFmt = (Format == D3DFMT_R5G6B5)      ? DXGI_FORMAT_B5G6R5_UNORM :
                          (Format == D3DFMT_A2R10G10B10)  ? DXGI_FORMAT_R10G10B10A2_UNORM :
                                                            DXGI_FORMAT_B8G8R8A8_UNORM;
    UINT count = 0;
    output->GetDisplayModeList(dxgiFmt, 0, &count, nullptr);
    if (Mode >= count) return D3DERR_INVALIDCALL;
    std::vector<DXGI_MODE_DESC> modes(count);
    output->GetDisplayModeList(dxgiFmt, 0, &count, modes.data());
    pMode->Width       = modes[Mode].Width;
    pMode->Height      = modes[Mode].Height;
    pMode->RefreshRate = modes[Mode].RefreshRate.Numerator /
                         std::max(1u, modes[Mode].RefreshRate.Denominator);
    pMode->Format      = Format;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Root12::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode)
{
    if (!pMode || FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;
    ComPtr<IDXGIOutput> output;
    if (FAILED(m_adapters[Adapter]->EnumOutputs(0, output.GetAddressOf())))
        return D3DERR_INVALIDCALL;
    DXGI_OUTPUT_DESC od{};
    output->GetDesc(&od);
    pMode->Width       = od.DesktopCoordinates.right  - od.DesktopCoordinates.left;
    pMode->Height      = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
    pMode->RefreshRate = 60;
    pMode->Format      = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Root12::CheckDeviceType(
    UINT, D3DDEVTYPE DevType, D3DFORMAT, D3DFORMAT, BOOL)
{
    return (DevType == D3DDEVTYPE_HAL) ? D3D_OK : D3DERR_NOTAVAILABLE;
}

namespace {

bool Dx12FormatSupportsUsage(ID3D12Device* dev, D3DFORMAT fmt,
                             DWORD usage, D3DRESOURCETYPE rtype) noexcept
{
    if (!dev) return false;

    const FormatMapping mapping = FormatConverter::ToDxgi(fmt);
    if (!mapping.IsValid()) return false;

    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = mapping.dxgiFormat;
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                        &fs, sizeof(fs))))
        return false;

    const auto s1 = fs.Support1;

    if (usage & D3DUSAGE_DEPTHSTENCIL)
        return (s1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) != 0;

    if (usage & D3DUSAGE_RENDERTARGET)
        if (!(s1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) return false;

    switch (rtype) {
    case D3DRTYPE_TEXTURE:
        if (!(s1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D)) return false;
        break;
    case D3DRTYPE_CUBETEXTURE:
        if (!(s1 & D3D12_FORMAT_SUPPORT1_TEXTURECUBE)) return false;
        break;
    case D3DRTYPE_VOLUMETEXTURE:
        if (!(s1 & D3D12_FORMAT_SUPPORT1_TEXTURE3D)) return false;
        break;
    case D3DRTYPE_SURFACE:

        if (!(s1 & (D3D12_FORMAT_SUPPORT1_TEXTURE2D |
                    D3D12_FORMAT_SUPPORT1_RENDER_TARGET |
                    D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)))
            return false;
        break;
    default:
        break;
    }

    if (!(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) &&
        (rtype == D3DRTYPE_TEXTURE || rtype == D3DRTYPE_CUBETEXTURE ||
         rtype == D3DRTYPE_VOLUMETEXTURE)) {
        if (!(s1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) return false;
    }

    if ((usage & (D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE)) &&
        !FormatConverter::HasSRGBVariant(mapping.dxgiFormat))
        return false;

    return true;
}

}

HRESULT STDMETHODCALLTYPE D9Root12::CheckDeviceFormat(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT, DWORD Usage,
    D3DRESOURCETYPE RType, D3DFORMAT CheckFmt)
{
    if (DevType != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;

    if (FormatConverter::IsFourCC(CheckFmt)) {

        if (FormatConverter::IsNullSurfaceFormat(CheckFmt))
            return (Usage & D3DUSAGE_RENDERTARGET) ? D3D_OK : D3DERR_NOTAVAILABLE;

        FormatConverter::DepthFormatViews dfv{};
        if (FormatConverter::GetDepthFormatViews(CheckFmt, dfv)) {
            ID3D12Device* dev = DeviceContext12::Current()
                              ? DeviceContext12::Current()->Device() : nullptr;
            if (!dev) return D3DERR_NOTAVAILABLE;
            D3D12_FEATURE_DATA_FORMAT_SUPPORT ds{ dfv.dsv }, sr{ dfv.srv };
            if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &ds, sizeof(ds))) ||
                FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &sr, sizeof(sr))))
                return D3DERR_NOTAVAILABLE;
            const bool ok = (ds.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) &&
                            (sr.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
            return ok ? D3D_OK : D3DERR_NOTAVAILABLE;
        }

    }

    if (DeviceContext12* ctx = DeviceContext12::Current()) {
        if (Dx12FormatSupportsUsage(ctx->Device(), CheckFmt, Usage, RType))
            return D3D_OK;
        return D3DERR_NOTAVAILABLE;
    }

    switch (CheckFmt) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5:
    case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_A8L8:
    case D3DFMT_V8U8: case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3:
    case D3DFMT_DXT4:  case D3DFMT_DXT5:
    case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_R32F:  case D3DFMT_A16B16G16R16F: case D3DFMT_A32B32G32R32F:
    case D3DFMT_INDEX16: case D3DFMT_INDEX32:
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE:
    case D3DFMT_A2B10G10R10: case D3DFMT_G16R16F: case D3DFMT_R16F:
    case D3DFMT_G32R32F: case D3DFMT_A8B8G8R8: case D3DFMT_A2R10G10B10:
        return D3D_OK;
    default:
        return D3DERR_NOTAVAILABLE;
    }
}

HRESULT STDMETHODCALLTYPE D9Root12::CheckDeviceMultiSampleType(
    UINT, D3DDEVTYPE DevType, D3DFORMAT, BOOL,
    D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
{
    if (DevType != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    if (MultiSampleType == D3DMULTISAMPLE_NONE) {
        if (pQualityLevels) *pQualityLevels = 1;
        return D3D_OK;
    }
    return D3DERR_NOTAVAILABLE;
}

HRESULT STDMETHODCALLTYPE D9Root12::CheckDepthStencilMatch(
    UINT, D3DDEVTYPE DevType, D3DFORMAT, D3DFORMAT, D3DFORMAT DepthFmt)
{
    if (DevType != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    switch (DepthFmt) {
    case D3DFMT_D24S8: case D3DFMT_D32F_LOCKABLE: case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE: case D3DFMT_D24X8:
        return D3D_OK;
    default:
        return D3DERR_NOTAVAILABLE;
    }
}

HRESULT STDMETHODCALLTYPE D9Root12::CheckDeviceFormatConversion(
    UINT, D3DDEVTYPE DevType, D3DFORMAT, D3DFORMAT)
{
    return (DevType == D3DDEVTYPE_HAL) ? D3D_OK : D3DERR_NOTAVAILABLE;
}

HRESULT STDMETHODCALLTYPE D9Root12::GetDeviceCaps(
    UINT Adapter, D3DDEVTYPE DevType, D3DCAPS9* pCaps)
{
    if (!pCaps || DevType != D3DDEVTYPE_HAL) return D3DERR_INVALIDCALL;
    if (FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;

    SynthesiseCaps(Adapter, pCaps);
    return D3D_OK;
}

HMONITOR STDMETHODCALLTYPE D9Root12::GetAdapterMonitor(UINT Adapter)
{
    if (FAILED(EnsureAdapters()) || Adapter >= m_adapters.size()) return nullptr;
    ComPtr<IDXGIOutput> output;
    if (FAILED(m_adapters[Adapter]->EnumOutputs(0, output.GetAddressOf()))) return nullptr;
    DXGI_OUTPUT_DESC od{};
    output->GetDesc(&od);
    return od.Monitor;
}

HRESULT D9Root12::CreateDeviceInternal(
    UINT Adapter, D3DDEVTYPE DevType,
    HWND hFocus, DWORD BehaviourFlags,
    D3DPRESENT_PARAMETERS* pPP, bool isEx,
    IDirect3DDevice9** ppDevice) noexcept
{
    if (!ppDevice || !pPP) return D3DERR_INVALIDCALL;
    if (DevType != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
    if (FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;

    bool mt   = (BehaviourFlags & D3DCREATE_MULTITHREADED) != 0;
    HWND hwnd = hFocus ? hFocus : pPP->hDeviceWindow;

    // Recorded because it changes what a later fault means. DeviceContext12 has
    // a LockContext() that nothing calls, so outside the upload ring -- which
    // locks itself -- this backend serialises nothing. A device removal on a
    // machine where this says "yes" is a different suspect list from one where
    // it says "no".
    DXLOG_INFO("[dx12] CreateDevice flags=0x%08X, D3DCREATE_MULTITHREADED=%s",
               (unsigned)BehaviourFlags, mt ? "yes" : "no");

    if (pPP->Windowed && (pPP->BackBufferWidth == 0 || pPP->BackBufferHeight == 0)) {
        RECT rc{};
        if (hwnd && GetClientRect(hwnd, &rc)) {
            if (pPP->BackBufferWidth  == 0) pPP->BackBufferWidth  = static_cast<UINT>(rc.right - rc.left);
            if (pPP->BackBufferHeight == 0) pPP->BackBufferHeight = static_cast<UINT>(rc.bottom - rc.top);
        }
        if (pPP->BackBufferWidth  == 0) pPP->BackBufferWidth  = 1;
        if (pPP->BackBufferHeight == 0) pPP->BackBufferHeight = 1;
    }
    if (pPP->BackBufferCount  == 0) pPP->BackBufferCount  = 1;
    if (pPP->BackBufferFormat == D3DFMT_UNKNOWN && pPP->Windowed)
        pPP->BackBufferFormat = D3DFMT_X8R8G8B8;

#if defined(_M_IX86)
    if (!(BehaviourFlags & D3DCREATE_FPU_PRESERVE)) {
        unsigned int prev = 0;
        _controlfp_s(&prev, _PC_24,  _MCW_PC);
        _controlfp_s(&prev, _RC_NEAR, _MCW_RC);
    }
#endif

    DXLOG_INFO("[dx12] CreateDevice%s: adapter=%u %ux%u bbFmt=%d dsFmt=%d "
               "windowed=%d autoDS=%d bbCount=%u presentIval=%u flags=0x%08X",
               isEx ? "Ex" : "", Adapter,
               pPP->BackBufferWidth, pPP->BackBufferHeight,
               (int)pPP->BackBufferFormat, (int)pPP->AutoDepthStencilFormat,
               pPP->Windowed ? 1 : 0, pPP->EnableAutoDepthStencil ? 1 : 0,
               pPP->BackBufferCount, pPP->PresentationInterval,
               (unsigned)BehaviourFlags);

    DeviceContext12* ctx{};
    HRESULT hr = DeviceContext12::Create(hwnd, *pPP, mt, Adapter, &ctx);
    if (FAILED(hr)) {
        DXLOG_FATAL_HR(hr, "[dx12] DeviceContext12::Create failed - no D3D12 "
                           "device for this adapter/window");
        return hr;
    }

    auto* dev = new (std::nothrow) D9Device12(
        std::unique_ptr<DeviceContext12>(ctx),
        this, BehaviourFlags, *pPP, isEx);
    if (!dev) { ctx->Destroy(); return E_OUTOFMEMORY; }

    hr = dev->FinishInit();
    if (FAILED(hr)) {
        DXLOG_FATAL_HR(hr, "[dx12] D9Device12::FinishInit failed - implicit "
                           "surfaces or default target binding could not be created");
        dev->Release();
        return hr;
    }

    DXLOG_INFO("[dx12] CreateDevice%s succeeded", isEx ? "Ex" : "");
    *ppDevice = dev;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Root12::CreateDevice(
    UINT A, D3DDEVTYPE T, HWND hFocus, DWORD Beh,
    D3DPRESENT_PARAMETERS* pPP, IDirect3DDevice9** ppDev)
{
    return CreateDeviceInternal(A, T, hFocus, Beh, pPP, false, ppDev);
}

HRESULT STDMETHODCALLTYPE D9Root12::CreateDeviceEx(
    UINT A, D3DDEVTYPE T, HWND hFocus, DWORD Beh,
    D3DPRESENT_PARAMETERS* pPP, D3DDISPLAYMODEEX*,
    IDirect3DDevice9Ex** ppDevEx)
{
    if (!ppDevEx || !pPP) return D3DERR_INVALIDCALL;
    IDirect3DDevice9* pBase{};
    HRESULT hr = CreateDeviceInternal(A, T, hFocus, Beh, pPP, true, &pBase);
    if (FAILED(hr)) return hr;
    hr = pBase->QueryInterface(__uuidof(IDirect3DDevice9Ex),
                               reinterpret_cast<void**>(ppDevEx));
    pBase->Release();
    return hr;
}

UINT STDMETHODCALLTYPE D9Root12::GetAdapterModeCountEx(UINT A, CONST D3DDISPLAYMODEFILTER*)
{
    return GetAdapterModeCount(A, D3DFMT_X8R8G8B8);
}
HRESULT STDMETHODCALLTYPE D9Root12::EnumAdapterModesEx(
    UINT A, CONST D3DDISPLAYMODEFILTER*, UINT Mode, D3DDISPLAYMODEEX* pMode)
{
    if (!pMode) return D3DERR_INVALIDCALL;
    D3DDISPLAYMODE basic{};
    HRESULT hr = EnumAdapterModes(A, D3DFMT_X8R8G8B8, Mode, &basic);
    if (SUCCEEDED(hr)) {
        pMode->Size             = sizeof(D3DDISPLAYMODEEX);
        pMode->Width            = basic.Width;
        pMode->Height           = basic.Height;
        pMode->RefreshRate      = basic.RefreshRate;
        pMode->Format           = basic.Format;
        pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE D9Root12::GetAdapterDisplayModeEx(
    UINT A, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRot)
{
    if (!pMode) return D3DERR_INVALIDCALL;
    D3DDISPLAYMODE basic{};
    HRESULT hr = GetAdapterDisplayMode(A, &basic);
    if (SUCCEEDED(hr)) {
        pMode->Size             = sizeof(D3DDISPLAYMODEEX);
        pMode->Width            = basic.Width;
        pMode->Height           = basic.Height;
        pMode->RefreshRate      = basic.RefreshRate;
        pMode->Format           = basic.Format;
        pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }
    if (pRot) *pRot = D3DDISPLAYROTATION_IDENTITY;
    return hr;
}
HRESULT STDMETHODCALLTYPE D9Root12::GetAdapterLUID(UINT Adapter, LUID* pLUID)
{
    if (!pLUID || FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;
    DXGI_ADAPTER_DESC1 desc{};
    HRESULT hr = m_adapters[Adapter]->GetDesc1(&desc);
    if (SUCCEEDED(hr)) *pLUID = desc.AdapterLuid;
    return SUCCEEDED(hr) ? D3D_OK : D3DERR_INVALIDCALL;
}

}
