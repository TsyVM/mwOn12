// Caches translated and compiled shaders for the D3D12 backend.
//
// Uses the same translator as the D3D11 backend, so the two produce identical
// HLSL from the same game shader, and compiles it for the D3D12 targets.
//
// Keys combine a hash of the original bytecode with the variant state that has
// to be compiled into a pixel shader -- alpha test, fog, and the kind of
// texture bound to each sampler -- since those were render states in D3D9 with
// no equivalent here.
//
// Unlike the D3D11 backend, there is no on-disk cache, so every shader is
// recompiled at each launch. That is a real and measurable part of this
// backend's load time and is worth addressing.
//
// The reflected input signature is kept with each compiled shader because the
// pipeline state object needs it to build an input layout, and reflecting on
// demand at draw time would be far too slow.

#pragma once

#ifndef MWON12_DX12_SHADERCACHE12_H
#define MWON12_DX12_SHADERCACHE12_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <synchapi.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mwon12 {

inline constexpr UINT kZeroStreamSlot12 = 16;

struct CompiledShader12 {
    std::vector<uint8_t> dxbc;

    struct SigInput {
        std::string name;
        UINT        index{ 0 };
    };
    std::vector<SigInput> inputSignature;

    uint32_t outTexMask{ 0 };
    bool     outColor0{ false };
    bool     outColor1{ false };
    bool     outFog{ false };

    uint32_t samplerMask{ 0 };
    uint8_t  samplerTypes[16]{};
    bool     usesVPos{ false };
    bool     usesVFace{ false };
    bool     writesDepth{ false };
    bool     writesOC1Plus{ false };
    bool     isVertexShader{ false };
};

class ShaderCache12 {
public:

    HRESULT GetOrCompile(const DWORD* bytecode, size_t byteLen, bool isVertexShader,
                         uint32_t alphaFunc, uint32_t fogMode, uint32_t texKindMask,
                         const CompiledShader12** ppOut) noexcept;

    HRESULT GetDiagnosticShader(bool isVertexShader,
                                const CompiledShader12** ppOut) noexcept;

    [[nodiscard]] size_t EntryCount() const noexcept;

private:
    struct Key {
        uint64_t bytecodeHash;
        uint32_t variant;
        uint32_t texKindMask;
        bool operator==(const Key& o) const noexcept
        {
            return bytecodeHash == o.bytecodeHash && variant == o.variant &&
                   texKindMask == o.texKindMask;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept
        {
            uint64_t h = k.bytecodeHash ^ (uint64_t(k.variant) * 0x9E3779B97F4A7C15ull);
            h ^= uint64_t(k.texKindMask) * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
            return static_cast<size_t>(h);
        }
    };

    HRESULT CompileHlsl(const std::string& hlsl, bool isVertexShader,
                        uint32_t alphaFunc, uint32_t fogMode, uint32_t texKindMask,
                        std::vector<uint8_t>* pDxbc) noexcept;
    static HRESULT ReflectSignatures(CompiledShader12& out) noexcept;

    mutable SRWLOCK m_lock = SRWLOCK_INIT;
    std::unordered_map<Key, std::unique_ptr<CompiledShader12>, KeyHash> m_cache;
    std::unique_ptr<CompiledShader12> m_diagVS;
    std::unique_ptr<CompiledShader12> m_diagPS;
};

[[nodiscard]] DXGI_FORMAT DeclTypeToFormat12(BYTE declType) noexcept;

[[nodiscard]] const char* DeclUsageToSemantic12(BYTE usage) noexcept;

HRESULT BuildInputLayout12(const D3DVERTEXELEMENT9* decl,
                           const CompiledShader12& vs,
                           std::vector<D3D12_INPUT_ELEMENT_DESC>& outElements,
                           std::vector<std::string>& outNames) noexcept;

UINT ExpandFVF12(DWORD fvf, D3DVERTEXELEMENT9* pOut) noexcept;

}

#endif
