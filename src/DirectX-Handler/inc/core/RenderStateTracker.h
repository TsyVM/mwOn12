#pragma once

#ifndef MWON12_RENDER_STATE_TRACKER_H
#define MWON12_RENDER_STATE_TRACKER_H

#include <d3d9.h>
#include <cstring>

// The authoritative shadow copy of D3D9 device state, plus dirty tracking.
//
// D3D9 is a bag of independent render states that the driver reconciles at
// draw time. D3D11 and D3D12 both want immutable state objects chosen in
// advance. Reconciling on every SetRenderState would mean rebuilding those
// objects hundreds of times per frame for state the game may overwrite before
// it draws anything, so nothing is translated on the way in: setters write
// here and raise a dirty bit, and the draw path resolves whatever is still
// dirty once, immediately before submitting.
//
// The dirty bits are grouped by destination object — blend, rasteriser,
// depth-stencil — so that changing a cull mode does not force a blend state
// rebuild.
//
// Both backends share this structure. It is also what state blocks capture
// from and restore into, and what the per-draw census reports.
namespace mwon12 {

// Arrays are sized by the largest D3D9 enumerant rather than by what the game
// is expected to use, so an out-of-range index is impossible by construction
// and no bounds check is needed on the hot path. Raw interface pointers are
// deliberate: this mirrors what the game set, it does not own anything, and
// lifetime is the resource wrapper's business.
struct D9RenderState {

    DWORD rs[D3DRS_BLENDOPALPHA + 1]{};

    DWORD tss[8][D3DTSS_CONSTANT + 1]{};

    DWORD samp[8][D3DSAMP_DMAPOFFSET + 1]{};

    IDirect3DBaseTexture9*      textures[8]{};
    IDirect3DVertexBuffer9*     streams[16]{};
    UINT                        streamStrides[16]{};
    UINT                        streamOffsets[16]{};
    IDirect3DIndexBuffer9*      indexBuffer{};
    IDirect3DVertexDeclaration9* vertexDecl{};
    IDirect3DVertexShader9*     vertexShader{};
    IDirect3DPixelShader9*      pixelShader{};

    D9RenderState() { std::memset(this, 0, sizeof(*this)); }
};

class RenderStateTracker {
public:

    enum : DWORD {
        kDirtyBlend        = 1u << 0,
        kDirtyRast         = 1u << 1,
        kDirtyDepthStencil = 1u << 2,
        kDirtyAll          = 0xFFFFFFFFu,
    };

    RenderStateTracker() noexcept;

    [[nodiscard]] const D9RenderState& State() const noexcept { return m_state; }
    [[nodiscard]]       D9RenderState& State()       noexcept { return m_state; }

    void SetRenderState(D3DRENDERSTATETYPE type, DWORD value) noexcept;
    void SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) noexcept;
    void SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) noexcept;

    [[nodiscard]] DWORD Dirty() const noexcept          { return m_dirty; }
    void ClearDirty(DWORD bits) noexcept                { m_dirty &= ~bits; }
    [[nodiscard]] UINT8 SamplerDirty() const noexcept   { return m_sampDirty; }
    void ClearSamplerDirty() noexcept                   { m_sampDirty = 0; }

    void MarkAllDirty() noexcept                        { m_dirty = kDirtyAll; m_sampDirty = 0xFF; }

    void ResetToDefaults() noexcept
    {
        m_state = D9RenderState{};
        ApplyDefaults();
        MarkAllDirty();
    }

private:
    D9RenderState m_state;
    DWORD         m_dirty{ kDirtyAll };
    UINT8         m_sampDirty{ 0xFF };

    void ApplyDefaults() noexcept;
    [[nodiscard]] static DWORD GroupOf(D3DRENDERSTATETYPE type) noexcept;
};

}

#endif
