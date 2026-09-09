// Records D3D9 state changes and tracks what needs rebuilding.
//
// Every setter writes into the shadow state and marks the group of D3D11 state
// objects that the change invalidates -- blend, rasteriser or depth-stencil.
// Grouping matters: changing a cull mode should not force a blend state
// rebuild, and a game that sets state redundantly (which they all do) should
// not cause any rebuild at all.
//
// ApplyDefaults establishes the state a freshly created D3D9 device starts in.
// Getting those defaults wrong is a subtle source of faults, because a game
// that relies on a default without setting it explicitly will render
// incorrectly from the very first frame with nothing to indicate why.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <core/RenderStateTracker.h>

namespace mwon12 {

RenderStateTracker::RenderStateTracker() noexcept
{
    ApplyDefaults();
}

DWORD RenderStateTracker::GroupOf(D3DRENDERSTATETYPE type) noexcept
{
    switch (type) {
    case D3DRS_ALPHABLENDENABLE: case D3DRS_SRCBLEND: case D3DRS_DESTBLEND:
    case D3DRS_BLENDOP: case D3DRS_SEPARATEALPHABLENDENABLE:
    case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA: case D3DRS_BLENDOPALPHA:
    case D3DRS_COLORWRITEENABLE:
        return kDirtyBlend;

    case D3DRS_FILLMODE: case D3DRS_CULLMODE: case D3DRS_SCISSORTESTENABLE:
    case D3DRS_MULTISAMPLEANTIALIAS: case D3DRS_ANTIALIASEDLINEENABLE:
    case D3DRS_DEPTHBIAS: case D3DRS_SLOPESCALEDEPTHBIAS:
        return kDirtyRast;

    case D3DRS_ZENABLE: case D3DRS_ZFUNC: case D3DRS_ZWRITEENABLE:
    case D3DRS_STENCILENABLE: case D3DRS_STENCILMASK: case D3DRS_STENCILWRITEMASK:
    case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL: case D3DRS_STENCILPASS:
    case D3DRS_STENCILFUNC: case D3DRS_TWOSIDEDSTENCILMODE:
    case D3DRS_CCW_STENCILFAIL: case D3DRS_CCW_STENCILZFAIL:
    case D3DRS_CCW_STENCILPASS: case D3DRS_CCW_STENCILFUNC:
        return kDirtyDepthStencil;

    default:
        return 0;
    }
}

void RenderStateTracker::SetRenderState(D3DRENDERSTATETYPE type, DWORD value) noexcept
{
    if (static_cast<UINT>(type) < static_cast<UINT>(D3DRS_BLENDOPALPHA + 1)) {
        if (m_state.rs[type] == value)
            return;
        m_state.rs[type] = value;
        m_dirty |= GroupOf(type);
    }
}

void RenderStateTracker::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) noexcept
{
    if (stage < 8 && static_cast<UINT>(type) < static_cast<UINT>(D3DTSS_CONSTANT + 1)) {
        if (m_state.tss[stage][type] == value)
            return;
        m_state.tss[stage][type] = value;
    }
}

void RenderStateTracker::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) noexcept
{
    if (sampler < 8 && static_cast<UINT>(type) < static_cast<UINT>(D3DSAMP_DMAPOFFSET + 1)) {
        if (m_state.samp[sampler][type] == value)
            return;
        m_state.samp[sampler][type] = value;
        m_sampDirty |= static_cast<UINT8>(1u << sampler);
    }
}

void RenderStateTracker::ApplyDefaults() noexcept
{
    auto& rs = m_state.rs;

    rs[D3DRS_ZENABLE]                    = D3DZB_TRUE;
    rs[D3DRS_FILLMODE]                   = D3DFILL_SOLID;
    rs[D3DRS_SHADEMODE]                  = D3DSHADE_GOURAUD;
    rs[D3DRS_ZWRITEENABLE]               = TRUE;
    rs[D3DRS_ALPHATESTENABLE]            = FALSE;
    rs[D3DRS_LASTPIXEL]                  = TRUE;
    rs[D3DRS_SRCBLEND]                   = D3DBLEND_ONE;
    rs[D3DRS_DESTBLEND]                  = D3DBLEND_ZERO;
    rs[D3DRS_CULLMODE]                   = D3DCULL_CCW;
    rs[D3DRS_ZFUNC]                      = D3DCMP_LESSEQUAL;
    rs[D3DRS_ALPHAREF]                   = 0;
    rs[D3DRS_ALPHAFUNC]                  = D3DCMP_ALWAYS;
    rs[D3DRS_DITHERENABLE]               = FALSE;
    rs[D3DRS_ALPHABLENDENABLE]           = FALSE;
    rs[D3DRS_FOGENABLE]                  = FALSE;
    rs[D3DRS_SPECULARENABLE]             = FALSE;
    rs[D3DRS_FOGCOLOR]                   = 0;
    rs[D3DRS_FOGTABLEMODE]               = D3DFOG_NONE;

    rs[D3DRS_FOGSTART]                   = 0;
    rs[D3DRS_FOGEND]                     = 0x3F800000;
    rs[D3DRS_FOGDENSITY]                 = 0x3F800000;
    rs[D3DRS_RANGEFOGENABLE]             = FALSE;
    rs[D3DRS_STENCILENABLE]              = FALSE;
    rs[D3DRS_STENCILFAIL]                = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILZFAIL]               = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILPASS]                = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILFUNC]                = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILREF]                 = 0;
    rs[D3DRS_STENCILMASK]                = 0xFFFFFFFF;
    rs[D3DRS_STENCILWRITEMASK]           = 0xFFFFFFFF;
    rs[D3DRS_TEXTUREFACTOR]              = 0xFFFFFFFF;
    rs[D3DRS_WRAP0]                      = 0;
    rs[D3DRS_WRAP1]                      = 0;
    rs[D3DRS_WRAP2]                      = 0;
    rs[D3DRS_WRAP3]                      = 0;
    rs[D3DRS_WRAP4]                      = 0;
    rs[D3DRS_WRAP5]                      = 0;
    rs[D3DRS_WRAP6]                      = 0;
    rs[D3DRS_WRAP7]                      = 0;
    rs[D3DRS_CLIPPING]                   = TRUE;
    rs[D3DRS_LIGHTING]                   = TRUE;
    rs[D3DRS_AMBIENT]                    = 0;
    rs[D3DRS_FOGVERTEXMODE]              = D3DFOG_NONE;
    rs[D3DRS_COLORVERTEX]                = TRUE;
    rs[D3DRS_LOCALVIEWER]                = TRUE;

    rs[D3DRS_NORMALIZENORMALS]           = FALSE;
    rs[D3DRS_DIFFUSEMATERIALSOURCE]      = D3DMCS_COLOR1;
    rs[D3DRS_SPECULARMATERIALSOURCE]     = D3DMCS_COLOR2;
    rs[D3DRS_AMBIENTMATERIALSOURCE]      = D3DMCS_MATERIAL;
    rs[D3DRS_EMISSIVEMATERIALSOURCE]     = D3DMCS_MATERIAL;
    rs[D3DRS_VERTEXBLEND]                = D3DVBF_DISABLE;
    rs[D3DRS_CLIPPLANEENABLE]            = 0;
    rs[D3DRS_POINTSIZE]                  = 0x3F800000;
    rs[D3DRS_POINTSIZE_MIN]              = 0x3F800000;
    rs[D3DRS_POINTSPRITEENABLE]          = FALSE;
    rs[D3DRS_POINTSCALEENABLE]           = FALSE;
    rs[D3DRS_POINTSCALE_A]               = 0x3F800000;
    rs[D3DRS_POINTSCALE_B]               = 0;
    rs[D3DRS_POINTSCALE_C]               = 0;
    rs[D3DRS_MULTISAMPLEANTIALIAS]       = TRUE;
    rs[D3DRS_MULTISAMPLEMASK]            = 0xFFFFFFFF;
    rs[D3DRS_PATCHEDGESTYLE]             = D3DPATCHEDGE_DISCRETE;
    rs[D3DRS_DEBUGMONITORTOKEN]          = D3DDMT_ENABLE;
    rs[D3DRS_POINTSIZE_MAX]              = 0x42800000;

    rs[D3DRS_INDEXEDVERTEXBLENDENABLE]   = FALSE;
    rs[D3DRS_COLORWRITEENABLE]           = 0x0000000F;
    rs[D3DRS_TWEENFACTOR]                = 0;
    rs[D3DRS_BLENDOP]                    = D3DBLENDOP_ADD;
    rs[D3DRS_POSITIONDEGREE]             = D3DDEGREE_CUBIC;
    rs[D3DRS_NORMALDEGREE]               = D3DDEGREE_LINEAR;
    rs[D3DRS_SCISSORTESTENABLE]          = FALSE;
    rs[D3DRS_SLOPESCALEDEPTHBIAS]        = 0;
    rs[D3DRS_ANTIALIASEDLINEENABLE]      = FALSE;
    rs[D3DRS_MINTESSELLATIONLEVEL]       = 0x3F800000;
    rs[D3DRS_MAXTESSELLATIONLEVEL]       = 0x3F800000;
    rs[D3DRS_ADAPTIVETESS_X]             = 0;
    rs[D3DRS_ADAPTIVETESS_Y]             = 0;
    rs[D3DRS_ADAPTIVETESS_Z]             = 0x3F800000;
    rs[D3DRS_ADAPTIVETESS_W]             = 0;
    rs[D3DRS_ENABLEADAPTIVETESSELLATION] = FALSE;
    rs[D3DRS_TWOSIDEDSTENCILMODE]        = FALSE;
    rs[D3DRS_CCW_STENCILFAIL]            = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILZFAIL]           = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILPASS]            = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILFUNC]            = D3DCMP_ALWAYS;
    rs[D3DRS_COLORWRITEENABLE1]          = 0x0000000F;
    rs[D3DRS_COLORWRITEENABLE2]          = 0x0000000F;
    rs[D3DRS_COLORWRITEENABLE3]          = 0x0000000F;
    rs[D3DRS_BLENDFACTOR]                = 0xFFFFFFFF;
    rs[D3DRS_SRGBWRITEENABLE]            = FALSE;
    rs[D3DRS_DEPTHBIAS]                  = 0;
    rs[D3DRS_SEPARATEALPHABLENDENABLE]   = FALSE;
    rs[D3DRS_SRCBLENDALPHA]              = D3DBLEND_ONE;
    rs[D3DRS_DESTBLENDALPHA]             = D3DBLEND_ZERO;
    rs[D3DRS_BLENDOPALPHA]               = D3DBLENDOP_ADD;

    for (UINT i = 0; i < 8; ++i) {
        m_state.tss[i][D3DTSS_COLOROP]   = (i == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        m_state.tss[i][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        m_state.tss[i][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        m_state.tss[i][D3DTSS_ALPHAOP]   = (i == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        m_state.tss[i][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        m_state.tss[i][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        m_state.tss[i][D3DTSS_BUMPENVMAT00] = 0;
        m_state.tss[i][D3DTSS_BUMPENVMAT01] = 0;
        m_state.tss[i][D3DTSS_BUMPENVMAT10] = 0;
        m_state.tss[i][D3DTSS_BUMPENVMAT11] = 0;
        m_state.tss[i][D3DTSS_TEXCOORDINDEX]   = i;
        m_state.tss[i][D3DTSS_BUMPENVLSCALE]   = 0;
        m_state.tss[i][D3DTSS_BUMPENVLOFFSET]  = 0;
        m_state.tss[i][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
        m_state.tss[i][D3DTSS_COLORARG0] = D3DTA_CURRENT;
        m_state.tss[i][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
        m_state.tss[i][D3DTSS_RESULTARG] = D3DTA_CURRENT;
        m_state.tss[i][D3DTSS_CONSTANT]  = 0;
    }

    for (UINT i = 0; i < 8; ++i) {
        m_state.samp[i][D3DSAMP_ADDRESSU]      = D3DTADDRESS_WRAP;
        m_state.samp[i][D3DSAMP_ADDRESSV]      = D3DTADDRESS_WRAP;
        m_state.samp[i][D3DSAMP_ADDRESSW]      = D3DTADDRESS_WRAP;
        m_state.samp[i][D3DSAMP_BORDERCOLOR]   = 0x00000000;
        m_state.samp[i][D3DSAMP_MAGFILTER]     = D3DTEXF_POINT;
        m_state.samp[i][D3DSAMP_MINFILTER]     = D3DTEXF_POINT;
        m_state.samp[i][D3DSAMP_MIPFILTER]     = D3DTEXF_NONE;
        m_state.samp[i][D3DSAMP_MIPMAPLODBIAS] = 0;
        m_state.samp[i][D3DSAMP_MAXMIPLEVEL]   = 0;
        m_state.samp[i][D3DSAMP_MAXANISOTROPY] = 1;
        m_state.samp[i][D3DSAMP_SRGBTEXTURE]   = 0;
        m_state.samp[i][D3DSAMP_ELEMENTINDEX]  = 0;
        m_state.samp[i][D3DSAMP_DMAPOFFSET]    = 0;
    }
}

}
