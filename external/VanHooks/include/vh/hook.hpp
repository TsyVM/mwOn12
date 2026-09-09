#pragma once

/**
 * vh/hook.hpp
 * vh::Hook — Universal RAII hook object returned by every hook creation function.
 *
 * Move-only. Destructor calls remove(). Works with the opaque Engine type —
 * all Engine method calls are routed through the internal bridge (hook_impl.hpp)
 * which is compiled into the library and not shipped in the SDK.
 */

#include <string>
#include <cstddef>

#include <vanhooks/vanhooks.hpp>
#include "result.hpp"

namespace vh {

// Internal bridge — implemented in the library, not visible in the SDK headers.
// These free functions accept a raw HookHandle + Engine* and forward to the
// real Engine methods, keeping Hook independent of the complete Engine type.
namespace detail {
    Status      hook_enable  (vanhooks::Engine*, vanhooks::HookHandle) noexcept;
    Status      hook_disable (vanhooks::Engine*, vanhooks::HookHandle) noexcept;
    Status      hook_remove  (vanhooks::Engine*, vanhooks::HookHandle) noexcept;
    bool        hook_enabled (vanhooks::Engine*, vanhooks::HookHandle) noexcept;
    std::string hook_tag     (vanhooks::Engine*, vanhooks::HookHandle);
    vanhooks::HookKind hook_kind   (vanhooks::Engine*, vanhooks::HookHandle) noexcept;
    void*       hook_target  (vanhooks::Engine*, vanhooks::HookHandle) noexcept;
    void*       hook_detour  (vanhooks::Engine*, vanhooks::HookHandle) noexcept;
    void*       hook_trampoline(vanhooks::Engine*, vanhooks::HookHandle) noexcept;
    vanhooks::Result<vanhooks::HookHandle> hook_chain(
        vanhooks::Engine*, vanhooks::HookHandle,
        void* next_detour, void** next_original_out, std::string tag);
} // namespace detail

class Hook {
public:
    // ── Construction (internal — users call vh::hook() etc.) ──────────────────

    Hook(vanhooks::Engine& engine, vanhooks::HookHandle handle) noexcept
        : engine_(&engine), handle_(handle) {}

    // ── Move-only ─────────────────────────────────────────────────────────────

    Hook(Hook&& other) noexcept
        : engine_(other.engine_), handle_(other.handle_)
    {
        other.handle_ = {};
    }

    Hook& operator=(Hook&& other) noexcept {
        if (this != &other) {
            do_remove();
            engine_ = other.engine_;
            handle_ = other.handle_;
            other.handle_ = {};
        }
        return *this;
    }

    Hook(const Hook&)            = delete;
    Hook& operator=(const Hook&) = delete;

    ~Hook() { do_remove(); }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    Hook& enable() {
        if (handle_.valid()) detail::hook_enable(engine_, handle_);
        return *this;
    }

    Hook& disable() {
        if (handle_.valid()) detail::hook_disable(engine_, handle_);
        return *this;
    }

    void remove() { do_remove(); }

    // ── Chaining ──────────────────────────────────────────────────────────────

    Result<Hook> chain(void* next_detour, void** next_original_out = nullptr,
                       std::string tag = {}) {
        if (!handle_.valid())
            return std::unexpected(vanhooks::Error::HookNotFound);
        return detail::hook_chain(engine_, handle_, next_detour, next_original_out,
                                  std::move(tag))
            .transform([this](vanhooks::HookHandle h) { return Hook(*engine_, h); });
    }

    template<typename Fn>
    Result<Hook> chain(Fn* next_detour, Fn** next_original_out = nullptr,
                       std::string tag = {}) {
        return chain(reinterpret_cast<void*>(next_detour),
                     reinterpret_cast<void**>(next_original_out),
                     std::move(tag));
    }

    // ── Introspection ─────────────────────────────────────────────────────────

    bool valid()              const noexcept { return handle_.valid(); }
    explicit operator bool()  const noexcept { return valid(); }

    bool        enabled()    const noexcept { return handle_.valid() && detail::hook_enabled(engine_, handle_); }
    std::string tag()        const          { return handle_.valid() ? detail::hook_tag(engine_, handle_) : std::string{}; }
    vanhooks::HookKind kind() const noexcept { return detail::hook_kind(engine_, handle_); }
    void*       target()     const noexcept { return handle_.valid() ? detail::hook_target(engine_, handle_)     : nullptr; }
    void*       detour()     const noexcept { return handle_.valid() ? detail::hook_detour(engine_, handle_)     : nullptr; }
    void*       trampoline() const noexcept { return handle_.valid() ? detail::hook_trampoline(engine_, handle_) : nullptr; }

    // Escape hatches for Engine interop (power users only).
    vanhooks::HookHandle  handle() const noexcept { return handle_; }
    vanhooks::Engine&     engine() const noexcept { return *engine_; }

private:
    vanhooks::Engine*    engine_ = nullptr;
    vanhooks::HookHandle handle_ = {};

    void do_remove() noexcept {
        if (handle_.valid() && engine_) {
            detail::hook_remove(engine_, handle_);
            handle_ = {};
        }
    }
};

} // namespace vh
