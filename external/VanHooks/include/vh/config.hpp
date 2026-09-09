#pragma once

/**
 * vh/config.hpp
 * Simplified, discoverable configuration structs under vh::config::.
 *
 * User-facing configuration structs for the vh:: API.
 */

#include <string>
#include <string_view>
#include <cstddef>

#include <vanhooks/vanhooks.hpp>

namespace vh::config {

// ─────────────────────────────────────────────
//  Trampoline (inline patch) options
// ─────────────────────────────────────────────

struct Trampoline {
    bool        thread_safe = true;  // suspend threads during patch (recommended)
    std::string tag;
};

// ─────────────────────────────────────────────
//  API hook options  (hook_api / api_hook)
//  Resolves target via GetModuleHandle + GetProcAddress (Windows) or dlsym (POSIX).
// ─────────────────────────────────────────────

struct API {
    std::string_view module_name;    // e.g. "user32" or "user32.dll"
    std::string_view function_name;  // e.g. "MessageBoxW"
    bool             thread_safe = true;
    std::string      tag;
};

// ─────────────────────────────────────────────
//  IAT hook options  (Windows import table)
// ─────────────────────────────────────────────

struct IAT {
    std::string_view module_name;    // module containing the IAT (empty = all loaded)
    std::string_view import_name;    // symbol name to redirect
    std::string      tag;
};

// ─────────────────────────────────────────────
//  PLT / GOT hook options  (Linux / macOS)
// ─────────────────────────────────────────────

struct PLT {
    std::string_view library_name;   // shared object name, e.g. "libc"
    std::string_view symbol_name;    // symbol to redirect, e.g. "malloc"
    std::string      tag;
};

// ─────────────────────────────────────────────
//  VTable hook options
// ─────────────────────────────────────────────

struct VTable {
    void**      vtable     = nullptr;
    size_t      slot       = 0;      // which slot to patch (was slot_index)
    std::string tag;
};

// ─────────────────────────────────────────────
//  Mid-function hook options
// ─────────────────────────────────────────────

struct Mid {
    size_t      offset      = 0;     // byte offset into target function
    bool        thread_safe = true;
    std::string tag;
};

} // namespace vh::config
