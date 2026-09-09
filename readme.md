# MWOn12

**Need for Speed: Most Wanted (2005) rendered through DirectX 12.**

MWOn12 is a `d3d9.dll` that sits between the game and the graphics driver. The
game keeps making Direct3D 9 calls; MWOn12 turns them into Direct3D 12. The
game is never patched and never knows.

```
speed.exe
   │  Direct3D 9
   ▼
MWOn12  (d3d9.dll)
   │  Direct3D 12
   ▼
DXGI ──► GPU
```

---

## This is not D3D9On12

Microsoft ships a mapping layer called D3D9On12 that also puts D3D9 on top of
D3D12. MWOn12 does not use it, does not load it, and shares nothing with it but
a name that says what it does.

**Every part of the translation here is this project's own**: the D3D9 state
model, the shader translator, the fixed-function emulator, the descriptor
allocation, the resource barriers, the synchronisation. No D3D9On12, no
D3D11On12, no DXVK, no WineD3D.

MWOn12 does export `Direct3DCreate9On12` and `Direct3DCreate9On12Ex`, so that
anything resolving them by name still links — and then ignores the D3D12 device
it was handed and uses its own. Adopting a caller's device is exactly what On12
does, and exactly what this project exists to replace.

You can check: the built DLL imports `d3d12.dll`, `dxgi.dll`,
`D3DCOMPILER_47.dll`, `kernel32` and `user32`. Nothing else.

---

## Install

1. Build (below), or take a release.
2. Copy **`d3d9.dll`** and **`MWOn12.ini`** into the folder containing
   `speed.exe`.
3. Run the game.

Windows resolves an executable's imports from its own directory before the
system directory, so the game loads MWOn12 instead of the real runtime without
any patching.

To uninstall, delete the two files.

Check `MWOn12.log`, next to `speed.exe`, for which renderer you actually got:

```
MWOn12 v1.0.0 (DirectX 9 -> DirectX 12)  -  log opened.
Renderer backend: DirectX 12
```

---

## Configuration

Everything is in `MWOn12.ini`, read once at launch. The file documents every
key; the two that matter:

```ini
[Renderer]
Backend=2               ; 2 = DirectX 12.  1 = no translation at all.
BorderlessFullscreen=1  ; recommended
```

`Backend=1` hands the game to the system `d3d9.dll` untouched. It is an escape
hatch for a machine with no Direct3D 12 adapter, not a second renderer — MWOn12
falls back to it by itself if the adapter cannot do D3D12 feature level 11_0,
and says so in the log.

There are two options and they are numbered 1 and 2. MWDX used 1/2/3 with 2
meaning its DirectX 11 backend; an old ini that still says `3` is accepted and
means DirectX 12.

### If something looks wrong

Set `VerboseLog=1` and reproduce it, then read `MWOn12-render.log`. If the game
dies with "device removed", add `DredBreadcrumbs=1` — the log will then name the
GPU command that faulted. `DebugLayer=1` turns on D3D12 validation (needs the
Windows "Graphics Tools" optional feature) and pipes its messages into the same
log.

Turn them back off afterwards. Each one costs performance.

---

## Modding

[**MWOn12SDK**](../MWOn12SDK) is a separate folder next door, with three
things in it:

- **Shader kit** — replace the game's shaders with your own HLSL. No compiler,
  no code: set `ShaderDump=1` and `ShaderMods=1`, play, edit the dumped file,
  restart. Start here: most graphics mods are this.
- **Plugin SDK** — write a DLL that MWOn12 loads and hands the live D3D12
  device to, once per frame, with the back buffer bound and ready to draw on.
- **Graphics and hooking** — reach further than the present hook: see every
  draw as it is recorded and swap shaders at runtime with
  `<mwon12/graphics.hpp>`, or reach the game's own code with
  `<mwon12/hooks.hpp>`.

Plugins go in `<game>\MWOn12\Plugins\`. MWOn12 builds fine without the SDK
present; it just loads no plugins, and CMake says so at configure time.

### ASI mods

MWOn12 is also an **ASI loader**. Drop a `.asi` into `<game>\scripts\` (or
`<game>\MWOn12\ASI\`) and it runs — no separate loader needed, and no conflict
with one you already have.

An `.asi` is a DLL with a different extension and no required exports: it does
its work from `DllMain`, which is the format most existing Most Wanted mods
already ship in. They load on the first `Direct3DCreate9` — after the game's
code is in memory, before it has drawn anything, and outside the loader lock, so
hooks installed there are in place ahead of whatever they affect.

An ASI is not limited to gameplay either. MWOn12 exports `MWOn12_RegisterPlugin`
(with `MWOn12_UnregisterPlugin` and `MWOn12_GetHost`), so an ASI can join the
plugin system at runtime and get the live D3D12 device once per frame — the
SDK's `<mwon12/asi.hpp>` wraps that in one macro. Registration is deferred to
the render thread, so the ordering guarantee plugins rely on still holds.

So ASI versus plugin is a question of *when you run*, not what you may do. Both
can be installed at once.

Configured under `[ASI]` in `MWOn12.ini`; see `MWOn12SDK/docs/asi.md`.

---

## Build

```bat
Build.bat
```

Visual Studio 2022 with the C++ workload, and nothing else — no DirectX SDK, no
network. Output is `build_win32\Release\d3d9.dll`.

x86 only: `speed.exe` is 32-bit, and the build stops rather than producing a
DLL that can never load.

---

## Status and limits

The DirectX 12 renderer is what this project is for, and is the tested path.

Known limits, stated rather than discovered:

- **No multisampling.** Reported as unavailable, and a request for a
  multisampled target falls back to single-sampled with a log line. Reporting
  it honestly is deliberate — a game told MSAA exists and then handed a
  single-sampled target has no way to notice.
- **Tested against retail v1.3.** Other builds and other executables are not
  covered.

See [ARCHITECTURE.md](ARCHITECTURE.md) for how it is put together.
