#pragma once

/**
 * vh/group.hpp
 * vh::Group  — RAII container owning Hook objects with batch lifecycle management.
 * vh::HookRegistry — Process-wide singleton for named group lookup across modules.
 */

#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <mutex>
#include <unordered_map>

#include <vanhooks/vanhooks.hpp>
#include "result.hpp"
#include "hook.hpp"

namespace vh {

// Internal bridge declarations.
namespace detail {
    void   engine_queue_enable (vanhooks::Engine*, vanhooks::HookHandle) ;
    void   engine_queue_disable(vanhooks::Engine*, vanhooks::HookHandle);
    void   engine_queue_remove (vanhooks::Engine*, vanhooks::HookHandle);
    Status engine_apply_queued (vanhooks::Engine*);
} // namespace detail

// ─────────────────────────────────────────────
//  Group
// ─────────────────────────────────────────────

class Group {
public:
    explicit Group(std::string name = {}) : name_(std::move(name)) {}

    Group(Group&&) noexcept = default;
    Group& operator=(Group&&) noexcept = default;
    Group(const Group&)  = delete;
    Group& operator=(const Group&) = delete;

    ~Group() { remove_all(); }

    // ── Add hooks ─────────────────────────────────────────────────────────────

    Group& add(Hook hook) {
        hooks_.push_back(std::move(hook));
        return *this;
    }

    Group& add(Result<Hook> r) {
        if (r.has_value()) hooks_.push_back(std::move(*r));
        return *this;
    }

    // ── Batch lifecycle ───────────────────────────────────────────────────────

    Group& enable() {
        if (hooks_.empty()) return *this;
        auto* e = engine_ptr();
        for (auto& h : hooks_) if (h.valid()) detail::engine_queue_enable(e, h.handle());
        detail::engine_apply_queued(e);
        return *this;
    }

    Group& disable() {
        if (hooks_.empty()) return *this;
        auto* e = engine_ptr();
        for (auto& h : hooks_) if (h.valid()) detail::engine_queue_disable(e, h.handle());
        detail::engine_apply_queued(e);
        return *this;
    }

    void remove_all() {
        if (hooks_.empty()) return;
        auto* e = engine_ptr();
        for (auto& h : hooks_) if (h.valid()) detail::engine_queue_remove(e, h.handle());
        detail::engine_apply_queued(e);
        hooks_.clear();
    }

    void clear() { remove_all(); }

    // ── Queued batch ──────────────────────────────────────────────────────────

    Group& queue_enable() {
        auto* e = engine_ptr();
        for (auto& h : hooks_) if (h.valid()) detail::engine_queue_enable(e, h.handle());
        return *this;
    }

    Group& queue_disable() {
        auto* e = engine_ptr();
        for (auto& h : hooks_) if (h.valid()) detail::engine_queue_disable(e, h.handle());
        return *this;
    }

    Status apply() {
        if (hooks_.empty()) return {};
        return detail::engine_apply_queued(engine_ptr());
    }

    // ── Introspection ─────────────────────────────────────────────────────────

    size_t      size()  const noexcept { return hooks_.size(); }
    bool        empty() const noexcept { return hooks_.empty(); }
    std::string name()  const          { return name_; }

    Hook& operator[](size_t i)          { return hooks_.at(i); }
    const Hook& operator[](size_t i) const { return hooks_.at(i); }

    Hook& at(std::string_view tag) {
        for (auto& h : hooks_)
            if (h.tag() == tag) return h;
        throw std::out_of_range(std::string("vh::Group::at: tag not found: ") + std::string(tag));
    }

    // ── Iterator support ──────────────────────────────────────────────────────

    auto begin()        { return hooks_.begin(); }
    auto end()          { return hooks_.end(); }
    auto begin()  const { return hooks_.begin(); }
    auto end()    const { return hooks_.end(); }

private:
    std::string       name_;
    std::vector<Hook> hooks_;

    vanhooks::Engine* engine_ptr() {
        for (auto& h : hooks_)
            if (h.valid()) return &h.engine();
        return &vanhooks::global_engine();
    }
};

// ─────────────────────────────────────────────
//  HookRegistry
// ─────────────────────────────────────────────

class HookRegistry {
public:
    static HookRegistry& global() {
        static HookRegistry instance;
        return instance;
    }

    void register_group(Group group) {
        std::lock_guard lock(mutex_);
        std::string key = group.name();
        groups_.emplace(std::move(key), std::move(group));
    }

    Group* find_group(std::string_view name) {
        std::lock_guard lock(mutex_);
        auto it = groups_.find(std::string(name));
        return it != groups_.end() ? &it->second : nullptr;
    }

    void enable_all()  { std::lock_guard lock(mutex_); for (auto& [k,g] : groups_) g.enable();     }
    void disable_all() { std::lock_guard lock(mutex_); for (auto& [k,g] : groups_) g.disable();    }
    void remove_all()  { std::lock_guard lock(mutex_); for (auto& [k,g] : groups_) g.remove_all(); groups_.clear(); }

    size_t total_hook_count() const {
        std::lock_guard lock(mutex_);
        size_t n = 0;
        for (const auto& [k,g] : groups_) n += g.size();
        return n;
    }

    ~HookRegistry() { remove_all(); }

private:
    HookRegistry() = default;
    mutable std::mutex                     mutex_;
    std::unordered_map<std::string, Group> groups_;
};

} // namespace vh
