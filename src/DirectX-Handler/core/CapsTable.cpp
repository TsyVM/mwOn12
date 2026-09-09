// The D3DCAPS9 table both backends report from GetDeviceCaps.
//
// Read this before adding or removing a single bit.
//
// A capability table is a query API, and a query API that under-reports fails
// *silently*. A game that asks "can you blend?" and is told no does not error
// and does not log — it takes its own fallback path, or skips the feature
// entirely, and the only evidence is something missing from the screen. This
// has been the single most expensive class of bug in the project, twice:
//
//   - A placeholder DX12 table left SrcBlendCaps, TextureOpCaps and
//     MaxTextureBlendStages at zero. Most Wanted read that as "cannot blend,
//     no texture stages" and drew no user interface at all. Four unrelated
//     hypotheses were investigated before the table was suspected.
//
//   - VertexProcessingCaps and DeclTypes were both left zero by a memset.
//     Zero DeclTypes means "FLOAT32 vertex attributes only", which pushes any
//     game that packs its vertex streams — UBYTE4 blend indices, SHORT2
//     texture coordinates, FLOAT16 data, i.e. essentially all of them — onto
//     a path where vertex declarations cannot be built at all.
//
// Hence the rule this file exists to enforce: when something is *missing*
// rather than *wrong*, suspect the query APIs first. It lives in shared code
// so that there is exactly one table and the two backends cannot drift apart.
//
// The opposite error is just as expensive. Do not advertise a capability the
// emulation does not honour: a game told "yes" will commit to that path and
// render silently wrong output, where an honest "no" makes it use a fallback
// that works. D3DVTXPCAPS_TWEENING and D3DVTXPCAPS_TEXGEN_SPHEREMAP were
// removed for exactly this reason and must not come back before their
// implementations do.
//
// Values are otherwise reported generously. These are D3D9 limits being
// satisfied by D3D11-class hardware, so most of them are far below what the
// device can really do, and the honest answer to "how many textures?" is "more
// than a 2005 game will ever ask for".

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d9.h>
#include <d3d12.h>
#include <cstring>

namespace mwon12 {

void SynthesiseCaps(UINT adapterOrdinal, D3DCAPS9* pCaps) noexcept
{
    // Zero first so that any field this function forgets is at least
    // deterministic. Note that zero is a *meaningful and harmful* value for
    // several fields — see the header comment — so "it was memset" is never a
    // reason to leave one unset.
    std::memset(pCaps, 0, sizeof(*pCaps));

    pCaps->DeviceType     = D3DDEVTYPE_HAL;
    pCaps->AdapterOrdinal = adapterOrdinal;

    pCaps->Caps = D3DCAPS_READ_SCANLINE;

    // Defined by the June 2010 DXSDK but absent from the Windows 10 SDK
    // headers we build against, so it is spelled out rather than dropped.
    constexpr DWORD kD3DCAPS2_CANRENDERWINDOWED = 0x00080000L;
    pCaps->Caps2 =
        kD3DCAPS2_CANRENDERWINDOWED   |
        D3DCAPS2_FULLSCREENGAMMA      |
        D3DCAPS2_DYNAMICTEXTURES      |
        D3DCAPS2_CANAUTOGENMIPMAP;

    pCaps->Caps3 =
        D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD |
        D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION       |
        D3DCAPS3_COPY_TO_VIDMEM                    |
        D3DCAPS3_COPY_TO_SYSTEMMEM;

    pCaps->PresentationIntervals =
        D3DPRESENT_INTERVAL_IMMEDIATE |
        D3DPRESENT_INTERVAL_ONE       |
        D3DPRESENT_INTERVAL_TWO       |
        D3DPRESENT_INTERVAL_THREE     |
        D3DPRESENT_INTERVAL_FOUR;

    pCaps->CursorCaps = D3DCURSORCAPS_COLOR | D3DCURSORCAPS_LOWRES;

    pCaps->DevCaps =
        D3DDEVCAPS_EXECUTESYSTEMMEMORY |
        D3DDEVCAPS_EXECUTEVIDEOMEMORY  |
        D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
        D3DDEVCAPS_TLVERTEXVIDEOMEMORY  |
        D3DDEVCAPS_DRAWPRIMITIVES2     |
        D3DDEVCAPS_DRAWPRIMITIVES2EX   |
        D3DDEVCAPS_DRAWPRIMTLVERTEX    |
        D3DDEVCAPS_CANBLTSYSTONONLOCAL |
        D3DDEVCAPS_HWTRANSFORMANDLIGHT |
        D3DDEVCAPS_PUREDEVICE          |
        D3DDEVCAPS_HWRASTERIZATION;

    pCaps->PrimitiveMiscCaps =
        D3DPMISCCAPS_MASKZ                  |
        D3DPMISCCAPS_CULLNONE               |
        D3DPMISCCAPS_CULLCW                 |
        D3DPMISCCAPS_CULLCCW                |
        D3DPMISCCAPS_COLORWRITEENABLE       |
        D3DPMISCCAPS_CLIPPLANESCALEDPOINTS  |
        D3DPMISCCAPS_CLIPTLVERTS            |
        D3DPMISCCAPS_TSSARGTEMP             |
        D3DPMISCCAPS_BLENDOP                |
        D3DPMISCCAPS_NULLREFERENCE          |
        D3DPMISCCAPS_INDEPENDENTWRITEMASKS  |
        D3DPMISCCAPS_PERSTAGECONSTANT       |
        D3DPMISCCAPS_FOGANDSPECULARALPHA    |
        D3DPMISCCAPS_SEPARATEALPHABLEND     |
        D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS|
        D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING |
        D3DPMISCCAPS_FOGVERTEXCLAMPED;

    pCaps->RasterCaps =
        D3DPRASTERCAPS_DITHER           |
        D3DPRASTERCAPS_ZTEST            |
        D3DPRASTERCAPS_FOGVERTEX        |
        D3DPRASTERCAPS_FOGTABLE         |
        D3DPRASTERCAPS_MIPMAPLODBIAS    |
        D3DPRASTERCAPS_ZBUFFERLESSHSR   |
        D3DPRASTERCAPS_FOGRANGE         |
        D3DPRASTERCAPS_ANISOTROPY       |

        D3DPRASTERCAPS_WFOG             |
        D3DPRASTERCAPS_ZFOG             |
        D3DPRASTERCAPS_COLORPERSPECTIVE |
        D3DPRASTERCAPS_SCISSORTEST      |
        D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS |
        D3DPRASTERCAPS_DEPTHBIAS        |
        D3DPRASTERCAPS_MULTISAMPLE_TOGGLE;

    DWORD kAllCmpFuncs =
        D3DPCMPCAPS_NEVER        |
        D3DPCMPCAPS_LESS         |
        D3DPCMPCAPS_EQUAL        |
        D3DPCMPCAPS_LESSEQUAL    |
        D3DPCMPCAPS_GREATER      |
        D3DPCMPCAPS_NOTEQUAL     |
        D3DPCMPCAPS_GREATEREQUAL |
        D3DPCMPCAPS_ALWAYS;

    pCaps->ZCmpCaps      = kAllCmpFuncs;
    pCaps->AlphaCmpCaps  = kAllCmpFuncs;
    pCaps->StencilCaps   =
        D3DSTENCILCAPS_KEEP       |
        D3DSTENCILCAPS_ZERO       |
        D3DSTENCILCAPS_REPLACE    |
        D3DSTENCILCAPS_INCRSAT    |
        D3DSTENCILCAPS_DECRSAT    |
        D3DSTENCILCAPS_INVERT     |
        D3DSTENCILCAPS_INCR       |
        D3DSTENCILCAPS_DECR       |
        D3DSTENCILCAPS_TWOSIDED;

    DWORD kBlendCaps =
        D3DPBLENDCAPS_ZERO            |
        D3DPBLENDCAPS_ONE             |
        D3DPBLENDCAPS_SRCCOLOR        |
        D3DPBLENDCAPS_INVSRCCOLOR     |
        D3DPBLENDCAPS_SRCALPHA        |
        D3DPBLENDCAPS_INVSRCALPHA     |
        D3DPBLENDCAPS_DESTALPHA       |
        D3DPBLENDCAPS_INVDESTALPHA    |
        D3DPBLENDCAPS_DESTCOLOR       |
        D3DPBLENDCAPS_INVDESTCOLOR    |
        D3DPBLENDCAPS_SRCALPHASAT     |
        D3DPBLENDCAPS_BOTHSRCALPHA    |
        D3DPBLENDCAPS_BOTHINVSRCALPHA |
        D3DPBLENDCAPS_BLENDFACTOR     |
        D3DPBLENDCAPS_INVSRCCOLOR2    |
        D3DPBLENDCAPS_SRCCOLOR2;

    pCaps->SrcBlendCaps  = kBlendCaps;
    pCaps->DestBlendCaps = kBlendCaps;

    pCaps->TextureCaps =
        D3DPTEXTURECAPS_PERSPECTIVE         |
        D3DPTEXTURECAPS_ALPHA               |

        D3DPTEXTURECAPS_MIPMAP              |
        D3DPTEXTURECAPS_MIPVOLUMEMAP        |
        D3DPTEXTURECAPS_MIPCUBEMAP          |
        D3DPTEXTURECAPS_CUBEMAP             |
        D3DPTEXTURECAPS_VOLUMEMAP           |
        D3DPTEXTURECAPS_NONPOW2CONDITIONAL  |
        D3DPTEXTURECAPS_PROJECTED           |
        D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE;

    DWORD kFilterCaps =
        D3DPTFILTERCAPS_MINFPOINT        |
        D3DPTFILTERCAPS_MINFLINEAR       |
        D3DPTFILTERCAPS_MINFANISOTROPIC  |
        D3DPTFILTERCAPS_MIPFPOINT        |
        D3DPTFILTERCAPS_MIPFLINEAR       |
        D3DPTFILTERCAPS_MAGFPOINT        |
        D3DPTFILTERCAPS_MAGFLINEAR       |
        D3DPTFILTERCAPS_MAGFANISOTROPIC;

    pCaps->TextureFilterCaps        = kFilterCaps;
    pCaps->CubeTextureFilterCaps    = kFilterCaps;
    pCaps->VolumeTextureFilterCaps  = kFilterCaps;

    DWORD kAddrCaps =
        D3DPTADDRESSCAPS_WRAP          |
        D3DPTADDRESSCAPS_MIRROR        |
        D3DPTADDRESSCAPS_CLAMP         |
        D3DPTADDRESSCAPS_BORDER        |
        D3DPTADDRESSCAPS_INDEPENDENTUV |
        D3DPTADDRESSCAPS_MIRRORONCE;

    pCaps->TextureAddressCaps       = kAddrCaps;
    pCaps->VolumeTextureAddressCaps = kAddrCaps;

    pCaps->LineCaps =
        D3DLINECAPS_TEXTURE    |
        D3DLINECAPS_ZTEST      |
        D3DLINECAPS_BLEND      |
        D3DLINECAPS_ALPHACMP   |
        D3DLINECAPS_FOG        |
        D3DLINECAPS_ANTIALIAS;

    pCaps->ShadeCaps =
        D3DPSHADECAPS_COLORGOURAUDRGB    |
        D3DPSHADECAPS_SPECULARGOURAUDRGB |
        D3DPSHADECAPS_ALPHAGOURAUDBLEND  |
        D3DPSHADECAPS_FOGGOURAUD;

    pCaps->StretchRectFilterCaps =
        D3DPTFILTERCAPS_MINFPOINT  |
        D3DPTFILTERCAPS_MINFLINEAR |
        D3DPTFILTERCAPS_MAGFPOINT  |
        D3DPTFILTERCAPS_MAGFLINEAR;

    pCaps->MaxTextureWidth      = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    pCaps->MaxTextureHeight     = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    pCaps->MaxVolumeExtent      = D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    pCaps->MaxTextureRepeat     = 8192;
    pCaps->MaxTextureAspectRatio = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    pCaps->MaxAnisotropy        = D3D12_REQ_MAXANISOTROPY;

    pCaps->MaxVertexW = 1.0e10f;
    pCaps->GuardBandLeft   = -32768.0f;
    pCaps->GuardBandTop    = -32768.0f;
    pCaps->GuardBandRight  =  32768.0f;
    pCaps->GuardBandBottom =  32768.0f;
    pCaps->ExtentsAdjust   = 0.0f;

    // TWEENING and TEXGEN_SPHEREMAP are absent on purpose: the fixed-function
    // emulator implements neither, and claiming them makes a game commit to a
    // path that renders wrongly instead of taking its own fallback. Add a bit
    // here only in the same change that implements it.
    pCaps->VertexProcessingCaps =
        D3DVTXPCAPS_TEXGEN            |
        D3DVTXPCAPS_MATERIALSOURCE7   |
        D3DVTXPCAPS_DIRECTIONALLIGHTS |
        D3DVTXPCAPS_POSITIONALLIGHTS  |
        D3DVTXPCAPS_LOCALVIEWER;

    pCaps->VertexShaderVersion = D3DVS_VERSION(3, 0);
    pCaps->PixelShaderVersion  = D3DPS_VERSION(3, 0);

    pCaps->MaxVertexShaderConst      = 256;
    pCaps->PixelShader1xMaxValue     = 8.0f;

    // Every packed vertex format D3D9 defines. Leaving this at zero declares
    // "FLOAT32 attributes only", which breaks vertex declaration creation for
    // essentially every real game — see the header comment.
    pCaps->DeclTypes =
        D3DDTCAPS_UBYTE4    |
        D3DDTCAPS_UBYTE4N   |
        D3DDTCAPS_SHORT2N   |
        D3DDTCAPS_SHORT4N   |
        D3DDTCAPS_USHORT2N  |
        D3DDTCAPS_USHORT4N  |
        D3DDTCAPS_UDEC3     |
        D3DDTCAPS_DEC3N     |
        D3DDTCAPS_FLOAT16_2 |
        D3DDTCAPS_FLOAT16_4;

    pCaps->MaxStreams            = 16;
    pCaps->MaxStreamStride      = 2048;
    pCaps->MaxPrimitiveCount    = 0x0FFFFFFu;
    pCaps->MaxVertexIndex       = 0x0FFFFFFu;

    // D3D11 allows eight; D3D9 games are written against four and some index
    // the array by this value, so report the D3D9 figure.
    pCaps->NumSimultaneousRTs = 4;

    pCaps->MaxUserClipPlanes       = 6;
    pCaps->MaxActiveLights         = 8;
    pCaps->MaxVertexBlendMatrices  = 4;

    // Zero is the honest answer, not an oversight. Indexed vertex blending
    // (matrix-palette skinning via D3DRS_INDEXEDVERTEXBLENDENABLE) is not
    // implemented — the fixed-function path falls back to WORLDMATRIX(0) — so
    // reporting a palette size would invite a game onto a path that silently
    // draws every skinned mesh in its bind pose.
    pCaps->MaxVertexBlendMatrixIndex = 0;
    pCaps->MaxPointSize            = 256.0f;

    pCaps->VS20Caps.Caps                     = D3DVS20CAPS_PREDICATION;
    pCaps->VS20Caps.DynamicFlowControlDepth  = D3DVS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    pCaps->VS20Caps.NumTemps                 = D3DVS20_MAX_NUMTEMPS;
    pCaps->VS20Caps.StaticFlowControlDepth   = D3DVS20_MAX_STATICFLOWCONTROLDEPTH;

    pCaps->PS20Caps.Caps                     = D3DPS20CAPS_ARBITRARYSWIZZLE  |
                                               D3DPS20CAPS_GRADIENTINSTRUCTIONS|
                                               D3DPS20CAPS_PREDICATION         |
                                               D3DPS20CAPS_NODEPENDENTREADLIMIT|
                                               D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
    pCaps->PS20Caps.DynamicFlowControlDepth  = D3DPS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    pCaps->PS20Caps.NumTemps                 = D3DPS20_MAX_NUMTEMPS;
    pCaps->PS20Caps.StaticFlowControlDepth   = D3DPS20_MAX_STATICFLOWCONTROLDEPTH;
    pCaps->PS20Caps.NumInstructionSlots      = D3DPS20_MAX_NUMINSTRUCTIONSLOTS;

    pCaps->VertexTextureFilterCaps = kFilterCaps;

    pCaps->MaxVShaderInstructionsExecuted = 0xFFFFFFFFu;
    pCaps->MaxPShaderInstructionsExecuted = 0xFFFFFFFFu;
    pCaps->MaxVertexShader30InstructionSlots = 32768;
    pCaps->MaxPixelShader30InstructionSlots  = 32768;

    pCaps->DevCaps2 =
        D3DDEVCAPS2_STREAMOFFSET              |
        D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES |
        D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET;

    pCaps->FVFCaps = D3DFVFCAPS_PSIZE | 8;

    // The full texture-stage operation set. BUMPENVMAP, BUMPENVMAPLUMINANCE
    // and PREMODULATE are advertised while the emitter only approximates them
    // (they need cross-stage wiring the stage loop does not model). That is a
    // considered exception to the "don't advertise what you can't honour"
    // rule: each still contributes a plausible result rather than nothing, and
    // withholding them costs the whole fixed-function path. Each logs once.
    pCaps->TextureOpCaps =
        D3DTEXOPCAPS_DISABLE                |
        D3DTEXOPCAPS_SELECTARG1             |
        D3DTEXOPCAPS_SELECTARG2             |
        D3DTEXOPCAPS_MODULATE               |
        D3DTEXOPCAPS_MODULATE2X             |
        D3DTEXOPCAPS_MODULATE4X             |
        D3DTEXOPCAPS_ADD                    |
        D3DTEXOPCAPS_ADDSIGNED              |
        D3DTEXOPCAPS_ADDSIGNED2X            |
        D3DTEXOPCAPS_SUBTRACT               |
        D3DTEXOPCAPS_ADDSMOOTH              |
        D3DTEXOPCAPS_BLENDDIFFUSEALPHA      |
        D3DTEXOPCAPS_BLENDTEXTUREALPHA      |
        D3DTEXOPCAPS_BLENDFACTORALPHA       |
        D3DTEXOPCAPS_BLENDTEXTUREALPHAPM    |
        D3DTEXOPCAPS_BLENDCURRENTALPHA      |
        D3DTEXOPCAPS_PREMODULATE            |
        D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR |
        D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA |
        D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR |
        D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA |
        D3DTEXOPCAPS_BUMPENVMAP             |
        D3DTEXOPCAPS_BUMPENVMAPLUMINANCE    |
        D3DTEXOPCAPS_DOTPRODUCT3            |
        D3DTEXOPCAPS_MULTIPLYADD            |
        D3DTEXOPCAPS_LERP;

    pCaps->MaxTextureBlendStages  = 8;
    pCaps->MaxSimultaneousTextures = 8;
}

}
