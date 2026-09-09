// Which renderer is in use, and every setting read from MWOn12.ini.
//
// Configured() is what the user asked for; Active() is what the machine can
// actually provide, after probing for the requested API and falling back if it
// is unavailable. They differ often enough that anything reporting to the user
// should report both.
//
// Everything here is resolved once, lazily, on first use. Probing for a
// graphics adapter initialises drivers, which cannot be done safely while the
// DLL is still loading, so none of it can happen at load time.

#pragma once

#include <d3d9.h>

namespace mwon12 {

// MWOn12 translates to DirectX 12 and nothing else. Backend=1 is not a second
// renderer but an escape hatch: it hands the game to the system runtime
// untouched, so a machine with no D3D12 adapter still starts.
//
// Numbered 1 and 2 with no gap. MWDX used 1/2/3 with 2 meaning DirectX 11, and
// carrying that forward would have left a hole where a removed backend used to
// be -- which reads as a missing option rather than a deliberate choice. An ini
// written for MWDX that still says 3 is accepted and means DirectX 12.
enum class Backend : int {
    DX9  = 1,
    DX12 = 2,
};

constexpr const char* BackendName(Backend b) noexcept
{
    switch (b) {
    case Backend::DX9:  return "DirectX 9 (passthrough)";
    case Backend::DX12: return "DirectX 12";
    }
    return "unknown";
}

namespace BackendSelect {

bool PerfLogEnabled();

bool HalfPixelFixEnabled();

bool LegacyColorDefault();

bool ShaderDiskCacheEnabled();

bool ShaderModsEnabled();

bool ShaderDumpEnabled();

bool LowLatencyEnabled();

bool NoBindCache();
bool StateBlockRecordOnly();
unsigned UploadRingMB();
bool DebugLayerEnabled();

bool GpuValidationEnabled();

bool DredBreadcrumbsEnabled();

bool TextureTraceEnabled();

// True on the frame F11 is newly pressed. Arms a census capture on whatever
// screen is showing, which the draw-count heuristic cannot reach.
bool CensusTriggerPressed();

bool VerboseLogEnabled();

bool BorderlessFullscreenEnabled();

bool DumpFrameEnabled();

unsigned CensusLimit();

// The MWOn12.ini that was actually read, as a full path. Empty only if the
// search ran before any setting was requested, which cannot happen because
// every accessor resolves the config first. Plugins are handed this so a
// plugin's own section lands in the same file as the renderer's.
const wchar_t* IniPath();

FARPROC RealD3D9Export(const char* name);

Backend Active();

Backend Configured();

IDirect3D9* CreateReal9(UINT sdkVersion);
HRESULT     CreateReal9Ex(UINT sdkVersion, IDirect3D9Ex** ppD3D);

}
}
