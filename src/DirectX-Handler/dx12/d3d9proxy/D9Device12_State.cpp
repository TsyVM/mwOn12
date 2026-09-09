// The D3D12 render, texture stage and sampler state setters.
//
// Record into the shadow state and raise dirty bits; nothing is translated
// here. The draw path resolves everything into a pipeline state object once,
// immediately before submitting.
//
// While a state block is open, a change is recorded into the block without
// also being applied to the device. The original runtime's behaviour here is
// not documented unambiguously, so StateBlockRecordOnly=0 switches to the
// other reading, where the change is applied as well.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy12/D9Device12.h>
#include <core/Log.h>

#include <algorithm>
#include <cstring>

namespace mwon12 {

namespace {

template <class T, class U>
void SwapSlot(T*& slot, U* want) noexcept
{
    auto* p = static_cast<T*>(want);
    if (slot == p) return;
    if (p) p->AddRef();
    if (slot) slot->Release();
    slot = p;
}

}

HRESULT STDMETHODCALLTYPE D9Device12::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{

    if (State > D3DRS_BLENDOPALPHA) return D3D_OK;
    if (m_recordingBlock) { m_recordingBlock->RecordRenderState(State, Value); return D3D_OK; }
    m_rst.SetRenderState(State, Value);

    switch (State) {
    case D3DRS_ALPHAREF:
        m_emu.g_emuMisc[2] = float(Value & 0xFF) / 255.0f;
        m_emuDirty = true;
        break;
    case D3DRS_FOGSTART:
    case D3DRS_FOGEND:
    case D3DRS_FOGDENSITY:
    case D3DRS_FOGCOLOR:
        m_emuDirty      = true;
        m_ffpConstDirty = true;
        break;
    case D3DRS_AMBIENT: {
        m_ffpConst.GlobalAmbient[0] = ((Value >> 16) & 0xFF) / 255.0f;
        m_ffpConst.GlobalAmbient[1] = ((Value >>  8) & 0xFF) / 255.0f;
        m_ffpConst.GlobalAmbient[2] = ((Value      ) & 0xFF) / 255.0f;
        m_ffpConst.GlobalAmbient[3] = ((Value >> 24) & 0xFF) / 255.0f;
        m_ffpConstDirty = true;
        break;
    }
    case D3DRS_TEXTUREFACTOR:
        m_ffpConstDirty = true;
        break;
    default:
        break;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue)
{
    if (!pValue) return D3DERR_INVALIDCALL;
    if (State > D3DRS_BLENDOPALPHA) { *pValue = 0; return D3D_OK; }
    *pValue = m_rst.State().rs[State];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetTextureStageState(
    DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{

    if (m_recordingBlock) { m_recordingBlock->RecordTextureStageState(Stage, Type, Value); return D3D_OK; }
    m_rst.SetTextureStageState(Stage, Type, Value);
    if (Stage >= 8 || Type > D3DTSS_CONSTANT) return D3D_OK;

    switch (Type) {
    case D3DTSS_BUMPENVMAT00: std::memcpy(&m_emu.g_bumpMat[Stage][0], &Value, 4); m_emuDirty = true; break;
    case D3DTSS_BUMPENVMAT10: std::memcpy(&m_emu.g_bumpMat[Stage][1], &Value, 4); m_emuDirty = true; break;
    case D3DTSS_BUMPENVMAT01: std::memcpy(&m_emu.g_bumpMat[Stage][2], &Value, 4); m_emuDirty = true; break;
    case D3DTSS_BUMPENVMAT11: std::memcpy(&m_emu.g_bumpMat[Stage][3], &Value, 4); m_emuDirty = true; break;
    case D3DTSS_BUMPENVLSCALE:  std::memcpy(&m_emu.g_bumpScaleOff[Stage][0], &Value, 4); m_emuDirty = true; break;
    case D3DTSS_BUMPENVLOFFSET: std::memcpy(&m_emu.g_bumpScaleOff[Stage][1], &Value, 4); m_emuDirty = true; break;
    default: break;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetTextureStageState(
    DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
{
    if (!pValue) return D3DERR_INVALIDCALL;
    if (Stage >= 8 || Type > D3DTSS_CONSTANT) { *pValue = 0; return D3D_OK; }
    *pValue = m_rst.State().tss[Stage][Type];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetSamplerState(
    DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
{
    if (Sampler >= 8 || Type > D3DSAMP_DMAPOFFSET) {

        static bool s_warned = false;
        if (!s_warned && Sampler >= 8 && Sampler < 16) {
            s_warned = true;
            DXLOG_WARN("[dx12] sampler state set on s%lu; only s0-s7 are tracked, "
                       "so this sampler keeps default filtering", Sampler);
        }
        return D3D_OK;
    }
    if (m_recordingBlock) { m_recordingBlock->RecordSamplerState(Sampler, Type, Value); return D3D_OK; }
    m_rst.SetSamplerState(Sampler, Type, Value);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetSamplerState(
    DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue)
{
    if (!pValue) return D3DERR_INVALIDCALL;
    if (Sampler >= 8 || Type > D3DSAMP_DMAPOFFSET) { *pValue = 0; return D3D_OK; }
    *pValue = m_rst.State().samp[Sampler][Type];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture)
{

    if (Stage >= 8) {
        if (pTexture) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                DXLOG_WARN("[dx12] SetTexture on stage %lu (beyond the 8 tracked "
                           "stages) - the texture is not bound for that stage",
                           Stage);
            }
        }
        return D3D_OK;
    }
    if (m_recordingBlock) { m_recordingBlock->RecordTexture(Stage, pTexture); return D3D_OK; }
    SwapSlot(m_rst.State().textures[Stage], pTexture);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture)
{
    if (!ppTexture) return D3DERR_INVALIDCALL;
    if (Stage >= 8) { *ppTexture = nullptr; return D3D_OK; }
    *ppTexture = m_rst.State().textures[Stage];
    if (*ppTexture) (*ppTexture)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetVertexShader(IDirect3DVertexShader9* pShader)
{
    if (m_recordingBlock) { m_recordingBlock->RecordVertexShader(pShader); return D3D_OK; }
    SwapSlot(m_rst.State().vertexShader, pShader);
    m_vs = static_cast<D9VertexShader12*>(m_rst.State().vertexShader);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetVertexShader(IDirect3DVertexShader9** ppShader)
{
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = m_rst.State().vertexShader;
    if (*ppShader) (*ppShader)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetPixelShader(IDirect3DPixelShader9* pShader)
{
    if (m_recordingBlock) { m_recordingBlock->RecordPixelShader(pShader); return D3D_OK; }
    SwapSlot(m_rst.State().pixelShader, pShader);
    m_ps = static_cast<D9PixelShader12*>(m_rst.State().pixelShader);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetPixelShader(IDirect3DPixelShader9** ppShader)
{
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = m_rst.State().pixelShader;
    if (*ppShader) (*ppShader)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl)
{
    if (m_recordingBlock) { m_recordingBlock->RecordVertexDeclaration(pDecl); return D3D_OK; }
    SwapSlot(m_rst.State().vertexDecl, pDecl);
    m_decl = static_cast<D9VertexDecl12*>(m_rst.State().vertexDecl);
    if (pDecl) m_fvf = 0;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl)
{
    if (!ppDecl) return D3DERR_INVALIDCALL;
    *ppDecl = m_rst.State().vertexDecl;
    if (*ppDecl) (*ppDecl)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetFVF(DWORD FVF)
{
    if (m_recordingBlock) { m_recordingBlock->RecordFVF(FVF); return D3D_OK; }
    m_fvf = FVF;
    if (FVF != 0) {

        SwapSlot(m_rst.State().vertexDecl, static_cast<IDirect3DVertexDeclaration9*>(nullptr));
        m_decl = nullptr;
    }
    m_fvfDeclCached = 0xFFFFFFFF;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetFVF(DWORD* pFVF)
{
    if (!pFVF) return D3DERR_INVALIDCALL;
    *pFVF = m_fvf;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData,
    UINT OffsetInBytes, UINT Stride)
{
    if (StreamNumber >= 16) return D3DERR_INVALIDCALL;
    if (m_recordingBlock) {
        m_recordingBlock->RecordStreamSource(StreamNumber, pStreamData,
                                             OffsetInBytes, Stride);
        return D3D_OK;
    }
    auto& st = m_rst.State();
    SwapSlot(st.streams[StreamNumber], pStreamData);
    st.streamOffsets[StreamNumber] = OffsetInBytes;
    st.streamStrides[StreamNumber] = Stride;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData,
    UINT* pOffsetInBytes, UINT* pStride)
{
    if (StreamNumber >= 16) return D3DERR_INVALIDCALL;
    const auto& st = m_rst.State();
    if (ppStreamData) {
        *ppStreamData = st.streams[StreamNumber];
        if (*ppStreamData) (*ppStreamData)->AddRef();
    }
    if (pOffsetInBytes) *pOffsetInBytes = st.streamOffsets[StreamNumber];
    if (pStride)        *pStride        = st.streamStrides[StreamNumber];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetStreamSourceFreq(UINT StreamNumber, UINT Divider)
{
    if (StreamNumber >= 16) return D3DERR_INVALIDCALL;

    if (Divider != 1 && (Divider & (D3DSTREAMSOURCE_INDEXEDDATA |
                                    D3DSTREAMSOURCE_INSTANCEDATA))) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            DXLOG_WARN("[dx12] SetStreamSourceFreq instancing (0x%08X on stream %u) "
                       "is not implemented", Divider, StreamNumber);
        }
        return D3DERR_INVALIDCALL;
    }
    m_streamFreq[StreamNumber] = Divider;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetStreamSourceFreq(UINT StreamNumber, UINT* pDivider)
{
    if (!pDivider || StreamNumber >= 16) return D3DERR_INVALIDCALL;
    *pDivider = m_streamFreq[StreamNumber];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetIndices(IDirect3DIndexBuffer9* pIndexData)
{

    if (!pIndexData && m_rst.State().indexBuffer) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            DXLOG_WARN("[dx12] SetIndices(nullptr) - the game cleared the index "
                       "buffer itself (frame %llu)",
                       (unsigned long long)m_frameCount);
        }
    }
    if (m_recordingBlock) { m_recordingBlock->RecordIndices(pIndexData); return D3D_OK; }
    SwapSlot(m_rst.State().indexBuffer, pIndexData);
    if (auto* ib = static_cast<D9IndexBuffer12*>(m_rst.State().indexBuffer))
        m_ibFormat = ib->IndexFormat();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetIndices(IDirect3DIndexBuffer9** ppIndexData)
{
    if (!ppIndexData) return D3DERR_INVALIDCALL;
    *ppIndexData = m_rst.State().indexBuffer;
    if (*ppIndexData) (*ppIndexData)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetVertexShaderConstantF(
    UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
{
    if (m_recordingBlock) { m_recordingBlock->RecordVSConstF(StartRegister, pConstantData, Vector4fCount); return D3D_OK; }
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 256) return D3D_OK;

    if (Vector4fCount > 256 - StartRegister)
        Vector4fCount = 256 - StartRegister;
    std::memcpy(m_vsConst.c[StartRegister], pConstantData,
                size_t(Vector4fCount) * 4 * sizeof(float));
    m_vsConstDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetVertexShaderConstantF(
    UINT StartRegister, float* pConstantData, UINT Vector4fCount)
{
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 256 || Vector4fCount > 256 - StartRegister)
        return D3DERR_INVALIDCALL;
    std::memcpy(pConstantData, m_vsConst.c[StartRegister],
                size_t(Vector4fCount) * 4 * sizeof(float));
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetVertexShaderConstantI(
    UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
{
    if (m_recordingBlock) { m_recordingBlock->RecordVSConstI(StartRegister, pConstantData, Vector4iCount); return D3D_OK; }
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 16 || Vector4iCount > 16 - StartRegister)
        return D3DERR_INVALIDCALL;
    std::memcpy(m_vsConst.ic[StartRegister], pConstantData,
                size_t(Vector4iCount) * 4 * sizeof(int));
    m_vsConstDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetVertexShaderConstantI(
    UINT StartRegister, int* pConstantData, UINT Vector4iCount)
{
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 16 || Vector4iCount > 16 - StartRegister)
        return D3DERR_INVALIDCALL;
    std::memcpy(pConstantData, m_vsConst.ic[StartRegister],
                size_t(Vector4iCount) * 4 * sizeof(int));
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetVertexShaderConstantB(
    UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount)
{
    if (m_recordingBlock) { m_recordingBlock->RecordVSConstB(StartRegister, pConstantData, BoolCount); return D3D_OK; }
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 16 || BoolCount > 16 - StartRegister)
        return D3DERR_INVALIDCALL;

    for (UINT i = 0; i < BoolCount; ++i) {
        const UINT reg = StartRegister + i;
        m_vsConst.bc[reg / 4][reg % 4] = pConstantData[i] ? 1u : 0u;
    }
    m_vsConstDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetVertexShaderConstantB(
    UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
{
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 16 || BoolCount > 16 - StartRegister)
        return D3DERR_INVALIDCALL;
    for (UINT i = 0; i < BoolCount; ++i) {
        const UINT reg = StartRegister + i;
        pConstantData[i] = m_vsConst.bc[reg / 4][reg % 4] ? TRUE : FALSE;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetPixelShaderConstantF(
    UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
{
    if (m_recordingBlock) { m_recordingBlock->RecordPSConstF(StartRegister, pConstantData, Vector4fCount); return D3D_OK; }
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 224) return D3D_OK;

    if (Vector4fCount > 224 - StartRegister)
        Vector4fCount = 224 - StartRegister;
    std::memcpy(m_psConst.c[StartRegister], pConstantData,
                size_t(Vector4fCount) * 4 * sizeof(float));
    m_psConstDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetPixelShaderConstantF(
    UINT StartRegister, float* pConstantData, UINT Vector4fCount)
{
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 224 || Vector4fCount > 224 - StartRegister)
        return D3DERR_INVALIDCALL;
    std::memcpy(pConstantData, m_psConst.c[StartRegister],
                size_t(Vector4fCount) * 4 * sizeof(float));
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetPixelShaderConstantI(
    UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
{
    if (m_recordingBlock) { m_recordingBlock->RecordPSConstI(StartRegister, pConstantData, Vector4iCount); return D3D_OK; }
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 16 || Vector4iCount > 16 - StartRegister)
        return D3DERR_INVALIDCALL;
    std::memcpy(m_psConst.ic[StartRegister], pConstantData,
                size_t(Vector4iCount) * 4 * sizeof(int));
    m_psConstDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetPixelShaderConstantI(
    UINT StartRegister, int* pConstantData, UINT Vector4iCount)
{
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 16 || Vector4iCount > 16 - StartRegister)
        return D3DERR_INVALIDCALL;
    std::memcpy(pConstantData, m_psConst.ic[StartRegister],
                size_t(Vector4iCount) * 4 * sizeof(int));
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::SetPixelShaderConstantB(
    UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount)
{
    if (m_recordingBlock) { m_recordingBlock->RecordPSConstB(StartRegister, pConstantData, BoolCount); return D3D_OK; }
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 16 || BoolCount > 16 - StartRegister)
        return D3DERR_INVALIDCALL;
    for (UINT i = 0; i < BoolCount; ++i) {
        const UINT reg = StartRegister + i;
        m_psConst.bc[reg / 4][reg % 4] = pConstantData[i] ? 1u : 0u;
    }
    m_psConstDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device12::GetPixelShaderConstantB(
    UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
{
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (StartRegister >= 16 || BoolCount > 16 - StartRegister)
        return D3DERR_INVALIDCALL;
    for (UINT i = 0; i < BoolCount; ++i) {
        const UINT reg = StartRegister + i;
        pConstantData[i] = m_psConst.bc[reg / 4][reg % 4] ? TRUE : FALSE;
    }
    return D3D_OK;
}

}
