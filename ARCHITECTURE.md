# MWOn12 — architecture

Internal document. `README.md` is for users and says nothing about internals;
this one says how the thing is built and which mistakes are already paid for.

---

## 1. What it is

A `d3d9.dll` proxy for *Need for Speed: Most Wanted* (2005), PC retail v1.3,
32-bit. The game loads it instead of the system `d3d9.dll` and believes
throughout that it is talking to Direct3D 9.

```
[Renderer]
Backend=2   DirectX 12   our own D3D9 -> D3D12 translation
Backend=1   DirectX 9    pass through to the real system d3d9.dll
```

`Backend=1` is not a renderer this project maintains. It is the escape hatch
for a machine with no D3D12 adapter, so the game still starts.

## 2. The hard rule

**No Microsoft or third-party mapping layer may perform the translation.**

Forbidden as the rendering backend: D3D9On12, D3D11On12, WineD3D, DXVK, or any
compatibility wrapper.

The exported `Direct3DCreate9On12[Ex]` entry points exist only so a caller that
resolves them by name gets a working device. They route into **our** factory
and deliberately ignore the caller-supplied D3D12 device.

The only `LoadLibrary` of a graphics runtime is `System32\d3d9.dll`, used
solely for `Backend=1`.

**Why it matters:** the point of the project is that *our* code owns D3D9 state
interpretation, shader translation, descriptor allocation and synchronisation.
Handing that to a mapping layer would produce a working game but not this
project. When a translation problem looks intractable, the answer is to fix our
translation.

**Verification:** the built DLL must import only `d3d12.dll`, `dxgi.dll`,
`D3DCOMPILER_47.dll`, and kernel32/user32. Check with
`dumpbin /DEPENDENTS d3d9.dll`. In particular it must not import `d3d9.dll` —
it *is* `d3d9.dll`, and a self-import means someone linked `d3d9.lib`.

## 3. Layout

```
src/
  dllmain.cpp                  exports, backend dispatch, loader log
  Logger.cpp                   MWOn12.log (loader banner only; ASCII only)
  sdk/PluginHost.cpp           loads and drives plugins; compiles out with no SDK
  DirectX-Handler/
    inc/core/                  headers for the shared core
    core/                      API-INDEPENDENT code, shared by everything:
                                 BackendSelect     ini/env parsing, adapter probe
                                 D3D9ShaderTranslator  D3D9 bytecode -> HLSL
                                 FFPShaderGen      fixed-function -> HLSL
                                 FormatConverter   D3DFORMAT <-> DXGI_FORMAT
                                 RenderStateTracker  the D3D9 shadow state
                                 CapsTable         the one D3DCAPS9 answer
                                 ShaderMods        dump/replace user shaders
                                 Log               MWOn12-render.log
    dx12/
      core/                    DeviceContext12, Resource12, TextureStorage12,
                               PSOCache12, ShaderCache12, FFPEmulator12, Blitter12
      d3d9proxy/               the IDirect3D*9 COM objects
    directx/                   DX9 passthrough wrappers (Backend=1)
```

`core/` holds nothing D3D12-specific. Only `dx12/` knows about D3D12 and only
`directx/` knows about the passthrough.

### Relationship to MWDX

MWOn12 is a fork of MWDX with the DirectX 11 backend removed. MWDX kept its
shared code under `dx11/core/`, which was accurate when written and misleading
by the end; here it is `core/`.

The DX12 backend is byte-for-byte MWDX's, apart from the plugin hooks and the
rename. If a rendering bug exists in one it exists in the other.

Removing DX11 also removed the last reason to link `d3d11.lib`. Two shared
files still referenced it and no longer do: `Log.cpp` had a
`DrainInfoQueue(ID3D11Device*)` with no callers, and `CapsTable.cpp` used
`D3D11_REQ_*` limit constants, now the identical `D3D12_REQ_*` ones.

MWDX also linked **vanhooks** (and fetched Zydis over the network to build it),
plus MWSDK, VanGFX and VanGUI include paths. No source file referenced any of
them. They are gone, which is why MWOn12 builds offline and needs no DirectX
SDK.

## 4. Build

```bat
Build.bat                                    :: CMake + VS 2022, Win32, full build
cmake --build build_win32 --config Release   :: incremental
```

Visual Studio 2022 and the Windows SDK. **No** DirectX June 2010 SDK — the
Windows SDK ships `d3d9.h`, `dxguid.lib` and the rest, and the DXSDK headers
are too old for the DXGI 1.2 types and the 4-argument `D3DReflect` this code
uses. **No** FetchContent, so no network.

The DX12 source list in `CMakeLists.txt` is **explicit, not globbed**. A new
`.cpp` must be added there or it links as a wall of unresolved externals that
look like missing code and are not.

Output is always `d3d9.dll` regardless of the CMake target name.

## 5. Backend selection

`BackendSelect` answers two questions, each once per process:

- `Configured()` — what `MWOn12.ini` asked for
- `Active()` — what the machine can actually provide

Everything is resolved lazily behind `std::call_once`, **never in `DllMain`**:
probing for a D3D12 adapter initialises graphics drivers, and doing that under
the loader lock deadlocks. The first `Direct3DCreate9` is the earliest safe
point and also the first point anyone needs the answer.

Fallback runs in both directions. No D3D12 adapter demotes to passthrough; a
missing or broken system `d3d9.dll` promotes to D3D12. The game gets a
renderer whatever happens, and the log says which and why.

`MWOn12.ini` is searched for beside the DLL and beside the executable, under
that name and then `MWDX.ini`, so an existing MWDX config keeps working. A path
that does not exist is fine: `GetPrivateProfileInt` then returns every default,
which is a valid config.

Every setting also has a `MWON12_*` environment override, so a diagnostic run
can be scripted without editing a user's config and forgetting to put it back.

## 6. Plugins

`src/sdk/PluginHost.cpp` is the renderer's half. The contract itself lives in
MWOn12SDK, in `<mwon12/mwon12.h>`; there is **no copy of that header here**,
because two copies drift and a plugin built against the wrong one is a crash
rather than an error. With the SDK absent, `MWON12_HAVE_SDK=0` and every host
function compiles to an empty inline.

Hook points, and why each is where it is:

| Hook | Site | Why there |
|---|---|---|
| scan + load | first `OnDeviceCreated` | `LoadLibrary` under the loader lock deadlocks, so `DllMain` only records the game directory |
| `OnDeviceCreated` | end of `D9Device12::FinishInit`, on success only | a plugin is promised a fully constructed device |
| `OnPresent` | `D9Device12::RunPresentPlugins`, before `PresentFrame` | the game has finished drawing, nothing has been presented |
| `OnResize` | end of `D9Device12::Reset` | the new back buffer exists and is bound |
| `OnDeviceDestroyed` | top of `~D9Device12` | before teardown, while the device is still usable |

### The present hook

`RunPresentPlugins` transitions the back buffer to `RENDER_TARGET`, binds it as
the sole target with no depth, and sets viewport and scissor to cover it — so
the simplest possible plugin works without knowing what the game left behind.

Afterwards it calls `m_ctx->MarkCommandStateDirty()`. **This is the whole
safety argument** and it is not a new invention: `Blitter12` already uses the
command list for its own passes and signals it exactly this way. The next
`PreDrawFlush` sees the dirty flag and re-binds descriptor heaps, root
signature, PSO, targets and every cached binding from scratch. `Present` then
invalidates the rest, and `OpenCommandList` sets the flag again for the new
frame. Nothing is read back, so there is nothing to restore by hand.

Leaving the back buffer in `RENDER_TARGET` is correct: `PresentFrame`
transitions it to `PRESENT` from whatever state it is in.

There is deliberately **no sandbox**. A plugin shares the address space and can
corrupt the device as easily as we can. The ABI documents what is safe;
enforcing it is impossible and pretending otherwise would be worse.

## 7. Things already learned the hard way

- **Never `LoadLibrary` under the loader lock.** Both the adapter probe and the
  plugin scan are deferred for this reason.
- **`LoadLibrary("d3d9")` loads *us*.** The passthrough must build an absolute
  path into `System32`, or the DLL loads itself.
- **A command list that failed to `Close` must not be submitted.** D3D12 does
  not fail the offending call; it poisons the list and tells you at `Close`.
  Submitting anyway gets the device removed, after which `Reset` returns
  `E_INVALIDARG` and the reported fault is nowhere near the cause.
- **Per-frame resources must be indexed by the frame in flight.** This applies
  to plugins too, which is why `MWOn12_Frame` carries `frameSlot`.
- **In a 32-bit process, address space fails before memory does.** A mapped
  2 MB buffer per allocation exhausts the 2 GB user range in under a thousand
  allocations; hence the upload ring's shared growth pool.
- **`MWOn12.log` is written as ANSI with no BOM.** Keep it ASCII — a UTF-8
  em-dash arrives as three bytes of mojibake in Notepad and in pasted bug
  reports. `MWOn12-render.log` writes a BOM and is fine.
