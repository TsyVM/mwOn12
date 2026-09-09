// ASI loading. See AsiLoader.h for what an ASI is and why it is not a plugin.
//
// The whole of it is: find the files, LoadLibrary each one, keep the handle.
// There is no ABI to check and no entry point to call, because an ASI has
// neither -- its DllMain has already run by the time LoadLibrary returns, and
// whatever it was going to do is done.
//
// Two consequences worth being explicit about, since both look like omissions:
//
//   Nothing is ever unloaded. An ASI installs detours into the game's code and
//   leaves them there; freeing the module would leave those detours pointing
//   into unmapped memory, and the crash would happen later and somewhere else.
//   Even a failed-looking ASI stays loaded.
//
//   Nothing is validated. A DLL that is not really an ASI simply runs its
//   DllMain and does nothing, which is indistinguishable from an ASI that
//   chose to do nothing. There is no signal to check, so there is no check.

#include <sdk/AsiLoader.h>

#include "Logger.h"
#include <core/BackendSelect.h>
#include <core/Log.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

namespace mwon12::asi {
namespace {

std::vector<HMODULE> g_loaded;
std::once_flag       g_once;

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring GameDirW()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    const auto slash = dir.rfind(L'\\');
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    return dir;
}

int IniInt(const wchar_t* key, int fallback)
{
    const std::wstring ini = BackendSelect::IniPath();
    if (ini.empty()) return fallback;
    return static_cast<int>(GetPrivateProfileIntW(L"ASI", key, fallback,
                                                  ini.c_str()));
}

std::wstring IniString(const wchar_t* key)
{
    const std::wstring ini = BackendSelect::IniPath();
    if (ini.empty()) return {};
    wchar_t buf[1024]{};
    const DWORD n = GetPrivateProfileStringW(L"ASI", key, L"", buf,
                                             static_cast<DWORD>(std::size(buf)),
                                             ini.c_str());
    return std::wstring(buf, n);
}

// Loads every .asi in one directory. `loadedNames` carries the base names
// already taken, so the same mod present in two of the search directories is
// loaded once rather than twice -- loading it twice would install its hooks
// twice, and a detour chained onto itself usually means infinite recursion.
void LoadFrom(const std::wstring& dir, std::vector<std::wstring>& loadedNames)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"*.asi").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;   // no such folder is the normal case

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring lower = fd.cFileName;
        for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));

        bool already = false;
        for (const auto& n : loadedNames) if (n == lower) { already = true; break; }
        if (already) {
            DXLOG_INFO("[asi] %ls in %ls is already loaded from another "
                       "directory - skipped",
                       fd.cFileName, dir.c_str());
            continue;
        }

        const std::wstring full = dir + fd.cFileName;

        // LOAD_WITH_ALTERED_SEARCH_PATH so an ASI's own dependencies resolve
        // from the folder it sits in rather than the game directory. Most ASIs
        // are self-contained; the ones that are not break without this in a way
        // that reads as "the mod does not work" rather than "a DLL is missing".
        HMODULE mod = LoadLibraryExW(full.c_str(), nullptr,
                                     LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod) {
            const DWORD err = GetLastError();
            DXLOG_WARN("[asi] %ls could not be loaded (error %lu)%s",
                       fd.cFileName, err,
                       err == ERROR_BAD_EXE_FORMAT
                           ? " - this is a 64-bit build; speed.exe is 32-bit"
                           : "");
            continue;
        }

        loadedNames.push_back(lower);
        g_loaded.push_back(mod);
        DXLOG_INFO("[asi] loaded %ls from %ls", fd.cFileName, dir.c_str());

    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

void LoadAllOnce()
{
    if (!IniInt(L"Enabled", 1)) {
        DXLOG_INFO("[asi] disabled in MWOn12.ini");
        return;
    }

    const std::wstring game = GameDirW();
    if (game.empty()) return;

    std::vector<std::wstring> dirs;

    // An explicit Directory= wins outright: someone who set it has a layout in
    // mind, and quietly loading from three other places as well would make the
    // setting a lie.
    if (const std::wstring custom = IniString(L"Directory"); !custom.empty()) {
        std::wstring d = custom;
        // Relative to the game directory unless it names a drive.
        if (d.size() < 2 || d[1] != L':') d = game + d;
        if (d.back() != L'\\') d += L'\\';
        dirs.push_back(d);
    } else {
        // scripts\ first: it is the convention every other ASI loader uses, so
        // a mod the user already has works with no instructions.
        dirs.push_back(game + L"scripts\\");
        dirs.push_back(game + L"MWOn12\\ASI\\");
    }

    std::vector<std::wstring> names;
    for (const auto& d : dirs) LoadFrom(d, names);

    if (g_loaded.empty()) {
        std::wstring where;
        for (const auto& d : dirs) { if (!where.empty()) where += L", "; where += d; }
        DXLOG_INFO("[asi] no .asi files found (looked in %ls)", where.c_str());
    } else {
        Logger::Log("MWOn12: %u ASI mod(s) loaded",
                    static_cast<unsigned>(g_loaded.size()));
    }
}

}  // namespace

void LoadAll()
{
    std::call_once(g_once, LoadAllOnce);
}

unsigned LoadedCount() noexcept
{
    return static_cast<unsigned>(g_loaded.size());
}

}  // namespace mwon12::asi
