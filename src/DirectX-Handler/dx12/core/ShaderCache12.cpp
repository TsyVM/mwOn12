// Translates and compiles shaders for the D3D12 backend.
//
// Drives the shared translator in core/D3D9ShaderTranslator, then compiles the
// HLSL it returns for the D3D12 shader targets. Results are cached by bytecode
// hash plus the variant state that has to be compiled in.
//
// There is no on-disk cache here, so every shader is recompiled at each
// launch. That is a measurable part of this backend's startup time.
//
// The input signature is reflected once and stored with the compiled shader,
// because the pipeline state object needs it to build an input layout and
// reflecting at draw time would be far too slow.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/ShaderCache12.h>
#include <core/BackendSelect.h>
#include <core/D3D9ShaderTranslator.h>
#include <core/Dx12Common.h>
#include <core/ShaderMods.h>
#include <core/Log.h>

#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <new>

#pragma comment(lib, "d3dcompiler.lib")

namespace mwon12 {

using Microsoft::WRL::ComPtr;

DXGI_FORMAT DeclTypeToFormat12(BYTE declType) noexcept
{
    switch (declType) {
    case D3DDECLTYPE_FLOAT1:    return DXGI_FORMAT_R32_FLOAT;
    case D3DDECLTYPE_FLOAT2:    return DXGI_FORMAT_R32G32_FLOAT;
    case D3DDECLTYPE_FLOAT3:    return DXGI_FORMAT_R32G32B32_FLOAT;
    case D3DDECLTYPE_FLOAT4:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case D3DDECLTYPE_D3DCOLOR:  return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DDECLTYPE_UBYTE4:    return DXGI_FORMAT_R8G8B8A8_UINT;
    case D3DDECLTYPE_SHORT2:    return DXGI_FORMAT_R16G16_SINT;
    case D3DDECLTYPE_SHORT4:    return DXGI_FORMAT_R16G16B16A16_SINT;
    case D3DDECLTYPE_UBYTE4N:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case D3DDECLTYPE_SHORT2N:   return DXGI_FORMAT_R16G16_SNORM;
    case D3DDECLTYPE_SHORT4N:   return DXGI_FORMAT_R16G16B16A16_SNORM;
    case D3DDECLTYPE_USHORT2N:  return DXGI_FORMAT_R16G16_UNORM;
    case D3DDECLTYPE_USHORT4N:  return DXGI_FORMAT_R16G16B16A16_UNORM;
    case D3DDECLTYPE_UDEC3:     return DXGI_FORMAT_R10G10B10A2_UINT;
    case D3DDECLTYPE_DEC3N:     return DXGI_FORMAT_R10G10B10A2_UNORM;
    case D3DDECLTYPE_FLOAT16_2: return DXGI_FORMAT_R16G16_FLOAT;
    case D3DDECLTYPE_FLOAT16_4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                    return DXGI_FORMAT_UNKNOWN;
    }
}

const char* DeclUsageToSemantic12(BYTE usage) noexcept
{
    switch (usage) {
    case D3DDECLUSAGE_POSITION:     return "POSITION";
    case D3DDECLUSAGE_BLENDWEIGHT:  return "BLENDWEIGHT";
    case D3DDECLUSAGE_BLENDINDICES: return "BLENDINDICES";
    case D3DDECLUSAGE_NORMAL:       return "NORMAL";
    case D3DDECLUSAGE_PSIZE:        return "PSIZE";
    case D3DDECLUSAGE_TEXCOORD:     return "TEXCOORD";
    case D3DDECLUSAGE_TANGENT:      return "TANGENT";
    case D3DDECLUSAGE_BINORMAL:     return "BINORMAL";
    case D3DDECLUSAGE_TESSFACTOR:   return "TESSFACTOR";
    case D3DDECLUSAGE_POSITIONT:    return "POSITIONT";
    case D3DDECLUSAGE_COLOR:        return "COLOR";
    case D3DDECLUSAGE_FOG:          return "FOG";
    case D3DDECLUSAGE_DEPTH:        return "DEPTH";
    case D3DDECLUSAGE_SAMPLE:       return "SAMPLE";
    default:                        return "TEXCOORD";
    }
}

UINT ExpandFVF12(DWORD fvf, D3DVERTEXELEMENT9* pOut) noexcept
{
    if (!pOut) return 0;
    UINT n = 0, offset = 0;

    auto add = [&](BYTE type, BYTE usage, BYTE usageIndex, UINT size) {
        if (n >= MAXD3DDECLLENGTH) return;
        pOut[n].Stream     = 0;
        pOut[n].Offset     = static_cast<WORD>(offset);
        pOut[n].Type       = type;
        pOut[n].Method     = D3DDECLMETHOD_DEFAULT;
        pOut[n].Usage      = usage;
        pOut[n].UsageIndex = usageIndex;
        ++n;
        offset += size;
    };

    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ:
        add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);
        break;
    case D3DFVF_XYZRHW:
        add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16);
        break;
    case D3DFVF_XYZB1:
    case D3DFVF_XYZB2:
    case D3DFVF_XYZB3:
    case D3DFVF_XYZB4:
    case D3DFVF_XYZB5: {
        add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);

        const UINT betaCount = ((fvf & D3DFVF_POSITION_MASK) - D3DFVF_XYZB1) / 2 + 1;
        const bool lastIsIndices =
            (fvf & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR)) != 0;
        const UINT weights = lastIsIndices ? (betaCount - 1) : betaCount;
        if (weights >= 1) {
            static const BYTE kWeightType[5] = {
                D3DDECLTYPE_FLOAT1, D3DDECLTYPE_FLOAT1, D3DDECLTYPE_FLOAT2,
                D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4
            };
            const UINT w = weights > 4 ? 4 : weights;
            add(kWeightType[w], D3DDECLUSAGE_BLENDWEIGHT, 0, w * 4);
        }
        if (lastIsIndices)
            add(D3DDECLTYPE_UBYTE4, D3DDECLUSAGE_BLENDINDICES, 0, 4);
        break;
    }
    default:
        break;
    }

    if ((fvf & D3DFVF_NORMAL) && (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW)
        add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
    if (fvf & D3DFVF_PSIZE)
        add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
    if (fvf & D3DFVF_DIFFUSE)
        add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
    if (fvf & D3DFVF_SPECULAR)
        add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);

    const UINT texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (UINT i = 0; i < texCount && i < 8; ++i) {

        const UINT sizeBits = (fvf >> (16 + i * 2)) & 0x3u;
        switch (sizeBits) {
        case D3DFVF_TEXTUREFORMAT1:
            add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(i), 4);
            break;
        case D3DFVF_TEXTUREFORMAT3:
            add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(i), 12);
            break;
        case D3DFVF_TEXTUREFORMAT4:
            add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(i), 16);
            break;
        case D3DFVF_TEXTUREFORMAT2:
        default:
            add(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(i), 8);
            break;
        }
    }

    pOut[n] = D3DDECL_END();
    return n;
}

HRESULT BuildInputLayout12(const D3DVERTEXELEMENT9* decl,
                           const CompiledShader12& vs,
                           std::vector<D3D12_INPUT_ELEMENT_DESC>& outElements,
                           std::vector<std::string>& outNames) noexcept
{
    if (!decl) return D3DERR_INVALIDCALL;

    outElements.clear();
    outNames.clear();
    outElements.reserve(vs.inputSignature.size());

    outNames.reserve(vs.inputSignature.size());

    UINT declCount = 0;
    while (decl[declCount].Stream != 0xFF) {
        if (++declCount > MAXD3DDECLLENGTH) return D3DERR_INVALIDCALL;
    }

    for (const auto& in : vs.inputSignature) {

        if (in.name.size() > 3 &&
            (_strnicmp(in.name.c_str(), "SV_", 3) == 0))
            continue;

        outNames.push_back(in.name);

        D3D12_INPUT_ELEMENT_DESC e{};
        e.SemanticName         = outNames.back().c_str();
        e.SemanticIndex        = in.index;
        e.InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        e.InstanceDataStepRate = 0;

        const D3DVERTEXELEMENT9* match = nullptr;
        for (UINT i = 0; i < declCount; ++i) {
            const char* sem = DeclUsageToSemantic12(decl[i].Usage);
            if (_stricmp(sem, in.name.c_str()) == 0 && decl[i].UsageIndex == in.index) {
                match = &decl[i];
                break;
            }
        }

        if (!match) {
            const bool wantPos  = _stricmp(in.name.c_str(), "POSITION")  == 0;
            const bool wantPosT = _stricmp(in.name.c_str(), "POSITIONT") == 0;
            if (wantPos || wantPosT) {
                for (UINT i = 0; i < declCount; ++i) {
                    if ((decl[i].Usage == D3DDECLUSAGE_POSITION ||
                         decl[i].Usage == D3DDECLUSAGE_POSITIONT) &&
                        decl[i].UsageIndex == in.index) {
                        match = &decl[i];
                        break;
                    }
                }
            }
        }

        if (match) {
            e.Format      = DeclTypeToFormat12(match->Type);
            e.InputSlot   = match->Stream;
            e.AlignedByteOffset = match->Offset;
            if (e.Format == DXGI_FORMAT_UNKNOWN) {
                DXLOG_WARN("[dx12] declaration type %u has no DXGI format; input "
                           "%s%u padded from the zero stream",
                           (unsigned)match->Type, in.name.c_str(), in.index);
                match = nullptr;
            }
        }
        if (!match) {

            e.Format            = DXGI_FORMAT_R32G32B32A32_FLOAT;
            e.InputSlot         = kZeroStreamSlot12;
            e.AlignedByteOffset = 0;

            // Substituted silently, and reported only once per distinct
            // semantic: a missing input the shader declares is a per-draw
            // event, so logging it unconditionally buries everything else.
            struct Seen { char name[24]; UINT index; };
            static Seen     s_seen[32]{};
            static unsigned s_count = 0;
            bool known = false;
            for (unsigned i = 0; i < s_count; ++i)
                if (s_seen[i].index == in.index &&
                    _stricmp(s_seen[i].name, in.name.c_str()) == 0) { known = true; break; }
            if (!known) {
                if (s_count < 32) {
                    strncpy_s(s_seen[s_count].name, in.name.c_str(), _TRUNCATE);
                    s_seen[s_count].index = in.index;
                    ++s_count;
                }
                const bool isPosition = _stricmp(in.name.c_str(), "POSITION")  == 0 ||
                                        _stricmp(in.name.c_str(), "POSITIONT") == 0;
                DXLOG_WARN("[dx12] vertex shader input %s%u has no matching element "
                           "in the %u-element vertex declaration; it will read "
                           "(0,0,0,0) for every vertex.%s",
                           in.name.c_str(), in.index, declCount,
                           isPosition ? " This is the POSITION input, so every vertex "
                                        "of this draw collapses to the origin and the "
                                        "geometry does not appear at all."
                                      : "");
            }
        }
        outElements.push_back(e);
    }

    for (size_t i = 0; i < outElements.size() && i < outNames.size(); ++i)
        outElements[i].SemanticName = outNames[i].c_str();

    return S_OK;
}

HRESULT ShaderCache12::CompileHlsl(const std::string& hlsl, bool isVertexShader,
                                   uint32_t alphaFunc, uint32_t fogMode,
                                   uint32_t texKindMask,
                                   std::vector<uint8_t>* pDxbc) noexcept
{
    std::string defs;
    if (alphaFunc > 0 || fogMode > 0) {
        char def[96];
        std::snprintf(def, sizeof(def), "#define DX9_ATEST %u\n#define DX9_FOG %u\n",
                      alphaFunc, fogMode);
        defs += def;
    }
    if (texKindMask != 0 && !isVertexShader) {
        for (unsigned i = 0; i < 8; ++i) {
            const unsigned kind = (texKindMask >> (i * 2)) & 0x3u;
            if (kind == 0) continue;
            char def[48];
            std::snprintf(def, sizeof(def), "#define DX9_TEXKIND%u %u\n", i, kind);
            defs += def;
        }
    }
    const std::string body = defs.empty() ? hlsl : (defs + hlsl);

    ComPtr<ID3DBlob> blob, errs;

    const HRESULT hr = D3DCompile(
        body.data(), body.size(), "mw4real_dx12_translated",
        nullptr, nullptr, "main",
        isVertexShader ? "vs_5_0" : "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        blob.GetAddressOf(), errs.GetAddressOf());

    if (FAILED(hr)) {
        if (errs)
            DXLOG_ERROR("[dx12] D3DCompile failed: %s",
                        static_cast<const char*>(errs->GetBufferPointer()));

        DXLOG_ERROR("[dx12] generated HLSL that failed to compile:\n%s", body.c_str());
        return hr;
    }

    pDxbc->assign(static_cast<const uint8_t*>(blob->GetBufferPointer()),
                  static_cast<const uint8_t*>(blob->GetBufferPointer()) +
                      blob->GetBufferSize());
    return S_OK;
}

HRESULT ShaderCache12::ReflectSignatures(CompiledShader12& out) noexcept
{
    if (out.dxbc.empty()) return E_FAIL;

    ComPtr<ID3D12ShaderReflection> refl;
    HRESULT hr = D3DReflect(out.dxbc.data(), out.dxbc.size(),
                            IID_PPV_ARGS(refl.GetAddressOf()));
    if (FAILED(hr)) {
        DXLOG_HR(hr, "[dx12] D3DReflect on translated shader failed");
        return hr;
    }

    D3D12_SHADER_DESC sd{};
    if (FAILED(refl->GetDesc(&sd))) return E_FAIL;

    if (out.isVertexShader) {
        out.inputSignature.clear();
        out.inputSignature.reserve(sd.InputParameters);
        for (UINT i = 0; i < sd.InputParameters; ++i) {
            D3D12_SIGNATURE_PARAMETER_DESC p{};
            if (FAILED(refl->GetInputParameterDesc(i, &p))) continue;
            if (!p.SemanticName) continue;
            CompiledShader12::SigInput si;
            si.name  = p.SemanticName;
            si.index = p.SemanticIndex;
            out.inputSignature.push_back(std::move(si));
        }

        out.outTexMask = 0;
        out.outColor0 = out.outColor1 = out.outFog = false;
        for (UINT i = 0; i < sd.OutputParameters; ++i) {
            D3D12_SIGNATURE_PARAMETER_DESC p{};
            if (FAILED(refl->GetOutputParameterDesc(i, &p)) || !p.SemanticName)
                continue;
            if (_stricmp(p.SemanticName, "TEXCOORD") == 0 && p.SemanticIndex < 32)
                out.outTexMask |= (1u << p.SemanticIndex);
            else if (_stricmp(p.SemanticName, "COLOR") == 0)
                (p.SemanticIndex == 0 ? out.outColor0 : out.outColor1) = true;
            else if (_stricmp(p.SemanticName, "FOG") == 0)
                out.outFog = true;
        }
    }
    return S_OK;
}

HRESULT ShaderCache12::GetOrCompile(const DWORD* bytecode, size_t byteLen,
                                    bool isVertexShader,
                                    uint32_t alphaFunc, uint32_t fogMode,
                                    uint32_t texKindMask,
                                    const CompiledShader12** ppOut) noexcept
{
    if (!bytecode || !ppOut) return D3DERR_INVALIDCALL;
    *ppOut = nullptr;

    if (byteLen == 0) {
        byteLen = MeasureD3D9Shader(reinterpret_cast<const uint32_t*>(bytecode), 1u << 20);
        if (byteLen == 0) {
            DXLOG_ERROR("[dx12] shader blob has no END token - refusing to translate");
            return D3DERR_INVALIDCALL;
        }
    }

    Key key{};
    key.bytecodeHash = dx12::HashBytes(bytecode, byteLen);
    key.variant      = alphaFunc | (fogMode << 8);
    key.texKindMask  = isVertexShader ? 0u : texKindMask;

    AcquireSRWLockExclusive(&m_lock);
    if (const auto it = m_cache.find(key); it != m_cache.end()) {
        *ppOut = it->second.get();
        ReleaseSRWLockExclusive(&m_lock);
        return S_OK;
    }
    ReleaseSRWLockExclusive(&m_lock);

    Sm3TranslateOptions opt;
    opt.halfPixelFix        = BackendSelect::HalfPixelFixEnabled();
    opt.legacyColorDefault  = BackendSelect::LegacyColorDefault();
    opt.emitClipPlanes      = true;

    Sm3TranslateResult tr;
    if (!TranslateD3D9Shader(reinterpret_cast<const uint32_t*>(bytecode),
                             byteLen, opt, tr)) {
        DXLOG_ERROR("[dx12] D3D9 shader translation failed: %s", tr.error.c_str());
        return GetDiagnosticShader(isVertexShader, ppOut);
    }

    auto compiled = std::make_unique<CompiledShader12>();
    compiled->isVertexShader = tr.isVertexShader;
    compiled->samplerMask    = tr.samplerMask;
    std::memcpy(compiled->samplerTypes, tr.samplerTypes, sizeof(compiled->samplerTypes));
    compiled->usesVPos       = tr.usesVPos;
    compiled->usesVFace      = tr.usesVFace;
    compiled->writesDepth    = tr.writesDepth;
    compiled->writesOC1Plus  = tr.writesOC1Plus;

    HRESULT hr = E_FAIL;
    if (shadermods::Active()) {
        const shadermods::Id id = shadermods::MakeId(
            bytecode, byteLen, tr.isVertexShader, alphaFunc, fogMode,
            key.texKindMask);
        shadermods::Dump(id, tr.hlsl);

        std::string modded, name;
        if (shadermods::Load(id, &modded, &name)) {
            hr = CompileHlsl(modded, tr.isVertexShader, alphaFunc, fogMode,
                             key.texKindMask, &compiled->dxbc);
            if (SUCCEEDED(hr))
                DXLOG_INFO("[shadermods] using %s.hlsl", name.c_str());
            else
                shadermods::ReportCompileFailure(name);
        }
    }
    if (FAILED(hr))
        hr = CompileHlsl(tr.hlsl, tr.isVertexShader, alphaFunc, fogMode,
                         key.texKindMask, &compiled->dxbc);
    if (FAILED(hr))
        return GetDiagnosticShader(isVertexShader, ppOut);

    hr = ReflectSignatures(*compiled);
    if (FAILED(hr))
        return GetDiagnosticShader(isVertexShader, ppOut);

    AcquireSRWLockExclusive(&m_lock);

    auto [it, inserted] = m_cache.emplace(key, std::move(compiled));
    *ppOut = it->second.get();
    ReleaseSRWLockExclusive(&m_lock);

    DXLOG_TRACE("[dx12] translated %s sm%u_%u -> %zu bytes DXBC (atest=%u fog=%u)",
                tr.isVertexShader ? "VS" : "PS",
                tr.versionMajor, tr.versionMinor, it->second->dxbc.size(),
                alphaFunc, fogMode);
    return S_OK;
}

HRESULT ShaderCache12::GetDiagnosticShader(bool isVertexShader,
                                           const CompiledShader12** ppOut) noexcept
{
    if (!ppOut) return D3DERR_INVALIDCALL;

    AcquireSRWLockExclusive(&m_lock);
    auto& slot = isVertexShader ? m_diagVS : m_diagPS;
    if (slot) {
        *ppOut = slot.get();
        ReleaseSRWLockExclusive(&m_lock);
        return S_OK;
    }
    ReleaseSRWLockExclusive(&m_lock);

    static const char* kDiagVS =
        "struct VSIn { float4 pos : POSITION; };\n"
        "struct VSOut { float4 pos : SV_Position; float4 col0 : COLOR0;\n"
        "               float4 col1 : COLOR1; float fog : TEXCOORD8;\n"
        "               float4 tc[8] : TEXCOORD0; };\n"
        "VSOut main(VSIn i) {\n"
        "  VSOut o = (VSOut)0;\n"
        "  o.pos = i.pos; o.col0 = float4(1,0,1,1);\n"
        "  return o;\n"
        "}\n";
    static const char* kDiagPS =
        "float4 main() : SV_Target { return float4(1.0, 0.0, 1.0, 1.0); }\n";

    auto out = std::make_unique<CompiledShader12>();
    out->isVertexShader = isVertexShader;

    ComPtr<ID3DBlob> blob, errs;
    const char* src = isVertexShader ? kDiagVS : kDiagPS;
    HRESULT hr = D3DCompile(src, std::strlen(src), "mw4real_dx12_diagnostic",
                            nullptr, nullptr, "main",
                            isVertexShader ? "vs_5_0" : "ps_5_0",
                            0, 0, blob.GetAddressOf(), errs.GetAddressOf());
    if (FAILED(hr)) {
        DXLOG_HR(hr, "[dx12] diagnostic shader failed to compile");
        return hr;
    }
    out->dxbc.assign(static_cast<const uint8_t*>(blob->GetBufferPointer()),
                     static_cast<const uint8_t*>(blob->GetBufferPointer()) +
                         blob->GetBufferSize());
    ReflectSignatures(*out);

    AcquireSRWLockExclusive(&m_lock);
    auto& target = isVertexShader ? m_diagVS : m_diagPS;
    if (!target) target = std::move(out);
    *ppOut = target.get();
    ReleaseSRWLockExclusive(&m_lock);
    return S_OK;
}

size_t ShaderCache12::EntryCount() const noexcept
{
    AcquireSRWLockShared(&m_lock);
    const size_t n = m_cache.size();
    ReleaseSRWLockShared(&m_lock);
    return n;
}

}
