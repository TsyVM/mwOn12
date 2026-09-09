#pragma once

/**
 * vh/vh.hpp — VanHooks public API mega-include.
 *
 * Three-level progressive API:
 *
 *   Level 1 — vh::hook(target, detour)
 *   Level 2 — vh::inline_hook / api_hook / vtable_hook / iat_hook / plt_hook / mid_hook
 *   Level 3 — vh::group("Name")  →  Group;  vh::HookRegistry::global()
 *
 * #include <vh/vh.hpp>  is the only include end users need.
 */

#include "fwd.hpp"
#include "result.hpp"
#include "mid.hpp"
#include "config.hpp"
#include "hook.hpp"
#include "group.hpp"
#include "advanced.hpp"

#include <vanhooks/vanhooks.hpp>

#include <string_view>
#include <vector>

namespace vh {

using vanhooks::HookHandle;
using vanhooks::HookKind;
using vanhooks::HookDesc;
using vanhooks::Engine;

// ── Internal bridge declarations (compiled into the library) ──────────────────

namespace detail {
    // Trampoline
    Result<HookHandle> eng_hook_trampoline(Engine*, void* target, void* detour,
                                           void** orig, bool thread_safe, std::string tag);
    // API (by name)
    Result<HookHandle> eng_hook_api(Engine*, std::string_view module, std::string_view symbol,
                                    void* detour, void** orig, bool thread_safe, std::string tag);
    // IAT
    Result<HookHandle> eng_hook_iat(Engine*, std::string_view module, std::string_view import_name,
                                    void* detour, std::string tag);
    Result<std::vector<HookHandle>> eng_hook_iat_all(Engine*, std::string_view module,
                                                      std::string_view import_name,
                                                      void* detour, std::string tag);
    // PLT
    Result<HookHandle> eng_hook_plt(Engine*, std::string_view library, std::string_view symbol,
                                    void* detour, std::string tag);
    // VTable
    Result<HookHandle> eng_hook_vtable(Engine*, void** vtable, size_t slot,
                                       void* detour, std::string tag);
    // Mid
    Result<HookHandle> eng_hook_mid(Engine*, void* target, size_t offset,
                                    vanhooks::MidCallback cb, bool thread_safe, std::string tag);
} // namespace detail

// ═════════════════════════════════════════════
//  Level 1 — Beginner API
// ═════════════════════════════════════════════

template<typename Fn>
[[nodiscard]] Result<Hook> hook(Fn* target, Fn* detour) {
    auto& eng = vanhooks::global_engine();
    return detail::eng_hook_trampoline(&eng,
        reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(detour),
        nullptr, true, {})
        .transform([&eng](HookHandle h) { return Hook(eng, h); });
}

template<typename Fn>
[[nodiscard]] Result<Hook> hook(Fn* target, Fn* detour, Fn** original_out) {
    auto& eng = vanhooks::global_engine();
    return detail::eng_hook_trampoline(&eng,
        reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(detour),
        reinterpret_cast<void**>(original_out), true, {})
        .transform([&eng](HookHandle h) { return Hook(eng, h); });
}

template<typename Fn>
[[nodiscard]] Result<Hook> hook(std::string_view module, std::string_view symbol,
                                Fn* detour, Fn** original_out = nullptr) {
    auto& eng = vanhooks::global_engine();
    return detail::eng_hook_api(&eng, module, symbol,
        reinterpret_cast<void*>(detour),
        reinterpret_cast<void**>(original_out), true, {})
        .transform([&eng](HookHandle h) { return Hook(eng, h); });
}

// ═════════════════════════════════════════════
//  Level 2 — Explicit Hook Type API
// ═════════════════════════════════════════════

template<typename Fn>
[[nodiscard]] Result<Hook> inline_hook(Fn* target, Fn* detour,
                                       Fn** original_out = nullptr,
                                       config::Trampoline opts = {}) {
    auto& eng = vanhooks::global_engine();
    return detail::eng_hook_trampoline(&eng,
        reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(detour),
        reinterpret_cast<void**>(original_out),
        opts.thread_safe, opts.tag)
        .transform([&eng](HookHandle h) { return Hook(eng, h); });
}

template<typename Fn>
[[nodiscard]] Result<Hook> api_hook(std::string_view module, std::string_view symbol,
                                    Fn* detour, Fn** original_out = nullptr,
                                    config::Trampoline opts = {}) {
    auto& eng = vanhooks::global_engine();
    return detail::eng_hook_api(&eng, module, symbol,
        reinterpret_cast<void*>(detour),
        reinterpret_cast<void**>(original_out),
        opts.thread_safe, opts.tag)
        .transform([&eng](HookHandle h) { return Hook(eng, h); });
}

[[nodiscard]] inline Result<Hook> vtable_hook(void** vtable, size_t slot,
                                              void* detour,
                                              void** /*original_out*/ = nullptr,
                                              config::VTable opts = {}) {
    auto& eng = vanhooks::global_engine();
    return detail::eng_hook_vtable(&eng, vtable, slot, detour, opts.tag)
        .transform([&eng](HookHandle h) { return Hook(eng, h); });
}

[[nodiscard]] inline Result<Hook> iat_hook(std::string_view import_name,
                                           void* detour,
                                           config::IAT opts = {}) {
    auto& eng = vanhooks::global_engine();
    return detail::eng_hook_iat(&eng, opts.module_name, import_name, detour, opts.tag)
        .transform([&eng](HookHandle h) { return Hook(eng, h); });
}

[[nodiscard]] inline Result<std::vector<Hook>> iat_hook_all(std::string_view import_name,
                                                             void* detour,
                                                             config::IAT opts = {}) {
    auto& eng = vanhooks::global_engine();
    return detail::eng_hook_iat_all(&eng, opts.module_name, import_name, detour, opts.tag)
        .transform([&eng](std::vector<HookHandle> handles) {
            std::vector<Hook> hooks;
            hooks.reserve(handles.size());
            for (auto& h : handles) hooks.emplace_back(eng, h);
            return hooks;
        });
}

[[nodiscard]] inline Result<Hook> plt_hook(std::string_view library,
                                           std::string_view symbol,
                                           void* detour,
                                           config::PLT opts = {}) {
    auto& eng = vanhooks::global_engine();
    return detail::eng_hook_plt(&eng, library, symbol, detour, opts.tag)
        .transform([&eng](HookHandle h) { return Hook(eng, h); });
}

[[nodiscard]] inline Result<Hook> mid_hook(void* target,
                                           vanhooks::MidCallback callback,
                                           config::Mid opts = {}) {
    auto& eng = vanhooks::global_engine();
    return detail::eng_hook_mid(&eng, target, opts.offset, callback,
                                opts.thread_safe, opts.tag)
        .transform([&eng](HookHandle h) { return Hook(eng, h); });
}

// ═════════════════════════════════════════════
//  Level 3 — Group factory
// ═════════════════════════════════════════════

[[nodiscard]] inline Group group(std::string_view name = "") {
    return Group(std::string(name));
}

} // namespace vh
