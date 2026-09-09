// Configuration and backend selection.
//
// Two questions are answered here, both exactly once per process:
//
//   Configured()  what MWOn12.ini asked for
//   Active()      what the machine can actually provide
//
// Everything is resolved lazily behind std::call_once rather than in DllMain,
// because probing for a D3D12 adapter loads and initialises graphics drivers,
// and doing that under the loader lock deadlocks. The first Direct3DCreate9
// call is the earliest safe point, and it is also the first point at which
// anyone needs the answer.
//
// Settings are read once at startup and never re-read. That is a deliberate
// simplification: a backend switch mid-process would mean tearing down a live
// device, and no setting here is worth that.
//
// This file lives under core/ with the rest of the API-independent code: the
// shader translator, the fixed-function generator, the format converter, the
// state tracker, the capability table and the logger. Only dx12/ knows about
// D3D12, and only directx/ knows about the passthrough.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#include <core/BackendSelect.h>
#include <core/Log.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace mwon12 {
namespace {

std::once_flag g_configOnce;
std::once_flag g_realD3D9Once;
std::once_flag g_effectiveOnce;

Backend g_backend = Backend::DX12;

Backend g_effectiveBackend = Backend::DX9;
bool    g_perfLog = false;
bool    g_halfPixelFix = true;
bool    g_legacyColorDefault = true;
bool    g_shaderDiskCache = true;
bool    g_shaderMods = false;
bool    g_shaderDump = false;
bool    g_lowLatency = true;
bool    g_debugLayer = false;
bool    g_gpuValidation = false;
bool    g_dredBreadcrumbs = false;
bool    g_textureTrace = false;
unsigned g_uploadRingMB = 0;
bool    g_sbRecordOnly = true;
bool    g_noBindCache = false;
bool    g_verboseLog = false;
bool    g_borderless = true;
bool    g_dumpFrame = false;
unsigned g_censusLimit = 6000u;
std::wstring g_iniPath;

HMODULE g_realD3D9 = nullptr;
using PFN_Direct3DCreate9   = IDirect3D9* (WINAPI*)(UINT);
using PFN_Direct3DCreate9Ex = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
PFN_Direct3DCreate9   g_realCreate9   = nullptr;
PFN_Direct3DCreate9Ex g_realCreate9Ex = nullptr;

// Deliberately OutputDebugString and not the DXLOG macros. Config is resolved
// before the render log has a device to attach to, and a debugger or DebugView
// is the only thing guaranteed to be listening this early.
void Log(const char* msg)
{
    OutputDebugStringA("[MWOn12] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

void LoadRealD3D9();

std::wstring ThisModuleDir()
{
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModuleDir), &hSelf);
    wchar_t path[MAX_PATH]{};
    if (!hSelf || GetModuleFileNameW(hSelf, path, MAX_PATH) == 0)
        return {};
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? std::wstring{} : dir.substr(0, slash + 1);
}

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
        return {};
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? std::wstring{} : dir.substr(0, slash + 1);
}

bool FileExists(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// MWOn12.ini normally sits beside the DLL, which is also beside the executable.
// Both directories are searched because that stops being true if someone
// installs the DLL into a subdirectory and loads it another way. MWDX.ini
// is the project's former name, still accepted so existing configs keep
// working. Falling back to a path that does not exist is fine and intended:
// GetPrivateProfileInt then returns every default, which is a valid config.
std::wstring FindIniPath()
{

    const wchar_t* const names[] = { L"MWOn12.ini", L"MWDX.ini" };
    const std::wstring dirs[] = { ThisModuleDir(), ExeDir() };
    for (const auto& n : names)
        for (const auto& d : dirs) {
            const std::wstring p = d + n;
            if (FileExists(p)) return p;
        }

    return ThisModuleDir() + names[0];
}

// Reads every setting once. Each one follows the same precedence: ini value
// first, then an environment variable that overrides it. The env overrides
// exist so a diagnostic run can be scripted without editing a user's config
// and forgetting to put it back.
//
// Settings that alter rendering announce themselves in the log when they are
// not at their default. A user who has changed something and forgotten is a
// recurring source of "MWOn12 is broken" reports, and the log should answer that
// before anyone opens a debugger.
void LoadConfig()
{
    int value = 0;

    wchar_t env[8]{};
    const auto envRead = [&env](const wchar_t* name) noexcept -> bool {
        const DWORD n = GetEnvironmentVariableW(name, env, 8);
        if (n == 0 || n >= 8) { env[0] = L'\0'; return false; }
        return true;
    };
    if (envRead(L"MWON12_BACKEND"))
        value = _wtoi(env);

    const std::wstring ini = FindIniPath();
    g_iniPath = ini;
    // Default 2, not 1: translating to D3D12 is what MWOn12 is for, and a user
    // who dropped the DLL in without an ini wants that, not a passthrough that
    // looks identical to not having installed anything.
    if (value == 0)
        value = static_cast<int>(
            GetPrivateProfileIntW(L"Renderer", L"Backend", 2, ini.c_str()));

    g_perfLog = GetPrivateProfileIntW(L"Renderer", L"PerfLog", 0, ini.c_str()) != 0;
    if (envRead(L"MWON12_PERFLOG") && _wtoi(env) != 0)
        g_perfLog = true;
    if (g_perfLog)
        Log("PerfLog enabled - frame counters every 120 frames");

    g_halfPixelFix =
        GetPrivateProfileIntW(L"Renderer", L"HalfPixelFix", 1, ini.c_str()) != 0;
    if (envRead(L"MWON12_HALFPIXEL"))
        g_halfPixelFix = _wtoi(env) != 0;
    if (!g_halfPixelFix)
        Log("HalfPixelFix disabled - expect half-texel sampling shift vs DX9");

    g_legacyColorDefault =
        GetPrivateProfileIntW(L"Renderer", L"LegacyColorDefault", 1, ini.c_str()) != 0;
    if (envRead(L"MWON12_LEGACY_COLOR_DEFAULT"))
        g_legacyColorDefault = _wtoi(env) != 0;
    if (!g_legacyColorDefault)
        Log("LegacyColorDefault disabled - unwritten oD0/oD1 stay (0,0,0,0); "
            "FEng HUD sprites may render black");

    g_shaderDiskCache =
        GetPrivateProfileIntW(L"Renderer", L"ShaderDiskCache", 1, ini.c_str()) != 0;
    if (envRead(L"MWON12_SHADERDISKCACHE"))
        g_shaderDiskCache = _wtoi(env) != 0;

    g_shaderMods =
        GetPrivateProfileIntW(L"Renderer", L"ShaderMods", 0, ini.c_str()) != 0;
    if (envRead(L"MWON12_SHADERMODS"))
        g_shaderMods = _wtoi(env) != 0;
    if (g_shaderMods)
        Log("ShaderMods enabled - .hlsl files in MWOn12\\Shaders replace the "
            "generated shaders");

    g_shaderDump =
        GetPrivateProfileIntW(L"Renderer", L"ShaderDump", 0, ini.c_str()) != 0;
    if (envRead(L"MWON12_SHADERDUMP"))
        g_shaderDump = _wtoi(env) != 0;
    if (g_shaderDump)
        Log("ShaderDump enabled - generated shaders are written to "
            "MWOn12\\Shaders\\Dumped");

    if (g_shaderMods || g_shaderDump)
        Log("shader mods active - the translated-shader disk cache is bypassed "
            "this run");

    g_lowLatency =
        GetPrivateProfileIntW(L"Renderer", L"LowLatency", 1, ini.c_str()) != 0;
    if (envRead(L"MWON12_LOWLATENCY"))
        g_lowLatency = _wtoi(env) != 0;
    if (!g_lowLatency)
        Log("LowLatency disabled - default DXGI frame queueing");

    if (GetPrivateProfileIntW(L"Renderer", L"HDR", 0, ini.c_str()) != 0 ||
        envRead(L"MWON12_HDR"))
        Log("HDR/enhancement output modes were removed - faithful SDR only");

    g_debugLayer =
        GetPrivateProfileIntW(L"Renderer", L"DebugLayer", 0, ini.c_str()) != 0;
    if (envRead(L"MWON12_DEBUGLAYER"))
        g_debugLayer = _wtoi(env) != 0;
    if (g_debugLayer)
        Log("DebugLayer requested - creating device with validation");

    g_gpuValidation =
        GetPrivateProfileIntW(L"Renderer", L"GpuValidation", 0, ini.c_str()) != 0;
    if (envRead(L"MWON12_GPUVALIDATION"))
        g_gpuValidation = _wtoi(env) != 0;
    if (g_gpuValidation)
        Log("GpuValidation requested - expect a very large slowdown");

    g_dredBreadcrumbs =
        GetPrivateProfileIntW(L"Renderer", L"DredBreadcrumbs", 0, ini.c_str()) != 0;
    if (envRead(L"MWON12_DREDBREADCRUMBS"))
        g_dredBreadcrumbs = _wtoi(env) != 0;
    if (g_dredBreadcrumbs)
        Log("DredBreadcrumbs requested - a device removal will name the faulting "
            "command (costs a marker write per command)");

    g_textureTrace =
        GetPrivateProfileIntW(L"Renderer", L"TextureTrace", 0, ini.c_str()) != 0;
    if (envRead(L"MWON12_TEXTURETRACE"))
        g_textureTrace = _wtoi(env) != 0;
    if (g_textureTrace)
        Log("TextureTrace requested - every texture write path is logged");

    g_noBindCache =
        GetPrivateProfileIntW(L"Renderer", L"NoBindCache", 0, ini.c_str()) != 0;
    if (envRead(L"MWON12_NOBINDCACHE"))
        g_noBindCache = _wtoi(env) != 0;
    if (g_noBindCache)
        Log("NoBindCache=1 - every texture/sampler/state bind is issued "
            "unconditionally (diagnostic; slower)");

    g_sbRecordOnly =
        GetPrivateProfileIntW(L"Renderer", L"StateBlockRecordOnly", 1, ini.c_str()) != 0;
    if (envRead(L"MWON12_SB_RECORDONLY"))
        g_sbRecordOnly = _wtoi(env) != 0;
    if (!g_sbRecordOnly)
        Log("StateBlockRecordOnly=0 - state changes during a recording also "
            "apply to the device (pre-2026-09-08 behaviour)");

    g_uploadRingMB = static_cast<unsigned>(
        GetPrivateProfileIntW(L"Renderer", L"UploadRingMB", 0, ini.c_str()));
    if (envRead(L"MWON12_UPLOADRINGMB")) {
        const int v = _wtoi(env);
        if (v > 0) g_uploadRingMB = static_cast<unsigned>(v);
    }
    if (g_uploadRingMB > 256u) g_uploadRingMB = 256u;

    g_verboseLog =
        GetPrivateProfileIntW(L"Renderer", L"VerboseLog", 0, ini.c_str()) != 0;
    if (envRead(L"MWON12_VERBOSELOG"))
        g_verboseLog = _wtoi(env) != 0;
    g_verboseLog = g_verboseLog || g_debugLayer;
    ::mwon12::log::SetVerbose(g_verboseLog);
    if (g_verboseLog)
        Log("VerboseLog enabled - full Info/Trace detail in MWOn12-render.log");

    g_dumpFrame =
        GetPrivateProfileIntW(L"Renderer", L"DumpFrame", 0, ini.c_str()) != 0;
    if (envRead(L"MWON12_DUMPFRAME"))
        g_dumpFrame = _wtoi(env) != 0;
    if (g_dumpFrame)
        Log("DumpFrame enabled - per-draw census in MWOn12-render.log");

    g_censusLimit =
        static_cast<unsigned>(GetPrivateProfileIntW(L"Renderer", L"CensusLimit", 6000, ini.c_str()));
    if (g_censusLimit == 0)
        g_censusLimit = 6000u;
    if (envRead(L"MWON12_CENSUS_LIMIT")) {
        const int v = _wtoi(env);
        if (v > 0)
            g_censusLimit = static_cast<unsigned>(v);
    }
    if (g_censusLimit != 6000u) {
        char buf[96];
        _snprintf_s(buf, _TRUNCATE, "CensusLimit = %u (default 6000)", g_censusLimit);
        Log(buf);
    }

    g_borderless =
        GetPrivateProfileIntW(L"Renderer", L"BorderlessFullscreen", 1, ini.c_str()) != 0;
    if (envRead(L"MWON12_BORDERLESS"))
        g_borderless = _wtoi(env) != 0;
    if (!g_borderless)
        Log("BorderlessFullscreen disabled - window is left untouched");

    switch (value) {
    case 1:
        g_backend = Backend::DX9;
        Log("Backend = 1 (DirectX 9 passthrough)");
        break;
    case 2:
        g_backend = Backend::DX12;
        Log("Backend = 2 (DirectX 12)");
        break;
    case 3:
        // MWDX numbered DirectX 12 as 3, with 2 meaning its DirectX 11 backend.
        // Accepted so an existing MWDX.ini keeps working, and answered rather
        // than silently reinterpreted so the log explains the renumbering.
        g_backend = Backend::DX12;
        Log("Backend = 3 is MWDX's number for DirectX 12; MWOn12 calls it 2. "
            "Using DirectX 12 - update the ini to Backend=2.");
        break;
    default:
        g_backend = Backend::DX12;
        Log("Unknown Backend value - using 2 (DirectX 12)");
        break;
    }
}

// The probe creates nothing: passing a null device pointer asks the runtime
// "would this succeed?" and discards the result. That keeps the probe cheap
// and, more importantly, avoids holding a second device alive alongside the
// one the backend is about to create.
bool ProbeD3D12() noexcept
{

    const HRESULT hr = D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr);
    return SUCCEEDED(hr);
}

// Turns the requested backend into one this adapter can actually run.
//
// There are only two outcomes, and they demote in both directions. A machine
// with no D3D12 feature-level-11_0 adapter cannot run the translator at all,
// so it falls back to the system runtime; a machine whose system d3d9.dll is
// missing or broken has nothing to pass through to, so it is promoted to the
// translator. The principle is that the game gets a renderer whatever happens,
// and the log says which one and why.
void ResolveEffective()
{
    std::call_once(g_configOnce, LoadConfig);

    if (g_backend == Backend::DX12) {
        if (ProbeD3D12()) { g_effectiveBackend = Backend::DX12; return; }
        Log("Backend=2 requested but no D3D12 feature-level-11_0 adapter is "
            "present - falling back to DirectX 9 passthrough");
    }

    std::call_once(g_realD3D9Once, LoadRealD3D9);
    if (g_realCreate9) { g_effectiveBackend = Backend::DX9; return; }

    if (ProbeD3D12()) {
        Log("system d3d9.dll unusable - promoting to DirectX 12 so the game "
            "still has a renderer");
        g_effectiveBackend = Backend::DX12;
        return;
    }
    Log("no usable rendering backend found (no system d3d9.dll, no D3D12)");
    g_effectiveBackend = Backend::DX9;
}

// Loads the genuine runtime for the passthrough backend. The system directory
// is resolved and prepended explicitly: this DLL is itself named d3d9.dll and
// sits in the executable's directory, which comes first in the default search
// order, so any relative load would find us and recurse.
void LoadRealD3D9()
{
    wchar_t sysDir[MAX_PATH]{};
    if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0) {
        Log("GetSystemDirectory failed - DX9 passthrough unavailable");
        return;
    }
    std::wstring path(sysDir);
    path += L"\\d3d9.dll";

    g_realD3D9 = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_realD3D9) {
        Log("LoadLibrary(system d3d9.dll) failed - DX9 passthrough unavailable");
        return;
    }
    g_realCreate9 = reinterpret_cast<PFN_Direct3DCreate9>(
        GetProcAddress(g_realD3D9, "Direct3DCreate9"));
    g_realCreate9Ex = reinterpret_cast<PFN_Direct3DCreate9Ex>(
        GetProcAddress(g_realD3D9, "Direct3DCreate9Ex"));
    if (!g_realCreate9)
        Log("system d3d9.dll is missing Direct3DCreate9 - passthrough unavailable");
}

}

// Every accessor drives the same call_once. Callers are spread across both
// backends and there is no defined initialisation order between them, so each
// one has to be able to be the first.
namespace BackendSelect {

Backend Active()
{
    std::call_once(g_effectiveOnce, ResolveEffective);
    return g_effectiveBackend;
}

Backend Configured()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_backend;
}

bool PerfLogEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_perfLog;
}

bool HalfPixelFixEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_halfPixelFix;
}

bool LegacyColorDefault()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_legacyColorDefault;
}

bool ShaderDiskCacheEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_shaderDiskCache;
}

bool ShaderModsEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_shaderMods;
}

bool ShaderDumpEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_shaderDump;
}

bool LowLatencyEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_lowLatency;
}

bool DebugLayerEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_debugLayer;
}

bool GpuValidationEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_gpuValidation;
}

bool DredBreadcrumbsEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_dredBreadcrumbs;
}

bool TextureTraceEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_textureTrace;
}

// Edge-detected F11, for arming a per-draw census on whatever is on screen.
//
// The census could only ever arm itself on a frame with at least a hundred
// depth-tested draws, which means in-race and nothing else. Every front-end
// screen was invisible to it, which is exactly where a HUD or menu fault has
// to be captured. A key the tester presses at the moment of interest captures
// the screen that actually matters.
bool CensusTriggerPressed()
{
    static bool s_wasDown = false;
    const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool edge = down && !s_wasDown;
    s_wasDown = down;
    return edge;
}

bool NoBindCache()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_noBindCache;
}

bool StateBlockRecordOnly()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_sbRecordOnly;
}

unsigned UploadRingMB()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_uploadRingMB;
}

bool VerboseLogEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_verboseLog;
}

bool BorderlessFullscreenEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_borderless;
}

bool DumpFrameEnabled()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_dumpFrame;
}

unsigned CensusLimit()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_censusLimit;
}

const wchar_t* IniPath()
{
    std::call_once(g_configOnce, LoadConfig);
    return g_iniPath.c_str();
}

FARPROC RealD3D9Export(const char* name)
{
    std::call_once(g_realD3D9Once, LoadRealD3D9);
    return g_realD3D9 ? GetProcAddress(g_realD3D9, name) : nullptr;
}

IDirect3D9* CreateReal9(UINT sdkVersion)
{
    std::call_once(g_realD3D9Once, LoadRealD3D9);
    return g_realCreate9 ? g_realCreate9(sdkVersion) : nullptr;
}

HRESULT CreateReal9Ex(UINT sdkVersion, IDirect3D9Ex** ppD3D)
{
    std::call_once(g_realD3D9Once, LoadRealD3D9);
    return g_realCreate9Ex ? g_realCreate9Ex(sdkVersion, ppD3D) : E_FAIL;
}

}
}
