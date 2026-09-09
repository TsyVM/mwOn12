<div align="center">

# MWOn12

<p><em>Need for Speed: Most Wanted (2005), rendered through DirectX 12</em></p>

[![License: MIT](https://img.shields.io/badge/License-MIT-D2B48C?style=for-the-badge&labelColor=1C1008)](LICENSE.txt)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-D2B48C?style=for-the-badge&labelColor=1C1008&logo=cplusplus&logoColor=D2B48C)](https://en.cppreference.com/w/cpp/23)
[![Windows](https://img.shields.io/badge/Windows-x86-D2B48C?style=for-the-badge&labelColor=1C1008&logo=windows&logoColor=D2B48C)](#-build)
[![Direct3D 12](https://img.shields.io/badge/Direct3D-12-D2B48C?style=for-the-badge&labelColor=1C1008)](#-this-is-not-d3d9on12)
[![TeamVanilla](https://img.shields.io/badge/Team-TeamVanilla-D2B48C?style=for-the-badge&labelColor=1C1008)](https://www.teamvanilla.org/)

<br/>

[![Stars](https://img.shields.io/github/stars/tsyvm/mwon12?style=for-the-badge&color=D2B48C&labelColor=1C1008)](../../stargazers)
[![Issues](https://img.shields.io/github/issues/tsyvm/mwon12?style=for-the-badge&color=D2B48C&labelColor=1C1008)](../../issues)
[![Last Commit](https://img.shields.io/github/last-commit/tsyvm/mwon12?style=for-the-badge&color=D2B48C&labelColor=1C1008)](../../commits)

</div>

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:1C1008,50:6B4226,100:1C1008&height=3"/>

MWOn12 is a **`d3d9.dll` that sits between the game and the graphics driver**. The
game keeps making Direct3D 9 calls; MWOn12 turns them into Direct3D 12. The game
is never patched and never knows.

```
speed.exe
   │  Direct3D 9
   ▼
MWOn12  (d3d9.dll)
   │  Direct3D 12
   ▼
DXGI ──► GPU
```

Every part of the translation is **this project's own** — the D3D9 state model,
the shader translator, the fixed-function emulator, the descriptor allocation,
the resource barriers, the synchronisation. It is renderer-complete rather than
a shim over somebody else's mapping layer, and it is **self-contained**: no
DirectX SDK, no package manager, no network to build it.

<div align="center">

### Contents

[Not D3D9On12](#-this-is-not-d3d9on12) · [Install](#-install) · [Configuration](#-configuration) · [Modding](#-modding) · [Build](#-build) · [Status](#-status-and-limits) · [License](#-license)

</div>

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:1C1008,50:6B4226,100:1C1008&height=3"/>

## 🚫 This is not D3D9On12

Microsoft ships a mapping layer called D3D9On12 that also puts D3D9 on top of
D3D12. MWOn12 **does not use it, does not load it**, and shares nothing with it
but a name that says what it does.

No D3D9On12, no D3D11On12, no DXVK, no WineD3D.

MWOn12 does export `Direct3DCreate9On12` and `Direct3DCreate9On12Ex`, so that
anything resolving them by name still links — and then ignores the D3D12 device
it was handed and uses its own. Adopting a caller's device is exactly what On12
does, and exactly what this project exists to replace.

**You can check.** The built DLL imports `d3d12.dll`, `dxgi.dll`,
`D3DCOMPILER_47.dll`, `kernel32` and `user32`. Nothing else.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:1C1008,50:6B4226,100:1C1008&height=3"/>

## 📦 Install

1. Build (below), or take a release.
2. Copy **`d3d9.dll`** and **`MWOn12.ini`** into the folder containing `speed.exe`.
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

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:1C1008,50:6B4226,100:1C1008&height=3"/>

## ⚙️ Configuration

Everything is in `MWOn12.ini`, read once at launch. The file documents every
key; the two that matter:

```ini
[Renderer]
Backend=2               ; 2 = DirectX 12.  1 = no translation at all.
BorderlessFullscreen=1  ; recommended
```

`Backend=1` hands the game to the system `d3d9.dll` untouched. It is an **escape
hatch** for a machine with no Direct3D 12 adapter, not a second renderer —
MWOn12 falls back to it by itself if the adapter cannot do D3D12 feature level
11_0, and says so in the log.

There are two options and they are numbered 1 and 2. MWDX used 1/2/3 with 2
meaning its DirectX 11 backend; an old ini that still says `3` is accepted and
means DirectX 12.

### If something looks wrong

| Setting | What it gives you |
|---|---|
| `VerboseLog=1` | The full render trace in `MWOn12-render.log` |
| `DredBreadcrumbs=1` | On "device removed", names the GPU command that faulted |
| `DebugLayer=1` | D3D12 validation into the same log (needs the Windows **Graphics Tools** optional feature) |

Turn them back off afterwards. Each one costs performance.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:1C1008,50:6B4226,100:1C1008&height=3"/>

## 🧩 Modding

**[MWOn12SDK](../MWOn12SDK)** is a separate folder next door, with three things
in it:

| | |
|---|---|
| **Shader kit** | Replace the game's shaders with your own HLSL. No compiler, no code: set `ShaderDump=1` and `ShaderMods=1`, play, edit the dumped file, restart. **Start here** — most graphics mods are this. |
| **Plugin SDK** | Write a DLL that MWOn12 loads and hands the live D3D12 device to, once per frame, with the back buffer bound and ready to draw on. |
| **Graphics and hooking** | Reach further than the present hook: see every draw as it is recorded and swap shaders at runtime with `<mwon12/graphics.hpp>`, or reach the game's own code with `<mwon12/hooks.hpp>`. |

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

**An ASI is not limited to gameplay.** MWOn12 exports `MWOn12_RegisterPlugin`
(with `MWOn12_UnregisterPlugin` and `MWOn12_GetHost`), so an ASI can join the
plugin system at runtime and get the live D3D12 device once per frame — the
SDK's `<mwon12/asi.hpp>` wraps that in one macro. Registration is deferred to
the render thread, so the ordering guarantee plugins rely on still holds.

So ASI versus plugin is a question of **when you run**, not what you may do.
Both can be installed at once.

Configured under `[ASI]` in `MWOn12.ini`; see `MWOn12SDK/docs/asi.md`.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:1C1008,50:6B4226,100:1C1008&height=3"/>

## 🛠️ Build

```bat
Build.bat
```

Visual Studio 2022 with the C++ workload, and nothing else — no DirectX SDK, no
network. Output is `build_win32\Release\d3d9.dll`.

**x86 only:** `speed.exe` is 32-bit, and the build stops rather than producing a
DLL that can never load.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:1C1008,50:6B4226,100:1C1008&height=3"/>

## 📊 Status and limits

The DirectX 12 renderer is what this project is for, and is the tested path.

Known limits, stated rather than discovered:

- **No multisampling.** Reported as unavailable, and a request for a
  multisampled target falls back to single-sampled with a log line. Reporting it
  honestly is deliberate — a game told MSAA exists and then handed a
  single-sampled target has no way to notice.
- **Tested against retail v1.3.** Other builds and other executables are not
  covered.

See **[ARCHITECTURE.md](ARCHITECTURE.md)** for how it is put together.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:1C1008,50:6B4226,100:1C1008&height=3"/>

## 📄 License

MWOn12 is licensed under the MIT License — see [LICENSE.txt](LICENSE.txt).

<div align="center">

<sub>Built and maintained by <a href="https://github.com/TsyVM">TsyVM</a> · <a href="https://www.teamvanilla.org/">TeamVanilla</a></sub>

<img width="100%" src="https://capsule-render.vercel.app/api?type=waving&color=0:6B4226,100:1C1008&height=80&section=footer"/>

</div>
