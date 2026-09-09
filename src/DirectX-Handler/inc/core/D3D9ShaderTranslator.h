// Translates compiled Direct3D 9 shader bytecode into HLSL.
//
// The game ships its shaders already compiled as D3D9 token streams. Those
// cannot be handed to D3D11 or D3D12, and there is no source to rebuild from,
// so they are disassembled and re-emitted as HLSL for the platform compiler.
//
// The result carries more than the source text. Callers need to know which
// texture registers the shader samples and what kind of texture each expects,
// whether it writes depth or reads the screen position, and which stage it is
// -- all of which the caller must know before it can bind anything to the
// translated shader.

#pragma once

#ifndef MWON12_D3D9_SHADER_TRANSLATOR_H
#define MWON12_D3D9_SHADER_TRANSLATOR_H

#include <cstdint>
#include <cstddef>
#include <string>

namespace mwon12 {

struct Sm3TranslateOptions {

    bool halfPixelFix = true;

    bool emitClipPlanes = true;

    bool legacyColorDefault = true;
};

struct Sm3TranslateResult {
    std::string hlsl;
    std::string error;

    bool     isVertexShader = false;
    uint32_t versionMajor   = 0;
    uint32_t versionMinor   = 0;

    uint32_t samplerMask    = 0;

    uint8_t  samplerTypes[16] = {};

    bool usesVFace     = false;
    bool usesVPos      = false;
    bool writesDepth   = false;
    bool writesOC1Plus = false;
    bool writesFog     = false;
    bool writesPSize   = false;
    bool usesBumpEnv   = false;
    bool usesVSTexture = false;
};

bool TranslateD3D9Shader(
    const uint32_t*            tokens,
    size_t                     maxBytes,
    const Sm3TranslateOptions& opt,
    Sm3TranslateResult&        out);

size_t MeasureD3D9Shader(const uint32_t* tokens, size_t maxBytes);

constexpr uint32_t kSm3TranslatorVersion = 8;

}

#endif
