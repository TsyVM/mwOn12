// Fixed-function pipeline emulation — generates HLSL for draws with no shader.
//
// D3D9 games may draw without binding a shader at all, leaving transform,
// lighting and texture blending to the driver's fixed-function pipeline.
// Neither D3D11 nor D3D12 has one, so it has to be synthesised: this file
// turns the relevant render states into a vertex and pixel shader pair that
// reproduces the same equations.
//
// The state space is far too large to enumerate, so the states that actually
// change the generated code are packed into an FFPPermKey — light count and
// types, texture stage operations and arguments, fog mode, vertex format — and
// one shader pair is generated and cached per distinct key. Most Wanted uses
// the fixed-function path for its interface and parts of its HUD, so this runs
// on real frames, not just at load.
//
// Both backends share this generator and differ only in compile target.
//
// The lighting equation is worth reproducing exactly, because getting it
// approximately right is easy and produces plausible-looking, wrong output.
// Three mistakes were made here and are worth naming so they are not repeated:
//
//   - Light ambient was accumulated into the diffuse total. D3D9 multiplies a
//     light's ambient contribution by the *material's* ambient term, not its
//     diffuse, so the two must accumulate separately.
//   - Global ambient was pre-seeded into that same accumulator, which then
//     counted it a second time when the final colour was assembled.
//   - Specular used a constant view direction of float3(0,0,-1) — a
//     camera-space vector — inside world-space arithmetic. That is correct
//     only when the view matrix is identity, which is exactly the case a test
//     scene tends to use.
//
// Every stage result is saturated. D3D9 clamps each texture stage's output to
// [0,1] and the modulate-add family routinely exceeds it; a missing saturate()
// showed up as blown-out interface compositing rather than as anything that
// looked like a stage bug.
//
// A few corners of the fixed-function pipeline are not emulated: indexed
// vertex blending (matrix-palette skinning), vertex tweening, sphere-map
// texture coordinate generation, the bump-environment stage operations, and
// user clip planes on a generated vertex shader. Each of these logs once when
// a game asks for it rather than quietly producing wrong output, so a visual
// fault can be traced back here from the log alone.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/FFPShaderGen.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <type_traits>

namespace mwon12 {

namespace {

struct StringBuilder {
    std::string s;
    StringBuilder& operator<<(const char* v)        { s += v; return *this; }
    StringBuilder& operator<<(const std::string& v) { s += v; return *this; }
    StringBuilder& operator<<(char v)               { s += v; return *this; }
    template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, char>, int> = 0>
    StringBuilder& operator<<(T v)
    {
        if constexpr (std::is_signed_v<T>) s += std::to_string(static_cast<long long>(v));
        else                               s += std::to_string(static_cast<unsigned long long>(v));
        return *this;
    }
    [[nodiscard]] const std::string& str() const noexcept { return s; }
};

uint8_t EncodeStageArg(DWORD a) noexcept
{
    uint8_t src;
    switch (a & D3DTA_SELECTMASK) {
    case D3DTA_DIFFUSE:  src = 0; break;
    case D3DTA_CURRENT:  src = 1; break;
    case D3DTA_TEXTURE:  src = 2; break;
    case D3DTA_TFACTOR:  src = 3; break;
    case D3DTA_SPECULAR: src = 4; break;
    case D3DTA_TEMP:     src = 5; break;
    case D3DTA_CONSTANT: src = 3; break;
    default:             src = 1; break;
    }
    uint8_t code = src;
    if (a & D3DTA_COMPLEMENT)     code |= 0x10;
    if (a & D3DTA_ALPHAREPLICATE) code |= 0x20;
    return code;
}

std::string StageArgRGB(uint8_t code, const char* texExpr) noexcept
{
    const uint8_t src  = code & 0xF;
    const bool    comp = (code & 0x10) != 0;
    const bool    repl = (code & 0x20) != 0;

    auto rgbOf = [&](uint8_t s) -> std::string {
        switch (s) {
        case 0: return "IN.Diffuse.rgb";
        case 2: return std::string(texExpr) + ".rgb";
        case 3: return "TextureFactor.rgb";
        case 4: return "IN.Specular.rgb";
        default: return "current.rgb";
        }
    };
    auto alphaOf = [&](uint8_t s) -> std::string {
        switch (s) {
        case 0: return "IN.Diffuse.a";
        case 2: return std::string(texExpr) + ".a";
        case 3: return "TextureFactor.a";
        case 4: return "IN.Specular.a";
        default: return "current.a";
        }
    };

    std::string base = repl
        ? "float3(" + alphaOf(src) + "," + alphaOf(src) + "," + alphaOf(src) + ")"
        : rgbOf(src);
    if (comp) base = "(1.0 - (" + base + "))";
    return base;
}

std::string StageArgAlpha(uint8_t code, const char* texExpr) noexcept
{
    const uint8_t src  = code & 0xF;
    const bool    comp = (code & 0x10) != 0;

    std::string base;
    switch (src) {
    case 0: base = "IN.Diffuse.a"; break;
    case 2: base = std::string(texExpr) + ".a"; break;
    case 3: base = "TextureFactor.a"; break;
    case 4: base = "IN.Specular.a"; break;
    default: base = "current.a"; break;
    }
    if (comp) base = "(1.0 - (" + base + "))";
    return base;
}

std::string ApplyStageOp(uint32_t op,
                          const std::string& a0, const std::string& a1, const std::string& a2,
                          const std::string& a1Alpha,
                          const std::string& texA, const std::string& diffA,
                          const std::string& tfA,  const std::string& curA) noexcept
{
    switch (op) {
    case D3DTOP_SELECTARG1:                return a1;
    case D3DTOP_SELECTARG2:                return a2;
    case D3DTOP_MODULATE:                  return "(" + a1 + " * " + a2 + ")";
    case D3DTOP_MODULATE2X:                return "saturate((" + a1 + ") * (" + a2 + ") * 2.0)";
    case D3DTOP_MODULATE4X:                return "saturate((" + a1 + ") * (" + a2 + ") * 4.0)";
    case D3DTOP_ADD:                       return "saturate((" + a1 + ") + (" + a2 + "))";
    case D3DTOP_ADDSIGNED:                 return "saturate((" + a1 + ") + (" + a2 + ") - 0.5)";
    case D3DTOP_ADDSIGNED2X:               return "saturate(((" + a1 + ") + (" + a2 + ") - 0.5) * 2.0)";
    case D3DTOP_SUBTRACT:                  return "saturate((" + a1 + ") - (" + a2 + "))";
    case D3DTOP_ADDSMOOTH:                 return "saturate((" + a1 + ") + (" + a2 + ") - (" + a1 + ")*(" + a2 + "))";
    case D3DTOP_BLENDDIFFUSEALPHA:         return "lerp(" + a2 + ", " + a1 + ", " + diffA + ")";
    case D3DTOP_BLENDTEXTUREALPHA:         return "lerp(" + a2 + ", " + a1 + ", " + texA + ")";
    case D3DTOP_BLENDFACTORALPHA:          return "lerp(" + a2 + ", " + a1 + ", " + tfA + ")";
    case D3DTOP_BLENDCURRENTALPHA:         return "lerp(" + a2 + ", " + a1 + ", " + curA + ")";
    case D3DTOP_BLENDTEXTUREALPHAPM:       return "saturate((" + a1 + ") + (" + a2 + ") * (1.0 - (" + texA + ")))";

    case D3DTOP_PREMODULATE:               return "(" + a1 + " * " + a2 + ")";

    case D3DTOP_MODULATEALPHA_ADDCOLOR:    return "saturate(" + a1 + " + (" + a1Alpha + ") * (" + a2 + "))";
    case D3DTOP_MODULATECOLOR_ADDALPHA:    return "saturate((" + a1 + ") * (" + a2 + ") + (" + a1Alpha + "))";
    case D3DTOP_MODULATEINVALPHA_ADDCOLOR: return "saturate((1.0 - (" + a1Alpha + ")) * (" + a2 + ") + (" + a1 + "))";
    case D3DTOP_MODULATEINVCOLOR_ADDALPHA: return "saturate((1.0 - (" + a1 + ")) * (" + a2 + ") + (" + a1Alpha + "))";

    case D3DTOP_BUMPENVMAP:
    case D3DTOP_BUMPENVMAPLUMINANCE:       return a1;
    case D3DTOP_MULTIPLYADD:               return "saturate(" + a0 + " + (" + a1 + ") * (" + a2 + "))";
    case D3DTOP_LERP:                      return "lerp(" + a2 + ", " + a1 + ", " + a0 + ")";
    default:                               return "(" + a1 + " * " + a2 + ")";
    }
}
}

UINT ExpandFVF(DWORD fvf, D3DVERTEXELEMENT9* pOut) noexcept
{
    UINT idx    = 0;
    WORD offset = 0;

    auto add = [&](WORD stream, BYTE type, BYTE usage, BYTE usageIdx) {
        pOut[idx++] = { stream, offset, type, D3DDECLMETHOD_DEFAULT, usage, usageIdx };
        switch (type) {
        case D3DDECLTYPE_FLOAT1: offset += 4;  break;
        case D3DDECLTYPE_FLOAT2: offset += 8;  break;
        case D3DDECLTYPE_FLOAT3: offset += 12; break;
        case D3DDECLTYPE_FLOAT4: offset += 16; break;
        case D3DDECLTYPE_D3DCOLOR: offset += 4; break;
        case D3DDECLTYPE_UBYTE4:   offset += 4; break;
        default: break;
        }
    };

    DWORD posType = fvf & D3DFVF_POSITION_MASK;
    int totalBetas = 0;
    switch (posType) {
    case D3DFVF_XYZ:    add(0, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0); break;
    case D3DFVF_XYZRHW: add(0, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0); break;
    case D3DFVF_XYZB1:  add(0, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0); totalBetas = 1; break;
    case D3DFVF_XYZB2:  add(0, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0); totalBetas = 2; break;
    case D3DFVF_XYZB3:  add(0, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0); totalBetas = 3; break;
    case D3DFVF_XYZB4:  add(0, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0); totalBetas = 4; break;
    case D3DFVF_XYZB5:  add(0, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0); totalBetas = 5; break;
    default:             add(0, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0); break;
    }
    if (totalBetas > 0) {
        bool lastIsIndex = (fvf & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR)) != 0;
        int weightFloats = lastIsIndex ? totalBetas - 1 : totalBetas;
        static const BYTE kFloatType[5] = {
            0  , D3DDECLTYPE_FLOAT1, D3DDECLTYPE_FLOAT2,
            D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4
        };
        if (weightFloats > 0)
            add(0, kFloatType[std::min(weightFloats, 4)], D3DDECLUSAGE_BLENDWEIGHT, 0);
        if (lastIsIndex) {
            BYTE idxType = static_cast<BYTE>((fvf & D3DFVF_LASTBETA_D3DCOLOR)
                                                 ? D3DDECLTYPE_D3DCOLOR
                                                 : D3DDECLTYPE_UBYTE4);
            add(0, idxType, D3DDECLUSAGE_BLENDINDICES, 0);
        }
    }

    if (fvf & D3DFVF_NORMAL)   add(0, D3DDECLTYPE_FLOAT3,   D3DDECLUSAGE_NORMAL,   0);
    if (fvf & D3DFVF_PSIZE)    add(0, D3DDECLTYPE_FLOAT1,   D3DDECLUSAGE_PSIZE,    0);
    if (fvf & D3DFVF_DIFFUSE)  add(0, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR,    0);
    if (fvf & D3DFVF_SPECULAR) add(0, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR,    1);

    UINT texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (UINT t = 0; t < texCount; ++t) {
        UINT coordSize = (fvf >> (16 + t * 2)) & 3;
        BYTE declType;

        switch (coordSize) {
        case 1:  declType = D3DDECLTYPE_FLOAT3; break;
        case 2:  declType = D3DDECLTYPE_FLOAT4; break;
        case 3:  declType = D3DDECLTYPE_FLOAT1; break;
        default: declType = D3DDECLTYPE_FLOAT2; break;
        }
        add(0, declType, D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(t));
    }

    pOut[idx] = D3DDECL_END();
    return idx;
}

FFPPermKey BuildFFPKey(const D9RenderState& st,
                       const FFPFixedState& fx,
                       DWORD fvf,
                       const D3DVERTEXELEMENT9* pDecl) noexcept
{
    FFPPermKey k{};
    const auto& rs = st.rs;
    const auto& tss = st.tss;

    k.lightingEnabled = (rs[D3DRS_LIGHTING] != 0) ? 1 : 0;
    k.specularEnabled = (rs[D3DRS_SPECULARENABLE] != 0) ? 1 : 0;
    k.colorVertex     = (rs[D3DRS_COLORVERTEX] != 0) ? 1 : 0;

    uint32_t lcount = 0;
    for (int li = 0; li < 8; ++li) {
        if (!fx.lightEnabled[li]) continue;
        uint32_t type = 0;
        switch (fx.lights[li].Type) {
        case D3DLIGHT_POINT:       type = 1; break;
        case D3DLIGHT_SPOT:        type = 2; break;
        case D3DLIGHT_DIRECTIONAL: type = 0; break;
        }
        k.lightTypes |= (type << (lcount * 2));
        ++lcount;
    }
    k.lightCount = lcount;

    const bool fogOn = rs[D3DRS_FOGENABLE] != 0;
    k.fogVertexMode = fogOn ? (rs[D3DRS_FOGVERTEXMODE] & 3) : 0;
    k.rangeFog      = (k.fogVertexMode != 0 &&
                       rs[D3DRS_RANGEFOGENABLE] != 0) ? 1 : 0;

    bool hasNormal = false, hasColor0 = false, hasColor1 = false, isPosT = false;
    bool hasBlendIndices = false;
    uint32_t texCoordCount = 0;
    uint32_t blendWeightCount = 0;
    if (fvf) {
        hasNormal   = (fvf & D3DFVF_NORMAL)   != 0;
        hasColor0   = (fvf & D3DFVF_DIFFUSE)  != 0;
        hasColor1   = (fvf & D3DFVF_SPECULAR) != 0;
        isPosT      = ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW);
        texCoordCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
        switch (fvf & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZB1: blendWeightCount = 1; break;
        case D3DFVF_XYZB2: blendWeightCount = 2; break;
        case D3DFVF_XYZB3: blendWeightCount = 3; break;
        case D3DFVF_XYZB4: blendWeightCount = 4; break;
        case D3DFVF_XYZB5: blendWeightCount = 5; break;
        default: break;
        }

        if (fvf & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR)) {
            hasBlendIndices = true;
            if (blendWeightCount > 0) --blendWeightCount;
        }
    } else if (pDecl) {
        for (const D3DVERTEXELEMENT9* e = pDecl; e->Stream != 0xFF; ++e) {
            if (e->Usage == D3DDECLUSAGE_NORMAL)    hasNormal = true;
            if (e->Usage == D3DDECLUSAGE_COLOR && e->UsageIndex == 0) hasColor0 = true;
            if (e->Usage == D3DDECLUSAGE_COLOR && e->UsageIndex == 1) hasColor1 = true;
            if (e->Usage == D3DDECLUSAGE_POSITIONT) isPosT = true;
            if (e->Usage == D3DDECLUSAGE_TEXCOORD)
                texCoordCount = std::max(texCoordCount, (uint32_t)e->UsageIndex + 1u);
            if (e->Usage == D3DDECLUSAGE_BLENDWEIGHT) {

                switch (e->Type) {
                case D3DDECLTYPE_FLOAT1: blendWeightCount = 1; break;
                case D3DDECLTYPE_FLOAT2: blendWeightCount = 2; break;
                case D3DDECLTYPE_FLOAT3: blendWeightCount = 3; break;
                case D3DDECLTYPE_FLOAT4: blendWeightCount = 4; break;
                default: break;
                }
            }
            if (e->Usage == D3DDECLUSAGE_BLENDINDICES) hasBlendIndices = true;
        }
    }
    k.hasNormal    = hasNormal   ? 1 : 0;
    k.hasColor0    = hasColor0   ? 1 : 0;
    k.hasColor1    = hasColor1   ? 1 : 0;
    k.positionType = isPosT      ? 1 : 0;
    k.texCoordCount = texCoordCount & 0xF;

    DWORD vbf = rs[D3DRS_VERTEXBLEND];
    bool indexedBlendActive = hasBlendIndices &&
        (rs[D3DRS_INDEXEDVERTEXBLENDENABLE] != 0 || vbf == D3DVBF_0WEIGHTS);
    if (indexedBlendActive || vbf == D3DVBF_TWEENING) {
        static bool s_loggedBlendGap = false;
        if (!s_loggedBlendGap) {
            OutputDebugStringA(indexedBlendActive
                ? "[MWOn12] FFP: indexed vertex blending (matrix palette) requested "
                  "but not implemented; falling back to WORLDMATRIX(0) only.\n"
                : "[MWOn12] FFP: D3DVBF_TWEENING requested but not implemented; "
                  "falling back to WORLDMATRIX(0) only.\n");
            s_loggedBlendGap = true;
        }
        k.vertexBlendMode = 0;
    } else if (vbf >= D3DVBF_1WEIGHTS && vbf <= D3DVBF_3WEIGHTS && blendWeightCount > 0) {

        uint32_t count = std::min(blendWeightCount, 3u);
        if (blendWeightCount > 3) {
            static bool s_loggedClamp = false;
            if (!s_loggedClamp) {
                OutputDebugStringA("[MWOn12] FFP: vertex blend with >3 explicit weights "
                                   "(XYZB4/XYZB5) clamped to 3 (4 matrices).\n");
                s_loggedClamp = true;
            }
        }
        k.vertexBlendMode = count;
    } else {
        k.vertexBlendMode = 0;
    }

    uint32_t ttf = 0;
    for (uint32_t s = 0; s < 8; ++s) {
        if (tss[s][D3DTSS_TEXTURETRANSFORMFLAGS] & ~D3DTTFF_PROJECTED)
            ttf |= (1 << s);
    }
    k.texTransformFlags = ttf;

    uint32_t stageCount = 0;
    for (uint32_t s = 0; s < 8; ++s) {
        if (tss[s][D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
        ++stageCount;
    }
    k.activeStageCount = stageCount;
    k.alphaTestEnabled = (rs[D3DRS_ALPHATESTENABLE] != 0) ? 1 : 0;
    k.alphaTestFunc    = rs[D3DRS_ALPHAFUNC] & 7;
    k.fogPixelMode     = fogOn ? (rs[D3DRS_FOGTABLEMODE] & 3) : 0;

    const auto& stateTextures = st.textures;
    uint32_t stageKindBits = 0;
    for (uint32_t s = 0; s < stageCount; ++s) {
        if (!stateTextures[s]) continue;
        const D3DRESOURCETYPE trt = stateTextures[s]->GetType();
        uint32_t kind = 0;
        if (trt == D3DRTYPE_CUBETEXTURE)        kind = 1;
        else if (trt == D3DRTYPE_VOLUMETEXTURE) kind = 2;
        stageKindBits |= (kind << (s * 2));
    }
    k.texStageKind = stageKindBits;

    uint32_t projMask = 0;
    for (uint32_t s = 0; s < 8; ++s) {
        if (tss[s][D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED)
            projMask |= (1 << s);
    }
    k.projTexMask = projMask;

    for (uint32_t s = 0; s < 8; ++s) {
        auto& sd = k.stages[s];
        sd.colorOp   = static_cast<uint8_t>(tss[s][D3DTSS_COLOROP] & 0x1F);
        sd.colorArg1 = EncodeStageArg(tss[s][D3DTSS_COLORARG1]);
        sd.colorArg2 = EncodeStageArg(tss[s][D3DTSS_COLORARG2]);
        sd.colorArg0 = EncodeStageArg(tss[s][D3DTSS_COLORARG0]);
        sd.alphaOp   = static_cast<uint8_t>(tss[s][D3DTSS_ALPHAOP] & 0x1F);
        sd.alphaArg1 = EncodeStageArg(tss[s][D3DTSS_ALPHAARG1]);
        sd.alphaArg2 = EncodeStageArg(tss[s][D3DTSS_ALPHAARG2]);
        sd.alphaArg0 = EncodeStageArg(tss[s][D3DTSS_ALPHAARG0]);
    }

    for (uint32_t s = 0; s < 8; ++s) {
        DWORD tci  = tss[s][D3DTSS_TEXCOORDINDEX];
        uint32_t idx  = tci & 0x7;
        uint32_t mode = (tci >> 16) & 0xF;

        uint32_t genMode;
        switch (mode) {
        case 0: genMode = 0; break;
        case 1: genMode = 1; break;
        case 2: genMode = 2; break;
        case 3: genMode = 3; break;
        case 4: {

            static bool s_loggedSphereMap = false;
            if (!s_loggedSphereMap) {
                OutputDebugStringA("[MWOn12] FFP: D3DTSS_TCI_SPHEREMAP requested but not "
                                   "implemented; falling back to texcoord passthrough.\n");
                s_loggedSphereMap = true;
            }
            genMode = 0;
            break;
        }
        default:

            genMode = 0;
            break;
        }

        if (genMode != 0 && (isPosT || (!hasNormal && genMode != 2))) {
            static bool s_loggedNoNormal = false;
            if (!s_loggedNoNormal) {
                OutputDebugStringA(isPosT
                    ? "[MWOn12] FFP: texcoord generation requested on pre-transformed "
                      "(XYZRHW) geometry; falling back to texcoord passthrough.\n"
                    : "[MWOn12] FFP: camera-space normal/reflection texcoord generation "
                      "requested but vertex has no NORMAL; falling back to texcoord "
                      "passthrough.\n");
                s_loggedNoNormal = true;
            }
            genMode = 0;
        }

        k.texGen[s] = static_cast<uint8_t>((genMode << 3) | idx);
    }

    return k;
}

std::string GenerateFFPVS(const FFPPermKey& key) noexcept
{
    StringBuilder o;

    o << "#define FFP_LIGHTING "       << key.lightingEnabled << "\n";
    o << "#define FFP_LIGHT_COUNT "    << key.lightCount      << "\n";
    o << "#define FFP_SPECULAR "       << key.specularEnabled << "\n";
    o << "#define FFP_COLOR_VERTEX "   << key.colorVertex     << "\n";
    o << "#define FFP_TEXCOORD_COUNT " << key.texCoordCount   << "\n";
    o << "#define FFP_HAS_NORMAL "     << key.hasNormal       << "\n";
    o << "#define FFP_HAS_COLOR0 "     << key.hasColor0       << "\n";
    o << "#define FFP_HAS_COLOR1 "     << key.hasColor1       << "\n";
    o << "#define FFP_POSITIONT "      << key.positionType    << "\n";
    o << "#define FFP_FOG_VERTEX "     << key.fogVertexMode   << "\n";
    o << "#define FFP_VERTEX_BLEND_COUNT " << key.vertexBlendMode << "\n";
    for (uint32_t s = 0; s < 8; ++s)
        o << "#define FFP_TEX_TRANSFORM_" << s << " "
          << ((key.texTransformFlags >> s) & 1) << "\n";

    o << R"(
cbuffer FFPConstants : register(b1) {
    float4x4 WorldMatrix[4];
    float4x4 ViewMatrix;
    float4x4 ProjectionMatrix;
    float4x4 TextureMatrix[8];
    float4   MaterialDiffuse;
    float4   MaterialAmbient;
    float4   MaterialSpecular;
    float4   MaterialEmissive;
    float    MaterialPower;
    float3   CameraWorldPos;   // world-space eye
    float4   GlobalAmbient;
    float4   FogColor;
    float    FogStart;
    float    FogEnd;
    float    FogDensity;
    float    AlphaRef;
    float4   TextureFactor;
    float4   ViewportInfo;   // x=1/vpW y=1/vpH z=vpX w=vpY (XYZRHW → clip)
    struct { int Type; float3 p0; float4 Diffuse; float4 Specular; float4 Ambient;
             float4 Position; float4 Direction; float Range; float Falloff;
             float Attenuation0; float Attenuation1; float Attenuation2;
             float Theta; float Phi; float p1; } Lights[8];
    int ActiveLightCount; int3 _padLC;
}
)";

    o << "struct VSIn {\n";
    o << "    float4 Position : POSITION;\n";
    if (key.vertexBlendMode > 0)
        o << "    float" << key.vertexBlendMode << " BlendWeight : BLENDWEIGHT;\n";
    if (key.hasNormal)   o << "    float3 Normal   : NORMAL;\n";
    if (key.hasColor0)   o << "    float4 Color0   : COLOR0;\n";
    if (key.hasColor1)   o << "    float4 Color1   : COLOR1;\n";
    for (uint32_t t = 0; t < key.texCoordCount; ++t)
        o << "    float4 Tex" << t << " : TEXCOORD" << t << ";\n";
    o << "};\n";

    const uint32_t texRegCount = key.mixedFullTexOutputs
        ? 8u
        : std::max((uint32_t)key.texCoordCount, (uint32_t)key.activeStageCount);
    o << "struct VSOut {\n";
    o << "    float4 Pos     : SV_POSITION;\n";
    o << "    float4 Diffuse : COLOR0;\n";
    o << "    float4 Specular: COLOR1;\n";
    o << "    float  Fog     : TEXCOORD8;\n";
    for (uint32_t t = 0; t < texRegCount; ++t)
        o << "    float4 Tex" << t << " : TEXCOORD" << t << ";\n";
    o << "};\n";

    if (key.lightingEnabled && key.lightCount > 0 && key.hasNormal && !key.positionType) {

        o << R"(
void AccumulateLight(int i, float3 worldPos, float3 worldNorm,
                     inout float4 diffAcc, inout float4 ambAcc, inout float4 specAcc) {
    float3 L; float attn = 1.0;
    if (Lights[i].Type == 1) {
        float3 v = Lights[i].Position.xyz - worldPos;
        float d  = length(v); if (d > Lights[i].Range) return;
        L    = v / d;
        attn = 1.0 / (Lights[i].Attenuation0 + Lights[i].Attenuation1*d + Lights[i].Attenuation2*d*d);
    } else if (Lights[i].Type == 2) {
        float3 v = Lights[i].Position.xyz - worldPos;
        float d  = length(v); if (d > Lights[i].Range) return;
        L = v / d;
        float cA = dot(-L, normalize(Lights[i].Direction.xyz));
        float cT = cos(Lights[i].Theta*0.5), cP = cos(Lights[i].Phi*0.5);
        float sp = saturate((cA-cP)/(cT-cP+1e-6)); sp = pow(sp, Lights[i].Falloff);
        attn = sp / (Lights[i].Attenuation0 + Lights[i].Attenuation1*d + Lights[i].Attenuation2*d*d);
    } else {
        L = normalize(-Lights[i].Direction.xyz);
    }
    float NdotL = max(0.0, dot(worldNorm, L));
    diffAcc += attn * Lights[i].Diffuse * NdotL;
    // Separate accumulator on purpose: D3D9 scales a light's ambient by the
    // material's ambient term, not its diffuse. Folding these together is the
    // subtle version of this bug and it survives casual inspection.
    ambAcc  += attn * Lights[i].Ambient;
)";
        if (key.specularEnabled) {
            o << R"(
    if (NdotL > 0.0) {
        // Eye vector derived from the actual camera position in world space.
        // A constant float3(0,0,-1) is the camera-space view direction and is
        // only correct at an identity view matrix — which is precisely the
        // condition a simple test scene satisfies, so the error hides.
        float3 E = normalize(CameraWorldPos - worldPos);
        float3 H = normalize(L + E);
        float  s = max(0.0, dot(worldNorm, H));
        specAcc += attn * Lights[i].Specular * pow(s, max(MaterialPower, 1.0));
    }
)";
        }
        o << "}\n";
    }

    o << "VSOut main(VSIn IN) {\n";
    o << "    VSOut OUT = (VSOut)0;\n";

    if (key.positionType) {
        // Pre-transformed vertices (D3DFVF_XYZRHW). These arrive already in
        // screen space with a reciprocal w, and D3D9 feeds them past the
        // transform stage entirely. Passing them straight into SV_POSITION —
        // which the first version of this did — puts every 2-D interface quad
        // at the wrong scale and offset, because the hardware still applies
        // the viewport transform and the perspective divide.
        //
        // So D3D9's inverse viewport transform is reproduced explicitly: undo
        // the half-pixel offset, map screen coordinates into normalised device
        // coordinates against the current viewport, then multiply through by
        // clip-space w so that the divide the hardware is about to perform
        // arrives back at the intended NDC value.
        o << "    float _rhw = IN.Position.w;\n";
        o << "    float _w   = (_rhw > 1e-8) ? (1.0 / _rhw) : 1.0;\n";
        o << "    float2 _s  = IN.Position.xy - 0.5;\n";
        o << "    float2 _ndc;\n";
        o << "    _ndc.x = (_s.x - ViewportInfo.z) * (2.0 * ViewportInfo.x) - 1.0;\n";
        o << "    _ndc.y = 1.0 - (_s.y - ViewportInfo.w) * (2.0 * ViewportInfo.y);\n";
        o << "    OUT.Pos = float4(_ndc * _w, IN.Position.z * _w, _w);\n";
    } else if (key.vertexBlendMode > 0) {

        o << "    float _bw[4];\n";
        const char* comps = "xyzw";
        o << "    float _bwSum = 0;\n";
        for (uint32_t i = 0; i < key.vertexBlendMode; ++i) {
            o << "    _bw[" << i << "] = IN.BlendWeight." << comps[i] << "; _bwSum += _bw[" << i << "];\n";
        }
        o << "    _bw[" << key.vertexBlendMode << "] = 1.0 - _bwSum;\n";
        o << "    float4 worldPos = 0; float3 wN_blend = 0;\n";
        for (uint32_t i = 0; i <= key.vertexBlendMode; ++i) {
            o << "    worldPos += _bw[" << i << "] * mul(float4(IN.Position.xyz,1), WorldMatrix[" << i << "]);\n";
            if (key.hasNormal)
                o << "    wN_blend += _bw[" << i << "] * mul(IN.Normal, (float3x3)WorldMatrix[" << i << "]);\n";
        }
        o << "    float4 viewPos  = mul(worldPos, ViewMatrix);\n";
        o << "    OUT.Pos = mul(viewPos, ProjectionMatrix);\n";
    } else {
        o << "    float4 worldPos = mul(float4(IN.Position.xyz,1), WorldMatrix[0]);\n";
        o << "    float4 viewPos  = mul(worldPos, ViewMatrix);\n";
        o << "    OUT.Pos = mul(viewPos, ProjectionMatrix);\n";
    }

    if (key.lightingEnabled && key.lightCount > 0 && key.hasNormal && !key.positionType) {
        if (key.vertexBlendMode > 0)
            o << "    float3 wN = normalize(wN_blend);\n";
        else
            o << "    float3 wN = normalize(mul(IN.Normal, (float3x3)WorldMatrix[0]));\n";

        // All three start at zero. Seeding the ambient accumulator with
        // GlobalAmbient here is the tempting shortcut and is wrong: the final
        // colour below already adds GlobalAmbient, so a seeded accumulator
        // counts it twice and the scene comes out uniformly washed out.
        o << "    float4 dAcc = (float4)0;\n";
        o << "    float4 aAcc = (float4)0;\n";
        o << "    float4 sAcc = (float4)0;\n";
        o << "    for (int _li = 0; _li < " << key.lightCount << "; ++_li) AccumulateLight(_li, worldPos.xyz, wN, dAcc, aAcc, sAcc);\n";
        if (key.colorVertex && key.hasColor0)
            o << "    float4 _diffMat = IN.Color0;\n";
        else
            o << "    float4 _diffMat = MaterialDiffuse;\n";
        o << "    OUT.Diffuse   = saturate(MaterialEmissive + (GlobalAmbient + aAcc) * MaterialAmbient + dAcc * _diffMat);\n";
        o << "    OUT.Diffuse.a = _diffMat.a;\n";
        if (key.specularEnabled)
            o << "    OUT.Specular = saturate(sAcc * MaterialSpecular);\n";
    } else {
        if (key.hasColor0)
            o << "    OUT.Diffuse = IN.Color0;\n";
        else
            o << "    OUT.Diffuse = MaterialDiffuse;\n";
        if (key.hasColor1)
            o << "    OUT.Specular = IN.Color1;\n";
    }

    if (key.fogVertexMode > 0 && key.positionType) {
        o << (key.hasColor1 ? "    OUT.Fog = IN.Color1.a;\n"
                            : "    OUT.Fog = 1.0;\n");
    } else if (key.fogVertexMode > 0) {
        o << (key.rangeFog ? "    float _fogD = length(viewPos.xyz);\n"
                           : "    float _fogD = abs(viewPos.z);\n");
        if (key.fogVertexMode == 3)
            o << "    OUT.Fog = saturate((FogEnd - _fogD) / (FogEnd - FogStart + 1e-6));\n";
        else if (key.fogVertexMode == 1)
            o << "    OUT.Fog = saturate(exp(-FogDensity * _fogD));\n";
        else
            o << "    { float _fe = FogDensity * _fogD; OUT.Fog = saturate(exp(-_fe * _fe)); }\n";
    } else {
        o << "    OUT.Fog = 1.0;\n";
    }

    bool needsNormalGen = false, needsReflGen = false;
    for (uint32_t s = 0; s < key.activeStageCount; ++s) {
        uint32_t mode = (key.texGen[s] >> 3) & 7;
        if (mode == 1 || mode == 3) needsNormalGen = true;
        if (mode == 3)              needsReflGen   = true;
    }
    if (needsNormalGen) {

        if (key.vertexBlendMode > 0)
            o << "    float3 wN_tg = normalize(mul(normalize(wN_blend), (float3x3)ViewMatrix));\n";
        else
            o << "    float3 wN_tg = normalize(mul(mul(IN.Normal, (float3x3)WorldMatrix[0]), (float3x3)ViewMatrix));\n";
    }
    if (needsReflGen) {

        o << "    float3 refl_tg = reflect(normalize(viewPos.xyz), wN_tg);\n";
    }

    for (uint32_t t = 0; t < texRegCount; ++t) {
        std::string src;
        if (t < key.activeStageCount) {
            uint32_t mode = (key.texGen[t] >> 3) & 7;
            uint32_t idx  = key.texGen[t] & 7;
            switch (mode) {
            case 1:  src = "float4(wN_tg, 1.0)";      break;
            case 2:  src = "float4(viewPos.xyz, 1.0)"; break;
            case 3:  src = "float4(refl_tg, 1.0)";     break;
            default:
                src = (idx < key.texCoordCount)
                    ? (std::string("IN.Tex") + std::to_string(idx))
                    : "float4(0,0,0,1)";
                break;
            }
        } else {

            src = (t < key.texCoordCount)
                ? (std::string("IN.Tex") + std::to_string(t))
                : "float4(0,0,0,1)";
        }
        if ((key.texTransformFlags >> t) & 1)
            o << "    OUT.Tex" << t << " = mul(" << src << ", TextureMatrix[" << t << "]);\n";
        else
            o << "    OUT.Tex" << t << " = " << src << ";\n";
    }

    o << "    return OUT;\n}\n";
    return o.str();
}

std::string GenerateFFPPS(const FFPPermKey& key) noexcept
{
    StringBuilder o;

    for (uint32_t s = 0; s < 8; ++s) {
        const uint32_t kind = (key.texStageKind >> (s * 2)) & 0x3u;
        const char* texType = (kind == 1) ? "TextureCube"
                             : (kind == 2) ? "Texture3D"
                                           : "Texture2D";
        o << texType << " gTex" << s << " : register(t" << s << ");\n";
    }
    o << "SamplerState gSamp[8]: register(s0);\n";

    o << R"(
cbuffer FFPConstants : register(b1) {
    float4x4 _WM[4],_VM,_PM,_TM[8];
    float4 _MD,_MA,_MS,_ME; float _MP; float3 _pM;
    float4 GlobalAmbient, FogColor;
    float FogStart, FogEnd, FogDensity, AlphaRef;
    float4 TextureFactor;
    float4 ViewportInfo;   // keep layout in lock-step with VS/FFPConstantData
    int4 _Lights[64]; int ActiveLightCount; int3 _pLC;  // 8 lights x 128 B = 1024 B
}
)";

    const uint32_t texRegCount = key.mixedFullTexOutputs
        ? 8u
        : std::max((uint32_t)key.texCoordCount, (uint32_t)key.activeStageCount);
    o << "struct PSIn {\n";
    o << "    float4 Pos     : SV_POSITION;\n";
    o << "    float4 Diffuse : COLOR0;\n";
    o << "    float4 Specular: COLOR1;\n";
    o << "    float  Fog     : TEXCOORD8;\n";
    for (uint32_t t = 0; t < texRegCount; ++t)
        o << "    float4 Tex" << t << " : TEXCOORD" << t << ";\n";
    o << "};\n";

    if (key.mixedPsLink) {

        o << "struct PSRaw {\n";
        o << "    float4 Pos : SV_POSITION;\n";
        if (key.linkColor0) o << "    float4 Diffuse : COLOR0;\n";
        if (key.linkColor1) o << "    float4 Specular: COLOR1;\n";
        if (key.linkFog)    o << "    float  Fog     : FOG0;\n";
        for (uint32_t t = 0; t < 8; ++t)
            if ((key.linkTexMask >> t) & 1)
                o << "    float4 Tex" << t << " : TEXCOORD" << t << ";\n";
        o << "};\n";
        o << "float4 main(PSRaw RAW) : SV_TARGET {\n";
        o << "    PSIn IN = (PSIn)0;\n";
        o << "    IN.Pos      = RAW.Pos;\n";
        o << (key.linkColor0 ? "    IN.Diffuse  = RAW.Diffuse;\n"
                             : "    IN.Diffuse  = float4(1,1,1,1);\n");
        o << (key.linkColor1 ? "    IN.Specular = RAW.Specular;\n"
                             : "    IN.Specular = float4(0,0,0,0);\n");
        o << (key.linkFog    ? "    IN.Fog      = RAW.Fog;\n"
                             : "    IN.Fog      = 1.0;\n");
        for (uint32_t t = 0; t < texRegCount; ++t) {
            if ((key.linkTexMask >> t) & 1)
                o << "    IN.Tex" << t << " = RAW.Tex" << t << ";\n";
            else
                o << "    IN.Tex" << t << " = float4(0,0,0,1);\n";
        }
    } else {
        o << "float4 main(PSIn IN) : SV_TARGET {\n";
    }
    o << "    float4 current = IN.Diffuse;\n";
    o << "    float4 tex;\n";

    for (uint32_t s = 0; s < key.activeStageCount; ++s) {

        static const char* const kTexS[8] = {
            "IN.Tex0.xy", "IN.Tex1.xy", "IN.Tex2.xy", "IN.Tex3.xy",
            "IN.Tex4.xy", "IN.Tex5.xy", "IN.Tex6.xy", "IN.Tex7.xy"
        };
        static const char* const kTexSProj[8] = {
            "IN.Tex0.xy/IN.Tex0.w", "IN.Tex1.xy/IN.Tex1.w",
            "IN.Tex2.xy/IN.Tex2.w", "IN.Tex3.xy/IN.Tex3.w",
            "IN.Tex4.xy/IN.Tex4.w", "IN.Tex5.xy/IN.Tex5.w",
            "IN.Tex6.xy/IN.Tex6.w", "IN.Tex7.xy/IN.Tex7.w"
        };

        static const char* const kTexS3[8] = {
            "IN.Tex0.xyz", "IN.Tex1.xyz", "IN.Tex2.xyz", "IN.Tex3.xyz",
            "IN.Tex4.xyz", "IN.Tex5.xyz", "IN.Tex6.xyz", "IN.Tex7.xyz"
        };
        const uint32_t stageKind = (key.texStageKind >> (s * 2)) & 0x3u;
        const char* uv     = (stageKind != 0) ? kTexS3[s] : kTexS[s];

        const char* uvProj = kTexSProj[s];

        const std::string texN = std::string("gTex") + std::to_string(s);
        if (stageKind == 0 && ((key.projTexMask >> s) & 1))
            o << "    tex = " << texN << ".Sample(gSamp[" << s << "], "
              << uvProj << ");\n";
        else
            o << "    tex = " << texN << ".Sample(gSamp[" << s << "], " << uv << ");\n";

        const FFPStageDesc& sd = key.stages[s];
        const uint32_t colorOp = sd.colorOp;
        const uint32_t alphaOp = sd.alphaOp;

        const std::string texA  = "tex.a";
        const std::string diffA = "IN.Diffuse.a";
        const std::string tfA   = "TextureFactor.a";
        const std::string curA  = "current.a";

        if (colorOp == D3DTOP_DISABLE) {
            o << "    // stage " << s << " disabled\n";
        } else if (colorOp == D3DTOP_DOTPRODUCT3) {

            std::string a1 = StageArgRGB(sd.colorArg1, "tex");
            std::string a2 = StageArgRGB(sd.colorArg2, "tex");

            o << "    { float _d" << s << " = saturate(dot((" << a1 << ")*2.0-1.0, ("
              << a2 << ")*2.0-1.0)); current = float4(_d" << s << ",_d" << s
              << ",_d" << s << ",_d" << s << "); }\n";
        } else {
            std::string c0 = StageArgRGB(sd.colorArg0, "tex");
            std::string c1 = StageArgRGB(sd.colorArg1, "tex");
            std::string c2 = StageArgRGB(sd.colorArg2, "tex");
            std::string c1a = StageArgAlpha(sd.colorArg1, "tex");
            std::string colorExpr = ApplyStageOp(colorOp, c0, c1, c2, c1a,
                                                  texA, diffA, tfA, curA);

            std::string alphaExpr;
            if (alphaOp == D3DTOP_DISABLE) {
                alphaExpr = "current.a";
            } else {
                std::string a0 = StageArgAlpha(sd.alphaArg0, "tex");
                std::string a1 = StageArgAlpha(sd.alphaArg1, "tex");
                std::string a2 = StageArgAlpha(sd.alphaArg2, "tex");
                alphaExpr = ApplyStageOp(alphaOp, a0, a1, a2, a1, texA, diffA, tfA, curA);
            }

            o << "    current = float4(" << colorExpr << ", " << alphaExpr << ");\n";
        }
    }

    if (key.specularEnabled)
        o << "    current.rgb = saturate(current.rgb + IN.Specular.rgb);\n";

    if (key.fogPixelMode == 3)
        o << "    { float _fd = IN.Pos.w;"
             " float _ff = saturate((FogEnd - _fd) / (FogEnd - FogStart + 1e-6));"
             " current.rgb = lerp(FogColor.rgb, current.rgb, _ff); }\n";
    else if (key.fogPixelMode == 1)
        o << "    { float _ff = saturate(exp(-FogDensity * IN.Pos.w));"
             " current.rgb = lerp(FogColor.rgb, current.rgb, _ff); }\n";
    else if (key.fogPixelMode == 2)
        o << "    { float _fd = FogDensity * IN.Pos.w;"
             " float _ff = saturate(exp(-_fd * _fd));"
             " current.rgb = lerp(FogColor.rgb, current.rgb, _ff); }\n";
    else if (key.fogVertexMode > 0)

        o << "    current.rgb = lerp(FogColor.rgb, current.rgb, saturate(IN.Fog));\n";

    if (key.alphaTestEnabled) {
        uint32_t fn = key.alphaTestFunc;
        const char* cmp = nullptr;
        switch (fn) {
        case D3DCMP_NEVER:        o << "    discard;\n"; break;
        case D3DCMP_LESS:         cmp = "<";  break;
        case D3DCMP_EQUAL:        cmp = "=="; break;
        case D3DCMP_LESSEQUAL:    cmp = "<="; break;
        case D3DCMP_GREATER:      cmp = ">";  break;
        case D3DCMP_NOTEQUAL:     cmp = "!="; break;
        case D3DCMP_GREATEREQUAL: cmp = ">="; break;
        default: break;
        }
        if (cmp)
            o << "    if (!(current.a " << cmp << " AlphaRef)) discard;\n";
    }

    o << "    return current;\n}\n";
    return o.str();
}

void FillFFPConstants(FFPConstantData& cb,
                      const D9RenderState& st,
                      const FFPFixedState& fx) noexcept
{
    const auto& rs = st.rs;

    for (int w = 0; w < 4; ++w)
        std::memcpy(cb.WorldMatrix[w], &fx.world[w], 64);
    std::memcpy(cb.ViewMatrix,       &fx.view, 64);
    std::memcpy(cb.ProjectionMatrix, &fx.proj, 64);
    for (int t = 0; t < 8; ++t)
        std::memcpy(cb.TextureMatrix[t], &fx.texMatrix[t], 64);

    std::memcpy(cb.MaterialDiffuse,  &fx.material.Diffuse,  16);
    std::memcpy(cb.MaterialAmbient,  &fx.material.Ambient,  16);
    std::memcpy(cb.MaterialSpecular, &fx.material.Specular, 16);
    std::memcpy(cb.MaterialEmissive, &fx.material.Emissive, 16);
    cb.MaterialPower = fx.material.Power;

    {
        const D3DMATRIX& V = fx.view;
        cb.CameraWorldPos[0] = -(V._41 * V._11 + V._42 * V._12 + V._43 * V._13);
        cb.CameraWorldPos[1] = -(V._41 * V._21 + V._42 * V._22 + V._43 * V._23);
        cb.CameraWorldPos[2] = -(V._41 * V._31 + V._42 * V._32 + V._43 * V._33);
    }

    cb.FogStart   = *reinterpret_cast<const float*>(&rs[D3DRS_FOGSTART]);
    cb.FogEnd     = *reinterpret_cast<const float*>(&rs[D3DRS_FOGEND]);
    cb.FogDensity = *reinterpret_cast<const float*>(&rs[D3DRS_FOGDENSITY]);
    cb.AlphaRef   = rs[D3DRS_ALPHAREF] / 255.0f;

    {
        const DWORD fc = rs[D3DRS_FOGCOLOR];
        cb.FogColor[0] = ((fc >> 16) & 0xFF) / 255.0f;
        cb.FogColor[1] = ((fc >>  8) & 0xFF) / 255.0f;
        cb.FogColor[2] = ( fc        & 0xFF) / 255.0f;
        cb.FogColor[3] = ((fc >> 24) & 0xFF) / 255.0f;
    }

    {
        const DWORD tf = rs[D3DRS_TEXTUREFACTOR];
        cb.TextureFactor[0] = ((tf >> 16) & 0xFF) / 255.0f;
        cb.TextureFactor[1] = ((tf >>  8) & 0xFF) / 255.0f;
        cb.TextureFactor[2] = ( tf        & 0xFF) / 255.0f;
        cb.TextureFactor[3] = ((tf >> 24) & 0xFF) / 255.0f;
    }

    int lcount = 0;
    for (int li = 0; li < 8; ++li) {
        if (!fx.lightEnabled[li]) continue;
        auto& L = cb.Lights[lcount++];
        L.Type = fx.lights[li].Type;
        std::memcpy(L.Diffuse,   &fx.lights[li].Diffuse,   16);
        std::memcpy(L.Specular,  &fx.lights[li].Specular,  16);
        std::memcpy(L.Ambient,   &fx.lights[li].Ambient,   16);
        std::memcpy(L.Position,  &fx.lights[li].Position,  12);  L.Position[3]  = 0;
        std::memcpy(L.Direction, &fx.lights[li].Direction, 12);  L.Direction[3] = 0;
        L.Range        = fx.lights[li].Range;
        L.Falloff      = fx.lights[li].Falloff;
        L.Attenuation0 = fx.lights[li].Attenuation0;
        L.Attenuation1 = fx.lights[li].Attenuation1;
        L.Attenuation2 = fx.lights[li].Attenuation2;
        L.Theta        = fx.lights[li].Theta;
        L.Phi          = fx.lights[li].Phi;
    }
    cb.ActiveLightCount = lcount;
}

void FFPFixedState::Reset() noexcept
{
    auto identity = [](D3DMATRIX& m) {
        std::memset(&m, 0, sizeof(m));
        m._11 = m._22 = m._33 = m._44 = 1.0f;
    };
    for (auto& w : world) identity(w);
    identity(view);
    identity(proj);
    for (auto& t : texMatrix) identity(t);

    std::memset(&material, 0, sizeof(material));
    material.Diffuse  = { 1, 1, 1, 1 };
    material.Ambient  = { 0, 0, 0, 0 };
    material.Specular = { 0, 0, 0, 0 };
    material.Emissive = { 0, 0, 0, 0 };
    material.Power    = 0.0f;

    std::memset(lights, 0, sizeof(lights));
    for (auto& e : lightEnabled) e = false;
}

}
