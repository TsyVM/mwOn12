#pragma once

/**
 * VanHooks — Modern C++23 Cross-Platform Function Hooking Library
 * https://github.com/your-org/vanhooks
 *
 * This is the public SDK header. It exposes only the types needed to use
 * the vh:: API. The Engine implementation is opaque — use <vh/vh.hpp>.
 */

#include <cstdint>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace vanhooks {

// ─────────────────────────────────────────────
//  Version
// ─────────────────────────────────────────────

inline constexpr int             VERSION_MAJOR  = 0;
inline constexpr int             VERSION_MINOR  = 1;
inline constexpr int             VERSION_PATCH  = 0;
inline constexpr std::string_view VERSION_STRING = "0.1.0";

// ─────────────────────────────────────────────
//  Platform detection
// ─────────────────────────────────────────────

#if defined(_WIN32) || defined(_WIN64)
    #define VH_PLATFORM_WINDOWS 1
#elif defined(__linux__)
    #define VH_PLATFORM_LINUX 1
#elif defined(__APPLE__)
    #define VH_PLATFORM_MACOS 1
#endif

#if defined(__x86_64__) || defined(_M_X64)
    #define VH_ARCH_X64 1
#elif defined(__i386__) || defined(_M_IX86)
    #define VH_ARCH_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define VH_ARCH_ARM64 1
#endif

// ─────────────────────────────────────────────
//  Error codes
// ─────────────────────────────────────────────

enum class Error : uint32_t {
    Ok                      = 0,
    NotInitialized          = 1,
    AlreadyInitialized      = 2,
    InvalidArgument         = 3,
    Unsupported             = 4,
    OutOfRange              = 5,
    OsError                 = 6,

    MemoryAllocationFailed  = 100,
    MemoryProtectFailed     = 101,
    MemoryReadFailed        = 102,
    MemoryWriteFailed       = 103,
    TrampolineNoSpace       = 104,

    HookNotFound            = 200,
    HookAlreadyExists       = 201,
    HookInstallFailed       = 202,
    HookRemoveFailed        = 203,
    HookAlreadyEnabled      = 204,
    HookAlreadyDisabled     = 205,

    DisassemblyFailed       = 300,
    InstructionTooShort     = 301,
    UnsupportedInstruction  = 302,

    ModuleNotFound          = 400,
    SymbolNotFound          = 401,
    IATEntryNotFound        = 402,
    PLTEntryNotFound        = 403,

    ThreadSuspendFailed     = 500,
    ThreadResumeFailed      = 501,
    ThreadInPrologue        = 502,

    QueueEmpty              = 600,
    ChainBaseNotFound       = 601,
    ChainOrderViolation     = 602,
};

std::string_view error_to_string(Error e) noexcept;

template<typename T>
using Result = std::expected<T, Error>;

using Status = Result<void>;

// ─────────────────────────────────────────────
//  Hook handle (opaque value type)
// ─────────────────────────────────────────────

struct HookHandle {
    uint64_t id = 0;
    bool valid()              const noexcept { return id != 0; }
    explicit operator bool()  const noexcept { return valid(); }
};

inline bool operator==(HookHandle a, HookHandle b) noexcept { return a.id == b.id; }

// ─────────────────────────────────────────────
//  Hook kind
// ─────────────────────────────────────────────

enum class HookKind : uint8_t {
    Trampoline,
    IAT,
    PLT,
    VTable,
    Mid,
};

// ─────────────────────────────────────────────
//  Hook descriptor (introspection)
// ─────────────────────────────────────────────

struct HookDesc {
    HookHandle  handle;
    HookKind    kind;
    void*       target;
    void*       detour;
    void*       trampoline;   // nullptr for non-trampoline kinds
    bool        enabled;
    std::string tag;
};

// ─────────────────────────────────────────────
//  Mid-function hook register context
// ─────────────────────────────────────────────

struct alignas(8) MidContext {
#if defined(VH_ARCH_X64)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t rflags;
#elif defined(VH_ARCH_ARM64)
    uint64_t x[8];
    uint64_t lr;
    uint64_t _pad;
#elif defined(VH_ARCH_X86)
    uint32_t edi, esi, ebp, ebx, edx, ecx, eax;
    uint32_t eflags;
#endif
};

using MidCallback = void (*)(MidContext*) noexcept;

// ─────────────────────────────────────────────
//  Engine (opaque — use vh:: free functions)
//
//  The Engine class is forward-declared only.
//  Its methods, Config, and implementation details
//  are not part of the public SDK surface.
//  Access the global engine via vanhooks::global_engine()
//  if you need direct Engine interop; otherwise use <vh/vh.hpp>.
// ─────────────────────────────────────────────

class Engine;

Engine& global_engine();

} // namespace vanhooks

namespace vhook = vanhooks;
