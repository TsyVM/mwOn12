// User shader replacement -- the ShaderDump and ShaderMods settings.
//
// With dumping enabled, every shader the renderer generates is written out as
// HLSL. With mods enabled, an edited file matching a shader is compiled in its
// place. The two are meant to be used together: play to populate the dump
// directory, copy a file out, edit it, restart.
//
// A shader is identified by hashing the game's own D3D9 bytecode rather than
// anything backend-specific, so one edited file works under both renderers.
// Fixed-function shaders have no bytecode, so their permutation key is hashed
// instead.
//
// A mod that fails to compile is reported once and the original is used, so a
// mistake in an edited shader can never stop the game running.

#pragma once

#ifndef MWON12_SHADER_MODS_H
#define MWON12_SHADER_MODS_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace mwon12 {
namespace shadermods {

struct Id {
    uint64_t source        = 0;
    uint32_t variant       = 0;
    bool     isVertex      = false;
    bool     fixedFunction = false;
};

[[nodiscard]] bool Enabled() noexcept;
[[nodiscard]] bool DumpEnabled() noexcept;

[[nodiscard]] bool Active() noexcept;

[[nodiscard]] Id MakeId(const void* d3d9Bytecode,
                        size_t       byteLen,
                        bool         isVertex,
                        unsigned     alphaFunc,
                        unsigned     fogMode,
                        unsigned     texKindMask) noexcept;

[[nodiscard]] Id MakeFixedFunctionId(const void* permKey,
                                     size_t      keyLen,
                                     bool        isVertex) noexcept;

[[nodiscard]] std::string Name(const Id& id) noexcept;

void Dump(const Id& id, const std::string& hlsl) noexcept;

[[nodiscard]] bool Load(const Id&    id,
                        std::string* pHlslOut,
                        std::string* pNameOut) noexcept;

void ReportCompileFailure(const std::string& name) noexcept;

}
}

#endif
