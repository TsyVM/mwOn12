#pragma once

/**
 * vh/fwd.hpp
 * Forward declarations for the vh:: public API.
 * Include this for fast compilation when only types need to be named.
 */

namespace vh {

class Hook;
class Group;
class HookRegistry;

namespace config {
    struct Trampoline;
    struct API;
    struct IAT;
    struct PLT;
    struct VTable;
    struct Mid;
} // namespace config

} // namespace vh