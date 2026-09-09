// User shader replacement — the ShaderDump / ShaderMods feature.
//
// Two directories beside the executable:
//
//   MWOn12\Shaders\          user-edited .hlsl files, loaded in place of ours
//   MWOn12\Shaders\Dumped\   what MWOn12 generated, written for reference
//
// They are separate so that both keys can be left on permanently, which is the
// intended way to use this: play to populate Dumped, copy a file up one level,
// edit it, restart. Dumping only ever writes the generated original, and only
// into Dumped, so it can never overwrite the user's work.
//
// A mod is matched to a shader by hashing the game's own D3D9 bytecode, not
// anything backend-specific, so a shader keeps the same name across builds and
// across machines. Fixed-function shaders have no bytecode to hash, so their
// permutation key is hashed instead.
//
// Naming carries the pixel-shader variant (alpha test, fog, sampler kinds) as
// a suffix. Load falls back from the exact variant name to the un-suffixed
// name, which lets a user drop the suffix from a filename to have one edit
// apply to every variant of that shader — usually what they want, since the
// variants differ only in state folded in after the shader body.
//
// Nothing here is on a hot path: both entry points are reached only when a
// shader is first compiled, which is once per shader per run.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/ShaderMods.h>
#include <core/BackendSelect.h>
#include <core/Log.h>

#include <synchapi.h>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mwon12 {
namespace shadermods {

namespace {

// Shaders are compiled from whichever thread first needs them, and the game is
// not single-threaded, so all shared state sits behind one lock. It is only
// ever held for map and string operations, never across file I/O.
SRWLOCK g_lock = SRWLOCK_INIT;

bool g_pathsReady = false;
std::wstring g_modDir;
std::wstring g_dumpDir;

// Filenames already written this run, so a shader compiled in several variants
// does not rewrite its dump repeatedly.
std::unordered_set<std::wstring>      g_dumped;
// Memoised lookup result per shader name: 1 found, 0 known absent. Saves
// hitting the filesystem again for a shader we have already looked for.
std::unordered_map<std::string, int>  g_lookup;
// Mods that failed to compile. Once a file is in here it is never offered
// again, so a broken edit costs exactly one failed compile rather than one per
// variant.
std::unordered_set<std::string>       g_failed;

std::wstring ModuleDir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &self);

    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return std::wstring();

    std::wstring s(path, n);
    const size_t cut = s.find_last_of(L"\\/");
    return (cut == std::wstring::npos) ? std::wstring() : s.substr(0, cut + 1);
}

void EnsurePaths()
{
    if (g_pathsReady) return;
    g_pathsReady = true;

    const std::wstring base = ModuleDir();
    if (base.empty()) return;

    g_modDir  = base + L"MWOn12\\Shaders\\";
    g_dumpDir = g_modDir + L"Dumped\\";
}

// CreateDirectory only creates one level, so walk the path and create each
// component. The size check skips "C:\", which cannot be created and whose
// failure would otherwise be indistinguishable from a real one. Existing
// directories fail harmlessly; the attribute check at the end is what decides
// whether this worked.
bool MakeTree(const std::wstring& dir)
{
    if (dir.empty()) return false;

    std::wstring acc;
    for (size_t i = 0; i < dir.size(); ++i) {
        acc.push_back(dir[i]);
        if (dir[i] == L'\\' || dir[i] == L'/') {
            if (acc.size() > 3)
                CreateDirectoryW(acc.c_str(), nullptr);
        }
    }
    const DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// FNV-1a with a final avalanche mix. The mix matters: raw FNV-1a leaves weak
// diffusion in the high bits, and these values are printed as filenames, so
// two similar shaders should not produce names differing in one character.
// This is not a cryptographic hash and does not need to be — a collision
// serves the wrong mod, it does not corrupt anything.
uint64_t HashBytes(const void* data, size_t len) noexcept
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return h;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

bool ReadWholeFile(const std::wstring& path, std::string* out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (16 << 20)) {
        CloseHandle(h);
        return false;
    }

    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const BOOL ok = ReadFile(h, out->data(), static_cast<DWORD>(out->size()),
                             &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out->resize(got);

    // Strip a UTF-8 byte order mark. Notepad and several editors add one on
    // save, and D3DCompile treats it as stray characters before the first
    // token — which presents to the user as their untouched shader suddenly
    // failing to compile.
    if (out->size() >= 3 &&
        static_cast<uint8_t>((*out)[0]) == 0xEF &&
        static_cast<uint8_t>((*out)[1]) == 0xBB &&
        static_cast<uint8_t>((*out)[2]) == 0xBF)
        out->erase(0, 3);

    return true;
}

bool WriteWholeFile(const std::wstring& path, const std::string& text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD put = 0;
    const BOOL ok = WriteFile(h, text.data(),
                              static_cast<DWORD>(text.size()), &put, nullptr);
    CloseHandle(h);
    return ok && put == text.size();
}

}

bool Enabled() noexcept
{
    return BackendSelect::ShaderModsEnabled();
}

bool DumpEnabled() noexcept
{
    return BackendSelect::ShaderDumpEnabled();
}

bool Active() noexcept
{
    return Enabled() || DumpEnabled();
}

// Identifies a translated shader. The hash is over the game's own D3D9
// bytecode, which is what makes a mod file portable between the two backends —
// neither the translated HLSL nor the compiled DXBC would be.
//
// texKindMask is folded in for pixel shaders only. A vertex shader's variant
// key would otherwise change with unrelated pixel-stage state and rename its
// file mid-session.
Id MakeId(const void* d3d9Bytecode, size_t byteLen, bool isVertex,
          unsigned alphaFunc, unsigned fogMode, unsigned texKindMask) noexcept
{
    Id id{};
    id.isVertex      = isVertex;
    id.fixedFunction = false;
    id.source        = (d3d9Bytecode && byteLen)
                     ? HashBytes(d3d9Bytecode, byteLen) : 0ull;
    id.variant       = (alphaFunc & 0xFu)
                     | ((fogMode & 0x7u) << 4)
                     | ((isVertex ? 0u : (texKindMask & 0xFFFFu)) << 7);
    return id;
}

Id MakeFixedFunctionId(const void* permKey, size_t keyLen, bool isVertex) noexcept
{
    Id id{};
    id.isVertex      = isVertex;
    id.fixedFunction = true;
    id.source        = (permKey && keyLen) ? HashBytes(permKey, keyLen) : 0ull;
    id.variant       = 0;
    return id;
}

std::string Name(const Id& id) noexcept
{
    char buf[64];
    const char* stem = id.fixedFunction ? (id.isVertex ? "ffpvs" : "ffpps")
                                        : (id.isVertex ? "vs"    : "ps");
    if (id.variant != 0)
        std::snprintf(buf, sizeof(buf), "%s_%016llx_v%08x", stem,
                      static_cast<unsigned long long>(id.source), id.variant);
    else
        std::snprintf(buf, sizeof(buf), "%s_%016llx", stem,
                      static_cast<unsigned long long>(id.source));
    return std::string(buf);
}

void Dump(const Id& id, const std::string& hlsl) noexcept
{
    if (!DumpEnabled() || hlsl.empty())
        return;

    const std::string name = Name(id);
    const std::wstring file = Widen(name) + L".hlsl";

    AcquireSRWLockExclusive(&g_lock);
    EnsurePaths();
    const std::wstring dir = g_dumpDir;
    const bool first = dir.empty() ? false : g_dumped.insert(file).second;
    ReleaseSRWLockExclusive(&g_lock);

    if (!first)
        return;

    if (!MakeTree(dir)) {
        DXLOG_WARN("[shadermods] could not create the dump directory - "
                   "ShaderDump is on but nothing can be written");
        return;
    }

    if (WriteWholeFile(dir + file, hlsl))
        DXLOG_INFO("[shadermods] dumped %s.hlsl", name.c_str());
    else
        DXLOG_WARN("[shadermods] failed to write %s.hlsl", name.c_str());
}

// Looks for a user replacement, exact variant first and un-suffixed second.
//
// Both the memoised lookup and the failed set are read under the lock and
// copied out, because the file I/O below must not hold it — a slow disk would
// otherwise stall every thread compiling a shader.
bool Load(const Id& id, std::string* pHlslOut, std::string* pNameOut) noexcept
{
    if (!Enabled() || !pHlslOut)
        return false;

    const std::string exact = Name(id);

    Id base = id;
    base.variant = 0;
    const std::string generic = Name(base);

    AcquireSRWLockExclusive(&g_lock);
    EnsurePaths();
    const std::wstring dir = g_modDir;
    const auto cached = g_lookup.find(exact);
    const int known = (cached == g_lookup.end()) ? -1 : cached->second;
    const bool exactFailed   = g_failed.count(exact)   != 0;
    const bool genericFailed = g_failed.count(generic) != 0;
    ReleaseSRWLockExclusive(&g_lock);

    if (dir.empty() || known == 0)
        return false;

    const std::string* tried[2] = { &exact, &generic };
    const bool skip[2] = { exactFailed, genericFailed };
    const size_t count = (exact == generic) ? 1u : 2u;

    for (size_t i = 0; i < count; ++i) {
        if (skip[i])
            continue;
        const std::string& stem = *tried[i];

        std::string text;
        if (!ReadWholeFile(dir + Widen(stem) + L".hlsl", &text) || text.empty())
            continue;

        AcquireSRWLockExclusive(&g_lock);
        g_lookup[exact] = 1;
        ReleaseSRWLockExclusive(&g_lock);

        *pHlslOut = std::move(text);
        if (pNameOut) *pNameOut = stem;
        return true;
    }

    AcquireSRWLockExclusive(&g_lock);
    g_lookup[exact] = 0;
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

// Called when a loaded mod failed to compile. Blacklists it and drops the
// memoised lookups, so a shader whose exact-variant file just failed gets to
// re-evaluate and fall through to the un-suffixed file on its next request.
// Logged once per file — a broken mod would otherwise print on every variant.
void ReportCompileFailure(const std::string& name) noexcept
{
    bool first = false;
    AcquireSRWLockExclusive(&g_lock);
    first = g_failed.insert(name).second;
    g_lookup.clear();
    ReleaseSRWLockExclusive(&g_lock);

    if (first)
        DXLOG_ERROR("[shadermods] %s.hlsl failed to compile - the original "
                    "shader is being used instead; see the D3DCompile errors "
                    "above for the line number", name.c_str());
}

}
}
