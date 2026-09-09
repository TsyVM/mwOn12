#pragma once

/**
 * vh/advanced.hpp
 * Direct Engine access for power users.
 *
 * vh::advanced::engine() returns a reference to the global Engine.
 * The Engine type is opaque in the SDK — you can pass it to Engine interop
 * APIs but cannot construct or configure one directly from the SDK.
 *
 * If you need a custom allocator, independent engine instances, or full
 * Engine::Config control, contact us about a source license.
 */

#include <vanhooks/vanhooks.hpp>

namespace vh::advanced {

using Engine = vanhooks::Engine;

// Access the process-wide global engine.
inline Engine& engine() {
    return vanhooks::global_engine();
}

} // namespace vh::advanced
