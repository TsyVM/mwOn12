// The renderer's diagnostic log.
//
// Opens MWOn12-render.log beside the executable and serialises writes from every
// thread. Warnings and errors are always recorded; informational and trace
// messages only when verbose logging is enabled, so that a normal run leaves a
// short file and a diagnostic run leaves a complete one.
//
// Lines are flushed as they are written. The most valuable lines are the last
// ones before a crash, and those are exactly what a buffer loses.
//
// HRESULT values are decoded to names where they are known. A reader should
// not have to look up 0x8876086C to find out it means an invalid call.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/Log.h>

#include <d3d9.h>
#include <dxgi.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <atomic>

namespace mwon12 {
namespace log {

namespace {

SRWLOCK           g_lock       = SRWLOCK_INIT;
HANDLE            g_file       = INVALID_HANDLE_VALUE;
std::atomic<bool> g_initDone{ false };
std::atomic<bool> g_verbose{ false };
std::atomic<bool> g_fatalBoxShown{ false };
wchar_t           g_path[MAX_PATH] = L"";

void ThisModuleDir(wchar_t* out, size_t cch) noexcept
{
    out[0] = L'\0';
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ThisModuleDir), &self))
        return;
    wchar_t path[MAX_PATH]{};
    if (!self || GetModuleFileNameW(self, path, MAX_PATH) == 0)
        return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = L'\0';
    wcsncpy_s(out, cch, path, _TRUNCATE);
}

int NarrowInto(const wchar_t* w, char* dst, int dstCap) noexcept
{
    if (!w || !*w) { if (dstCap) dst[0] = '\0'; return 0; }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, dstCap, nullptr, nullptr);
    return n > 0 ? n - 1 : 0;
}

void RawWrite(const char* bytes, int len) noexcept
{
    OutputDebugStringA(bytes);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(g_file, bytes, static_cast<DWORD>(len), &wrote, nullptr);
    }
}

const char* LevelTag(Level l) noexcept
{
    switch (l) {
    case Level::Trace: return "TRACE";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    case Level::Fatal: return "FATAL";
    }
    return "?????";
}

const char* LeafFile(const char* file) noexcept
{
    if (!file) return "?";
    const char* leaf = file;
    for (const char* p = file; *p; ++p)
        if (*p == '\\' || *p == '/') leaf = p + 1;
    return leaf;
}

int FormatPrefix(char* buf, int cap, Level lvl,
                 const char* file, int line, const char* func) noexcept
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return std::snprintf(
        buf, cap, "[%02u:%02u:%02u.%03u][t%05lu][%s][%s:%d %s] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        GetCurrentThreadId(), LevelTag(lvl),
        LeafFile(file), line, func ? func : "?");
}

void Emit(Level lvl, const char* file, int line, const char* func,
          const char* body) noexcept
{

    if (lvl < Level::Warn && !g_verbose.load(std::memory_order_relaxed))
        return;

    char line_buf[4096];
    int n = FormatPrefix(line_buf, sizeof(line_buf), lvl, file, line, func);
    if (n < 0) n = 0;
    if (n > (int)sizeof(line_buf) - 2) n = (int)sizeof(line_buf) - 2;
    int m = std::snprintf(line_buf + n, sizeof(line_buf) - n, "%s\n", body ? body : "");
    int total = (m < 0) ? n : (n + m);
    if (total > (int)sizeof(line_buf) - 1) total = (int)sizeof(line_buf) - 1;
    line_buf[total] = '\0';

    AcquireSRWLockExclusive(&g_lock);
    RawWrite(line_buf, total);
    if (lvl >= Level::Warn && g_file != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_file);
    ReleaseSRWLockExclusive(&g_lock);

    if (lvl == Level::Fatal) {
        bool expected = false;
        if (g_fatalBoxShown.compare_exchange_strong(expected, true)) {
            wchar_t msg[1024];
            _snwprintf_s(msg, _TRUNCATE,
                L"DirectX 9->11.1 layer: a fatal error occurred.\n\n%hs\n\n"
                L"Full details were written to:\n%s\n\n"
                L"Please send that log file when reporting this.",
                body ? body : "(no detail)",
                g_path[0] ? g_path : L"(log file unavailable)");
            MessageBoxW(nullptr, msg, L"MWOn12 - DirectX error",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
        }
    }
}

}

void Init() noexcept
{
    bool expected = false;
    if (!g_initDone.compare_exchange_strong(expected, true))
        return;

    wchar_t dir[MAX_PATH];
    ThisModuleDir(dir, MAX_PATH);

    // The previous run's log is kept as MWOn12-render.prev.log rather than
    // truncated.
    //
    // This log is opened fresh on every launch, which loses the evidence in the
    // two cases it is most wanted: a crash followed immediately by a relaunch,
    // and a comparison run where the second backend overwrites the first
    // backend's capture. One generation costs a rename and removes both.
    auto tryOpen = [](const wchar_t* full) -> HANDLE {
        wchar_t prev[MAX_PATH];
        const wchar_t* ext = wcsrchr(full, L'.');
        if (ext) {
            const size_t stem = static_cast<size_t>(ext - full);
            _snwprintf_s(prev, _TRUNCATE, L"%.*s.prev%s",
                         static_cast<int>(stem), full, ext);
            DeleteFileW(prev);
            MoveFileW(full, prev);
        }
        return CreateFileW(full, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    };

    if (dir[0]) {
        _snwprintf_s(g_path, _TRUNCATE, L"%sMWOn12-render.log", dir);
        g_file = tryOpen(g_path);
    }
    if (g_file == INVALID_HANDLE_VALUE) {
        wchar_t tmp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tmp)) {
            _snwprintf_s(g_path, _TRUNCATE, L"%sMWOn12-render.log", tmp);
            g_file = tryOpen(g_path);
        }
    }
    if (g_file == INVALID_HANDLE_VALUE)
        g_path[0] = L'\0';

    if (g_file != INVALID_HANDLE_VALUE) {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD w = 0; WriteFile(g_file, bom, 3, &w, nullptr);
    }

    char banner[512];
    char pathUtf8[MAX_PATH * 2];
    NarrowInto(g_path[0] ? g_path : L"(none)", pathUtf8, sizeof(pathUtf8));
    SYSTEMTIME st; GetLocalTime(&st);
    int n = std::snprintf(banner, sizeof(banner),
        "\xEF\xBB\xBF" "===== MWOn12 render log (DirectX 9 -> DirectX 12) =====\n"
        "started %04u-%02u-%02u %02u:%02u:%02u  |  file: %s\n"
        "levels: Warn/Error/Fatal always; Trace/Info need VerboseLog=1\n"
        "==========================================================\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, pathUtf8);

    // snprintf reports the length it WANTED, not the length it wrote, so a
    // long log path -- g_path is MAX_PATH wide and its UTF-8 form up to twice
    // that -- makes n exceed the 512-byte banner. Writing n bytes from a
    // 512-byte buffer reads past the end of the stack frame and puts whatever
    // follows it into the log.
    if (n < 0) n = 0;
    if (n > static_cast<int>(sizeof(banner)) - 1)
        n = static_cast<int>(sizeof(banner)) - 1;

    AcquireSRWLockExclusive(&g_lock);
    if (n > 3) RawWrite(banner + 3, n - 3);
    if (g_file != INVALID_HANDLE_VALUE) FlushFileBuffers(g_file);
    ReleaseSRWLockExclusive(&g_lock);
}

void Shutdown() noexcept
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void SetVerbose(bool on) noexcept { g_verbose.store(on, std::memory_order_relaxed); }
bool Verbose() noexcept           { return g_verbose.load(std::memory_order_relaxed); }
const wchar_t* FilePath() noexcept { return g_path; }

void Write(Level lvl, const char* file, int line, const char* func,
           const char* fmt, ...) noexcept
{
    if (lvl < Level::Warn && !g_verbose.load(std::memory_order_relaxed))
        return;
    char body[3072];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt ? fmt : "", ap);
    va_end(ap);
    Emit(lvl, file, line, func, body);
}

void WriteHR(Level lvl, const char* file, int line, const char* func,
             HRESULT hr, const char* fmt, ...) noexcept
{
    if (lvl < Level::Warn && !g_verbose.load(std::memory_order_relaxed))
        return;
    char msg[2560];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);

    char body[3072];
    std::snprintf(body, sizeof(body), "%s  ->  hr=0x%08lX %s",
                  msg, static_cast<unsigned long>(hr), DecodeHR(hr));
    Emit(lvl, file, line, func, body);
}

const char* DecodeHR(HRESULT hr) noexcept
{
    switch (hr) {

    case S_OK:                          return "S_OK";
    case S_FALSE:                       return "S_FALSE";

    case E_FAIL:                        return "E_FAIL (unspecified failure)";
    case E_INVALIDARG:                  return "E_INVALIDARG (bad argument)";
    case E_OUTOFMEMORY:                 return "E_OUTOFMEMORY";
    case E_NOTIMPL:                     return "E_NOTIMPL (not implemented)";
    case E_NOINTERFACE:                 return "E_NOINTERFACE (QueryInterface miss)";
    case E_POINTER:                     return "E_POINTER (null pointer)";
    case E_ACCESSDENIED:                return "E_ACCESSDENIED";

    case DXGI_ERROR_DEVICE_REMOVED:     return "DXGI_ERROR_DEVICE_REMOVED (GPU removed / driver crash - call GetDeviceRemovedReason)";
    case DXGI_ERROR_DEVICE_HUNG:        return "DXGI_ERROR_DEVICE_HUNG (GPU hang - usually a bad draw/shader)";
    case DXGI_ERROR_DEVICE_RESET:       return "DXGI_ERROR_DEVICE_RESET (device reset - TDR)";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL:       return "DXGI_ERROR_INVALID_CALL (illegal parameters to the graphics API)";
    case DXGI_ERROR_WAS_STILL_DRAWING:  return "DXGI_ERROR_WAS_STILL_DRAWING";
    case DXGI_ERROR_UNSUPPORTED:        return "DXGI_ERROR_UNSUPPORTED (format/feature not supported)";
    case DXGI_ERROR_NOT_FOUND:          return "DXGI_ERROR_NOT_FOUND";
    case DXGI_ERROR_MORE_DATA:          return "DXGI_ERROR_MORE_DATA";
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
    case DXGI_ERROR_SDK_COMPONENT_MISSING: return "DXGI_ERROR_SDK_COMPONENT_MISSING (Graphics Tools not installed - debug layer)";

    case D3DERR_DEVICELOST:             return "D3DERR_DEVICELOST (device lost - needs Reset)";
    case D3DERR_DEVICENOTRESET:         return "D3DERR_DEVICENOTRESET (lost, resettable now)";
    case D3DERR_DEVICEREMOVED:          return "D3DERR_DEVICEREMOVED";
    case D3DERR_DEVICEHUNG:             return "D3DERR_DEVICEHUNG";
    case D3DERR_DRIVERINTERNALERROR:    return "D3DERR_DRIVERINTERNALERROR";
    case D3DERR_INVALIDCALL:            return "D3DERR_INVALIDCALL (bad arguments to a D3D9 call)";
    case D3DERR_NOTAVAILABLE:           return "D3DERR_NOTAVAILABLE (format/usage/caps unsupported)";
    case D3DERR_OUTOFVIDEOMEMORY:       return "D3DERR_OUTOFVIDEOMEMORY";
    case D3DERR_WRONGTEXTUREFORMAT:     return "D3DERR_WRONGTEXTUREFORMAT";
    case D3DERR_UNSUPPORTEDCOLOROPERATION: return "D3DERR_UNSUPPORTEDCOLOROPERATION";
    case D3DERR_UNSUPPORTEDTEXTUREFILTER:  return "D3DERR_UNSUPPORTEDTEXTUREFILTER";
    case D3DERR_CONFLICTINGRENDERSTATE: return "D3DERR_CONFLICTINGRENDERSTATE";
    case D3DERR_WASSTILLDRAWING:        return "D3DERR_WASSTILLDRAWING";

    default: break;
    }
    return "(unrecognised HRESULT)";
}

}
}
