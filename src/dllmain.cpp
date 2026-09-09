// MWOn12 entry point.
//
// This DLL is dropped next to speed.exe under the name d3d9.dll. Windows
// resolves an executable's imports from its own directory before the system
// directory, so the game links against us instead of the real runtime without
// any patching of the executable. Everything the game asks for arrives through
// the twelve exports below; MWOn12.def pins their names so the
// import table the game was built against still resolves.
//
// The only decision made here is which backend the game gets. Direct3DCreate9
// hands back one of two objects that both implement IDirect3D9: our D3D12
// root, or a thin wrapper around the system runtime. From the game's point of
// view they are indistinguishable.
//
// The second of those is not a renderer this project maintains -- it is the
// escape hatch for a machine with no D3D12 adapter, so the game still starts.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <new>
#include <string>

#include "Logger.h"
#include "directx/WrappedIDirect3D9.h"
#include <core/BackendSelect.h>
#include <d3d9proxy12/D9Root12.h>
#include <sdk/PluginHost.h>
#include <sdk/AsiLoader.h>

// __cdecl, matching MWON12_CALL in <mwon12/mwon12.h>. Spelled out rather than
// including that header: this file compiles whether or not the SDK is present,
// and the three exports below must exist either way so an ASI never finds a
// d3d9.dll with some of them missing.
#if defined(_MSC_VER)
#  define MWON12_CALL __cdecl
#else
#  define MWON12_CALL
#endif

// The system d3d9.dll, loaded by full path in DllMain. Only the passthrough
// backend uses these; the translating backends never touch the real runtime.
static HMODULE s_realD3D9 = nullptr;
using PFN_Direct3DCreate9   = IDirect3D9* (WINAPI*)(UINT);
using PFN_Direct3DCreate9Ex = HRESULT     (WINAPI*)(UINT, IDirect3D9Ex**);
static PFN_Direct3DCreate9   s_realCreate9   = nullptr;
static PFN_Direct3DCreate9Ex s_realCreate9Ex = nullptr;

extern "C" {

// PIX instrumentation hooks. The game calls these unconditionally, so they
// have to exist and be cheap; they carry no meaning outside a PIX capture.
// D3DPERF_GetStatus returning 0 tells the caller no profiler is attached,
// which is what stops the game emitting event markers at all.
int   WINAPI D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR)     { return 0; }
int   WINAPI D3DPERF_EndEvent()                        { return 0; }
DWORD WINAPI D3DPERF_GetStatus()                       { return 0; }
BOOL  WINAPI D3DPERF_QueryRepeatFrame()                { return FALSE; }
void  WINAPI D3DPERF_SetMarker(D3DCOLOR, LPCWSTR)     {}
void  WINAPI D3DPERF_SetOptions(DWORD)                 {}
void  WINAPI D3DPERF_SetRegion(D3DCOLOR, LPCWSTR)     {}
void  WINAPI DebugSetMute()                            {}

// Emitted once, on whichever create call comes first. Configured() is what the
// ini asked for; Active() is what survived probing the adapter. When they
// differ the user needs to know their setting was overruled and why, because
// the symptom otherwise is "my setting does nothing".
static void LogBackendChoice()
{
    using namespace mwon12;
    static bool s_logged = false;
    if (s_logged) return;
    s_logged = true;

    const auto want = BackendSelect::Configured();
    const auto got  = BackendSelect::Active();
    if (want == got)
        Logger::Log("Renderer backend: %s", BackendName(got));
    else
        Logger::Log("Renderer backend: %s requested, %s in use "
                    "(requested API unavailable on this adapter)",
                    BackendName(want), BackendName(got));
}

IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVer)
{
    using namespace mwon12;
    LogBackendChoice();
    // Before the renderer is built, so an ASI's hooks are in place before the
    // game has drawn anything -- and outside DllMain, where LoadLibrary would
    // be running under the loader lock. Idempotent; both create paths call it
    // because a game may use either.
    asi::LoadAll();
    if (BackendSelect::Active() == Backend::DX12) {
        // nothrow, like the Ex path below. This is a C entry point the game
        // calls through its import table; a std::bad_alloc unwinding out of it
        // crosses a frame that was never compiled to expect one, and the game
        // gets a corrupt stack rather than the null it knows how to handle.
        auto* root = new (std::nothrow) D9Root12(sdkVer);
        if (!root)
            Logger::Log("ERROR: Direct3DCreate9 - out of memory creating the "
                        "D3D12 root object");
        return root;
    }

    // Passthrough. BackendSelect only resolves to DX9 when it has already
    // confirmed the real entry point exists, so this branch failing means the
    // system runtime disappeared between probe and call.
    if (!s_realCreate9) {
        Logger::Log("ERROR: Direct3DCreate9 - DX9 passthrough selected but the "
                    "system d3d9.dll has no Direct3DCreate9");
        return nullptr;
    }
    IDirect3D9* pReal = s_realCreate9(sdkVer);
    if (!pReal) {
        Logger::Log("ERROR: system Direct3DCreate9 returned null");
        return nullptr;
    }
    return new WrappedIDirect3D9(pReal);
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVer, IDirect3D9Ex** ppD3D)
{
    using namespace mwon12;
    if (!ppD3D) return D3DERR_INVALIDCALL;
    *ppD3D = nullptr;
    LogBackendChoice();
    asi::LoadAll();

    if (BackendSelect::Active() == Backend::DX12) {
        // Construct at the IDirect3D9 refcount of 1, then let QueryInterface
        // take the reference the caller keeps. The Release below drops our
        // construction reference, so a QI failure destroys the object rather
        // than leaking it.
        auto* root = new (std::nothrow) D9Root12(sdkVer);
        if (!root) return E_OUTOFMEMORY;
        const HRESULT hr = root->QueryInterface(__uuidof(IDirect3D9Ex),
                                                reinterpret_cast<void**>(ppD3D));
        root->Release();
        return hr;
    }
    if (!s_realCreate9Ex) return E_NOTIMPL;
    return s_realCreate9Ex(sdkVer, ppD3D);
}

// The name is the only thing MWOn12 shares with D3D9On12.
//
// D3D9On12 is Microsoft's own D3D9-over-D3D12 mapping layer, and these two
// entry points are how a caller hands it an existing D3D12 device to render
// through. MWOn12 exports them so that anything resolving them by name still
// links, and then ignores the supplied device entirely: the call is routed
// into our own D3D12 factory.
//
// Adopting a caller's device would mean the game's D3D9 calls run through
// somebody else's translation layer, which is precisely what this project
// exists to replace. Our code owns the D3D9 state interpretation, the shader
// translation, the descriptor allocation and the synchronisation, or the
// project has no reason to exist.
IDirect3D9* WINAPI Direct3DCreate9On12(UINT sdkVer, void*, UINT)
{
    return Direct3DCreate9(sdkVer);
}

HRESULT WINAPI Direct3DCreate9On12Ex(UINT sdkVer, void*, UINT, IDirect3D9Ex** ppD3D)
{
    return Direct3DCreate9Ex(sdkVer, ppD3D);
}

// ── Runtime registration, for ASI mods ──────────────────────────────────────
//
// Nothing to do with Direct3D 9; these are here because d3d9.dll is the module
// an ASI can be certain is loaded, so it is the one worth hanging an entry
// point off. An ASI resolves them with GetProcAddress and joins the plugin
// system, which is what gives it the D3D12 device once a frame.
//
// __cdecl to match MWON12_CALL in the ABI header. Named in MWOn12.def so the
// names are undecorated on x86, where __cdecl would otherwise export them as
// _MWOn12_RegisterPlugin.

int MWON12_CALL MWOn12_RegisterPlugin(const void* api)
{
    return mwon12::sdk::PluginHost::RegisterAtRuntime(api);
}

void MWON12_CALL MWOn12_UnregisterPlugin(void* user)
{
    mwon12::sdk::PluginHost::UnregisterAtRuntime(user);
}

const void* MWON12_CALL MWOn12_GetHost(void)
{
    return mwon12::sdk::PluginHost::HostTable();
}

}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Nothing here is per-thread, and the game spawns threads freely.
        DisableThreadLibraryCalls(hInst);

        // Logs go beside the executable, not beside this DLL: the two are the
        // same directory for a normal install, but the executable is the one
        // the user will think to look in.
        char exePath[MAX_PATH]{};
        // Zeroed and length-checked rather than trusted: on truncation
        // GetModuleFileName is not documented to terminate the buffer, and the
        // std::string constructed from it would then run off the end of the
        // stack frame looking for a null.
        const DWORD exeLen = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (exeLen == 0 || exeLen >= MAX_PATH) exePath[0] = '\0';
        std::string gameDir = exePath;
        auto slash = gameDir.rfind('\\');
        if (slash != std::string::npos) gameDir.resize(slash + 1);
        else                            gameDir.clear();

        Logger::Init((gameDir + "MWOn12.log").c_str());
        Logger::Log("MWOn12 loading - game dir: %s", gameDir.c_str());

        // Absolute path into system32 is mandatory. A bare LoadLibrary("d3d9")
        // searches the executable's directory first and would find this DLL,
        // which loads us into ourselves.
        char sys32[MAX_PATH];
        GetSystemDirectoryA(sys32, MAX_PATH);
        s_realD3D9 = LoadLibraryA((std::string(sys32) + "\\d3d9.dll").c_str());
        if (s_realD3D9) {
            s_realCreate9   = reinterpret_cast<PFN_Direct3DCreate9>  (GetProcAddress(s_realD3D9, "Direct3DCreate9"));
            s_realCreate9Ex = reinterpret_cast<PFN_Direct3DCreate9Ex>(GetProcAddress(s_realD3D9, "Direct3DCreate9Ex"));
        }

        // Not fatal: the D3D12 backend needs none of this. Only the Backend=1
        // fallback is lost, and BackendSelect will promote away from it.
        if (!s_realCreate9)
            Logger::Log("WARN: system Direct3DCreate9 not found - DX9 passthrough disabled");
        else
            Logger::Log("Real d3d9.dll loaded OK");

        // Plugins are only recorded here, not loaded: LoadLibrary under the
        // loader lock deadlocks, and a plugin is a DLL. The scan happens on
        // the first device creation, which is the same reason BackendSelect
        // defers its adapter probe.
        mwon12::sdk::PluginHost::NoteGameDirectory(gameDir.c_str());
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        mwon12::sdk::PluginHost::Shutdown();
        Logger::Shutdown();

        // `reserved` is non-null when the process is terminating rather than
        // when this DLL is being unloaded on its own. In that case every other
        // module is going away too and the loader is already walking them, so
        // FreeLibrary here is at best redundant and at worst a re-entrant load
        // under a lock the loader holds -- the documented way to deadlock or
        // fault on exit. The mapping is reclaimed with the address space.
        //
        // A d3d9.dll proxy is effectively never unloaded any other way, so the
        // branch below is the unusual one; it exists so the DLL is still
        // correct if something does LoadLibrary/FreeLibrary it.
        if (reserved == nullptr && s_realD3D9) {
            FreeLibrary(s_realD3D9);
            s_realD3D9      = nullptr;
            s_realCreate9   = nullptr;
            s_realCreate9Ex = nullptr;
        }
    }
    return TRUE;
}
