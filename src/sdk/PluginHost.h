// Loads and drives MWOn12 plugins.
//
// The public contract lives in the MWOn12SDK, in <mwon12/mwon12.h>. This is
// only the renderer's half: finding the DLLs, calling their entry points, and
// forwarding the four events the ABI defines.
//
// When the SDK header is not present at build time the whole feature compiles
// out -- every function below becomes an empty inline -- so the renderer never
// depends on the SDK being checked out beside it. CMake reports which of the
// two happened.
//
// Nothing here is called under the loader lock. NoteGameDirectory only records
// a string; the scan that actually calls LoadLibrary is deferred to the first
// device creation, for the same reason BackendSelect defers its adapter probe:
// loading a DLL from inside DllMain deadlocks.

#pragma once

#ifndef MWON12_PLUGIN_HOST_H
#define MWON12_PLUGIN_HOST_H

#include <windows.h>

struct ID3D12Resource;

namespace mwon12 {

class D9Device12;

namespace sdk {
namespace PluginHost {

// Recorded in DllMain; the directory speed.exe lives in, with a trailing
// backslash. Plugins are looked for in <that>MWOn12\Plugins\.
void NoteGameDirectory(const char* dir) noexcept;

// Scans and loads, once. Safe to call repeatedly and from any point after
// DllMain has returned.
void EnsureLoaded() noexcept;

// True when at least one loaded plugin wants OnPresent. The Present path uses
// this to skip the barrier and rebinding work entirely in the normal case
// where nothing is installed.
[[nodiscard]] bool WantsPresent() noexcept;

// True when anything at all is loaded.
[[nodiscard]] bool Any() noexcept;

void OnDeviceCreated(D9Device12* dev) noexcept;
void OnResize(D9Device12* dev) noexcept;

// `rtvPtr` is the D3D12_CPU_DESCRIPTOR_HANDLE::ptr of the back buffer's RTV,
// which the caller has already bound as the sole render target. The command
// list is taken from the device's context rather than passed, so there is no
// way for the two to disagree about which list is open.
void OnPresent(D9Device12* dev, ID3D12Resource* backBuffer, UINT64 rtvPtr) noexcept;

void OnDeviceDestroyed() noexcept;

// Process teardown. Calls OnShutdown on everything, then frees the modules.
void Shutdown() noexcept;

// ── Runtime registration, for ASI mods ──────────────────────────────────────
//
// An .asi is loaded for its DllMain and has no entry point to call, so it joins
// the plugin system by calling these through the exports on d3d9.dll. See the
// runtime-registration section of <mwon12/mwon12.h> for the contract.
//
// void* rather than the ABI types because this header is compiled whether or
// not the SDK is present, and the stubs on the other side of that #if cannot
// name a struct that does not exist. dllmain.cpp casts at the boundary.

// Returns MWON12_OK (1) or MWON12_DECLINE (0). Takes effect on the render
// thread at the next safe point, not immediately.
[[nodiscard]] int RegisterAtRuntime(const void* api) noexcept;

void UnregisterAtRuntime(void* user) noexcept;

// The MWOn12_Host table, or null when the SDK was not built in.
[[nodiscard]] const void* HostTable() noexcept;

}
}
}

#endif
