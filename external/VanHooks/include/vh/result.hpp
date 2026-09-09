#pragma once

/**
 * vh/result.hpp
 * Result<T> and Status type aliases for the vh:: API.
 * These are direct re-exports of the vanhooks:: types — no new types, no overhead.
 */

#include <vanhooks/vanhooks.hpp>

namespace vh {

using vanhooks::Error;
using vanhooks::error_to_string;

template<typename T>
using Result = vanhooks::Result<T>;

using Status = vanhooks::Status;

} // namespace vh
