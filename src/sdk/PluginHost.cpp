// The renderer's half of the plugin ABI.
//
// A plugin is third-party code running inside the game's render thread with a
// live D3D12 command list, so the rules here are defensive on purpose:
//
//   - Every entry point is validated before it is trusted. A plugin whose
//     abiVersion or structSize does not match is rejected and its module freed,
//     rather than read through a struct laid out differently from ours.
//
//   - A plugin that declines is unloaded quietly. Declining is a normal answer
//     -- an overlay has nothing to do under the passthrough backend -- and
//     logging it as a failure trains people to ignore the log.
//
//   - Load failures never stop the game. The renderer is the product; a broken
//     plugin costs its own features and a warning, nothing more.
//
// What is deliberately NOT here is a sandbox. A plugin shares the address
// space and can corrupt the device as easily as the renderer can. The ABI
// documents what is safe to touch; enforcing it is not possible and pretending
// otherwise would be worse than saying so.

#include <sdk/PluginHost.h>

#include "Logger.h"

#if MWON12_HAVE_SDK

#include <mwon12/mwon12.h>

#include <core/BackendSelect.h>
#include <core/Log.h>
#include <core/DeviceContext12.h>
#include <d3d9proxy12/D9Device12.h>

#include <d3d12.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Reported to plugins as MWOn12_Host::version. Defined by CMake; the fallback
// keeps this file compilable on its own.
#ifndef MWON12_VERSION_STRING
#  define MWON12_VERSION_STRING "0.0.0-dev"
#endif

namespace mwon12 {
namespace sdk {
namespace {

struct Loaded {
    HMODULE      module{ nullptr };
    MWOn12_Plugin api{};
    std::string  name;      // owns the string the log uses; api.name is the
                            // plugin's own pointer and we do not assume it
                            // outlives the call that handed it over
};

std::once_flag        g_scanOnce;
std::vector<Loaded>   g_plugins;

// Plugins registered at runtime by an ASI, waiting to be taken on.
//
// An ASI registers from its own thread, whenever it likes -- but a plugin's
// callbacks are promised to arrive on the render thread and never concurrently,
// and calling OnDeviceCreated on the registering thread would break that on the
// first call. So a registration is queued here and the render thread picks it
// up at a point where it is safe to do so.
//
// g_pendingMutex guards both vectors below; g_hasPending lets the per-frame
// check be an atomic load rather than a lock on every present.
std::mutex             g_pendingMutex;
std::vector<Loaded>    g_pendingAdd;
std::vector<void*>     g_pendingRemove;
std::atomic<bool>      g_hasPending{ false };
std::string           g_gameDir;
std::string           g_pluginDir;
bool                  g_wantsPresent = false;
bool                  g_deviceLive   = false;

// Reused by ConfigInt/ConfigString. Resolved once, from the same rules
// BackendSelect uses, so a plugin and the renderer always read the same file.
std::wstring          g_iniPathW;

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1,
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ── Host services handed to every plugin ────────────────────────────────────

void MWON12_CALL HostLog(MWOn12_LogLevel level, const char* fmt, ...)
{
    if (!fmt) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    switch (level) {
    case MWON12_LOG_ERROR: DXLOG_ERROR("[plugin] %s", buf); break;
    case MWON12_LOG_WARN:  DXLOG_WARN ("[plugin] %s", buf); break;
    case MWON12_LOG_INFO:  DXLOG_INFO ("[plugin] %s", buf); break;
    default:               DXLOG_TRACE("[plugin] %s", buf); break;
    }
}

MWOn12_Backend MWON12_CALL HostActiveBackend()
{
    return (BackendSelect::Active() == Backend::DX12)
         ? MWON12_BACKEND_DX12
         : MWON12_BACKEND_DX9_PASSTHROUGH;
}

int MWON12_CALL HostConfigInt(const char* section, const char* key, int fallback)
{
    if (!section || !key || g_iniPathW.empty()) return fallback;
    return static_cast<int>(GetPrivateProfileIntW(
        Widen(section).c_str(), Widen(key).c_str(), fallback, g_iniPathW.c_str()));
}

int MWON12_CALL HostConfigString(const char* section, const char* key,
                                 const char* fallback, char* out, int outBytes)
{
    if (!out || outBytes <= 0) return 0;
    out[0] = '\0';
    if (!section || !key) return 0;

    wchar_t buf[1024]{};
    const DWORD n = GetPrivateProfileStringW(
        Widen(section).c_str(), Widen(key).c_str(),
        fallback ? Widen(fallback).c_str() : L"",
        buf, static_cast<DWORD>(std::size(buf)),
        g_iniPathW.empty() ? nullptr : g_iniPathW.c_str());

    const std::string s = Narrow(std::wstring(buf, n));
    const int copy = (static_cast<int>(s.size()) < outBytes - 1)
                   ? static_cast<int>(s.size()) : outBytes - 1;
    std::memcpy(out, s.data(), static_cast<size_t>(copy));
    out[copy] = '\0';
    return copy;
}

const char* MWON12_CALL HostGameDirectory()   { return g_gameDir.c_str(); }
const char* MWON12_CALL HostPluginDirectory() { return g_pluginDir.c_str(); }

const MWOn12_Host g_hostTable = {
    sizeof(MWOn12_Host),
    MWON12_ABI_VERSION,
    MWON12_VERSION_STRING,
    &HostLog,
    &HostActiveBackend,
    &HostConfigInt,
    &HostConfigString,
    &HostGameDirectory,
    &HostPluginDirectory,
};

// ── Loading ─────────────────────────────────────────────────────────────────

// Rejects anything we would otherwise have to guess about. structSize is
// checked as a range rather than for equality: a plugin built against an older
// header legitimately reports a smaller struct, and the fields it did not know
// about are already zero because we zeroed the struct before the call.
bool Accept(const MWOn12_Plugin& p, const wchar_t* file)
{
    if (p.abiVersion != MWON12_ABI_VERSION) {
        DXLOG_WARN("[plugin] %ls reports ABI version %u; this build implements "
                   "%u - not loaded", file, p.abiVersion, MWON12_ABI_VERSION);
        return false;
    }
    const unsigned minSize = static_cast<unsigned>(offsetof(MWOn12_Plugin, OnDeviceCreated));
    const unsigned maxSize = static_cast<unsigned>(sizeof(MWOn12_Plugin));
    if (p.structSize < minSize || p.structSize > maxSize) {
        DXLOG_WARN("[plugin] %ls reports structSize %u, which is outside the "
                   "range this build can read (%u..%u) - not loaded",
                   file, p.structSize, minSize, maxSize);
        return false;
    }
    return true;
}

void ScanAndLoad()
{
    if (g_gameDir.empty()) return;

    g_pluginDir = g_gameDir + "MWOn12\\Plugins\\";
    g_iniPathW  = BackendSelect::IniPath();

    const std::wstring pattern = Widen(g_pluginDir) + L"*.dll";

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        // Not an error, and not worth a warning: no plugin directory is the
        // normal state of a fresh install.
        DXLOG_INFO("[plugin] no plugins found in %s", g_pluginDir.c_str());
        return;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        const std::wstring full = Widen(g_pluginDir) + fd.cFileName;

        // LOAD_WITH_ALTERED_SEARCH_PATH so a plugin's own dependencies resolve
        // from the plugin directory rather than the game directory.
        HMODULE mod = LoadLibraryExW(full.c_str(), nullptr,
                                     LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod) {
            DXLOG_WARN("[plugin] %ls could not be loaded (error %lu) - skipped",
                       fd.cFileName, GetLastError());
            continue;
        }

        auto entry = reinterpret_cast<MWOn12_PluginMainFn>(
            GetProcAddress(mod, MWON12_PLUGIN_ENTRY_NAME));
        if (!entry) {
            // Almost always a DLL that simply is not a plugin -- something the
            // user dropped in the folder by mistake. Say which, and move on.
            DXLOG_INFO("[plugin] %ls exports no %s and is not an MWOn12 plugin "
                       "- skipped", fd.cFileName, MWON12_PLUGIN_ENTRY_NAME);
            FreeLibrary(mod);
            continue;
        }

        MWOn12_Plugin api{};
        const int rc = entry(&g_hostTable, &api);

        if (rc != MWON12_OK) {
            DXLOG_INFO("[plugin] %ls declined to load", fd.cFileName);
            FreeLibrary(mod);
            continue;
        }
        if (!Accept(api, fd.cFileName)) {
            FreeLibrary(mod);
            continue;
        }

        Loaded l;
        l.module = mod;
        l.api    = api;
        l.name   = api.name ? api.name : Narrow(fd.cFileName);
        if (api.OnPresent) g_wantsPresent = true;

        DXLOG_INFO("[plugin] loaded %s%s%s from %ls",
                   l.name.c_str(),
                   api.version ? " v" : "", api.version ? api.version : "",
                   fd.cFileName);
        g_plugins.push_back(std::move(l));

    } while (FindNextFileW(h, &fd));

    FindClose(h);

    if (!g_plugins.empty())
        DXLOG_INFO("[plugin] %u plugin(s) active, %s",
                   static_cast<unsigned>(g_plugins.size()),
                   g_wantsPresent ? "at least one drawing each frame"
                                  : "none drawing");
}

// Fills the device description from whatever the context currently says.
// Rebuilt per call rather than cached because a Reset changes most of it.
void FillDeviceInfo(D9Device12* dev, MWOn12_DeviceInfo& out)
{
    auto* ctx = dev->Ctx();
    out.structSize       = sizeof(MWOn12_DeviceInfo);
    out.device           = ctx->Device();
    out.commandQueue     = ctx->CmdQueue();
    out.swapChain        = ctx->SwapChain();
    out.d3d9Device       = static_cast<IDirect3DDevice9*>(dev);
    out.hwnd             = ctx->TargetHwnd();
    out.width            = ctx->BackBufferWidth();
    out.height           = ctx->BackBufferHeight();
    out.backBufferFormat = static_cast<uint32_t>(ctx->BackBufferFormat());
    out.frameCount       = ctx->BackBufferCount();
}

// Takes on everything an ASI has registered, and drops anything it has
// unregistered. Called only from the render thread, at points where running a
// plugin's OnDeviceCreated is safe.
//
// `deviceLive` says whether the device exists yet. When it does, a newly taken
// plugin gets its OnDeviceCreated here and now -- it missed the one the others
// received, and without it the plugin would reach OnPresent having never been
// told about the device it is meant to draw with.
void DrainPending(D9Device12* dev)
{
    if (!g_hasPending.load(std::memory_order_acquire)) return;

    std::vector<Loaded> added;
    std::vector<void*>  removed;
    {
        std::lock_guard lk(g_pendingMutex);
        added.swap(g_pendingAdd);
        removed.swap(g_pendingRemove);
        g_hasPending.store(false, std::memory_order_release);
    }

    for (void* user : removed) {
        for (auto it = g_plugins.begin(); it != g_plugins.end(); ++it) {
            if (it->api.user != user) continue;
            DXLOG_INFO("[plugin] %s unregistered", it->name.c_str());
            if (g_deviceLive && it->api.OnDeviceDestroyed)
                it->api.OnDeviceDestroyed(it->api.user);
            g_plugins.erase(it);
            break;
        }
    }

    for (auto& l : added) {
        if (l.api.OnPresent) g_wantsPresent = true;

        if (g_deviceLive && dev && dev->Ctx() && l.api.OnDeviceCreated) {
            MWOn12_DeviceInfo info{};
            FillDeviceInfo(dev, info);
            l.api.OnDeviceCreated(l.api.user, &info);
        }

        DXLOG_INFO("[plugin] %s%s%s registered at runtime",
                   l.name.c_str(),
                   l.api.version ? " v" : "", l.api.version ? l.api.version : "");
        g_plugins.push_back(std::move(l));
    }
}

}  // namespace

namespace PluginHost {

void NoteGameDirectory(const char* dir) noexcept
{
    if (dir) g_gameDir = dir;
}

void EnsureLoaded() noexcept
{
    std::call_once(g_scanOnce, ScanAndLoad);
}

// The pending check is what stops a chicken-and-egg: an ASI that registers a
// plugin with only an OnPresent would never be drained, because draining
// happens inside the present path this flag gates.
bool WantsPresent() noexcept
{
    return (g_wantsPresent || g_hasPending.load(std::memory_order_acquire))
        && g_deviceLive;
}
bool Any() noexcept
{
    return !g_plugins.empty() || g_hasPending.load(std::memory_order_acquire);
}

void OnDeviceCreated(D9Device12* dev) noexcept
{
    EnsureLoaded();
    // Before the guard below, not after: an ASI may have registered the only
    // plugin there is, and g_plugins would still be empty.
    DrainPending(dev);
    if (g_plugins.empty() || !dev || !dev->Ctx()) return;

    MWOn12_DeviceInfo info{};
    FillDeviceInfo(dev, info);

    for (auto& p : g_plugins)
        if (p.api.OnDeviceCreated)
            p.api.OnDeviceCreated(p.api.user, &info);

    // Only now: a plugin's OnPresent must never run before its
    // OnDeviceCreated, and Present is reached from another path.
    g_deviceLive = true;
}

void OnResize(D9Device12* dev) noexcept
{
    if (g_plugins.empty() || !dev || !dev->Ctx() || !g_deviceLive) return;

    MWOn12_DeviceInfo info{};
    FillDeviceInfo(dev, info);

    for (auto& p : g_plugins)
        if (p.api.OnResize)
            p.api.OnResize(p.api.user, &info);
}

void OnPresent(D9Device12* dev, ID3D12Resource* backBuffer, UINT64 rtvPtr) noexcept
{
    if (!g_deviceLive || !dev) return;

    // The safe point for taking on an ASI's registration: on the render thread,
    // between frames' worth of work, with the device up.
    DrainPending(dev);

    if (!g_wantsPresent) return;
    auto* ctx = dev->Ctx();
    if (!ctx || !ctx->CmdList()) return;

    MWOn12_Frame f{};
    f.structSize       = sizeof(MWOn12_Frame);
    f.device           = ctx->Device();
    f.commandQueue     = ctx->CmdQueue();
    f.commandList      = ctx->CmdList();
    f.swapChain        = ctx->SwapChain();
    f.backBuffer       = backBuffer;
    f.d3d9Device       = static_cast<IDirect3DDevice9*>(dev);
    f.backBufferRtv    = rtvPtr;
    f.width            = ctx->BackBufferWidth();
    f.height           = ctx->BackBufferHeight();
    f.backBufferFormat = static_cast<uint32_t>(ctx->BackBufferFormat());
    f.frameSlot        = ctx->FrameIndex();
    f.frameCount       = ctx->BackBufferCount();
    f.frameNumber      = ctx->FrameCounter();

    for (auto& p : g_plugins)
        if (p.api.OnPresent)
            p.api.OnPresent(p.api.user, &f);
}

void OnDeviceDestroyed() noexcept
{
    if (!g_deviceLive) return;
    g_deviceLive = false;

    for (auto& p : g_plugins)
        if (p.api.OnDeviceDestroyed)
            p.api.OnDeviceDestroyed(p.api.user);
}

int RegisterAtRuntime(const void* apiRaw) noexcept
{
    if (!apiRaw) return MWON12_DECLINE;
    const auto* api = static_cast<const MWOn12_Plugin*>(apiRaw);
    if (!Accept(*api, L"<runtime registration>")) return MWON12_DECLINE;

    Loaded l;
    l.module = nullptr;            // not ours to free; the ASI owns its module
    l.api    = *api;               // copied, as the ABI documents
    l.name   = api->name ? api->name : "unnamed";

    {
        std::lock_guard lk(g_pendingMutex);
        g_pendingAdd.push_back(std::move(l));
        g_hasPending.store(true, std::memory_order_release);
    }
    return MWON12_OK;
}

void UnregisterAtRuntime(void* user) noexcept
{
    std::lock_guard lk(g_pendingMutex);
    g_pendingRemove.push_back(user);
    g_hasPending.store(true, std::memory_order_release);
}

const void* HostTable() noexcept
{
    // An ASI can ask for this before any device exists, and the ini path is
    // resolved by the scan. Make sure it has happened so ConfigInt works.
    EnsureLoaded();
    return &g_hostTable;
}

void Shutdown() noexcept
{
    // The device is normally gone by now, but a process torn down abruptly can
    // reach here with one still live. Plugins are promised this ordering.
    OnDeviceDestroyed();

    for (auto& p : g_plugins)
        if (p.api.OnShutdown)
            p.api.OnShutdown(p.api.user);

    // Deliberately not FreeLibrary. This runs from DLL_PROCESS_DETACH, where
    // the loader lock is held and every other module may already be unloaded;
    // freeing one there is a documented way to deadlock or fault. The process
    // is ending, so the mapping costs nothing.
    g_plugins.clear();
    g_wantsPresent = false;

    std::lock_guard lk(g_pendingMutex);
    g_pendingAdd.clear();
    g_pendingRemove.clear();
    g_hasPending.store(false, std::memory_order_release);
}

}
}
}

#else  // !MWON12_HAVE_SDK

namespace mwon12 {
namespace sdk {
namespace PluginHost {

void NoteGameDirectory(const char*) noexcept {}
void EnsureLoaded() noexcept {}
bool WantsPresent() noexcept { return false; }
bool Any() noexcept { return false; }
void OnDeviceCreated(D9Device12*) noexcept {}
void OnResize(D9Device12*) noexcept {}
void OnPresent(D9Device12*, ID3D12Resource*, UINT64) noexcept {}
void OnDeviceDestroyed() noexcept {}
void Shutdown() noexcept {}
int  RegisterAtRuntime(const void*) noexcept { return 0; }
void UnregisterAtRuntime(void*) noexcept {}
const void* HostTable() noexcept { return nullptr; }

}
}
}

#endif  // MWON12_HAVE_SDK
