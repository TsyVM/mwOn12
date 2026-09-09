// The D3D12 shader and vertex declaration proxies.
//
// As in the D3D11 backend, these hold the game's original D3D9 bytecode and
// defer translation until a draw needs it, because the translation of a pixel
// shader depends on render state that is not known at creation time and
// because a game may create shaders it never uses.
//
// The vertex declaration keeps its D3D9 element array. Under D3D12 the input
// layout is part of the pipeline state object rather than a separate object,
// so it is resolved when the pipeline state is built and cached with it.

#pragma once

#ifndef MWON12_DX12_SHADERS12_H
#define MWON12_DX12_SHADERS12_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <vector>

#include <core/FFPShaderGen.h>
#include <core/PrivateData12.h>
#include <core/RenderStateTracker.h>
#include <core/ShaderCache12.h>

namespace mwon12 {

using Microsoft::WRL::ComPtr;

class D9Device12;

class D9VertexShader12 final : public IDirect3DVertexShader9 {
public:
    static HRESULT Create(D9Device12* dev, const DWORD* function,
                          D9VertexShader12** out) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override;
    HRESULT STDMETHODCALLTYPE GetFunction(void* pData, UINT* pSizeOfData) override;

    [[nodiscard]] const DWORD* Bytecode() const noexcept { return m_code.data(); }
    [[nodiscard]] size_t       ByteSize() const noexcept { return m_code.size() * sizeof(DWORD); }

private:
    explicit D9VertexShader12(D9Device12* dev) noexcept : m_dev(dev) {}
    ~D9VertexShader12() = default;

    D9Device12*        m_dev{ nullptr };
    std::atomic<ULONG> m_ref{ 1 };
    std::vector<DWORD> m_code;
};

class D9PixelShader12 final : public IDirect3DPixelShader9 {
public:
    static HRESULT Create(D9Device12* dev, const DWORD* function,
                          D9PixelShader12** out) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override;
    HRESULT STDMETHODCALLTYPE GetFunction(void* pData, UINT* pSizeOfData) override;

    [[nodiscard]] const DWORD* Bytecode() const noexcept { return m_code.data(); }
    [[nodiscard]] size_t       ByteSize() const noexcept { return m_code.size() * sizeof(DWORD); }

private:
    explicit D9PixelShader12(D9Device12* dev) noexcept : m_dev(dev) {}
    ~D9PixelShader12() = default;

    D9Device12*        m_dev{ nullptr };
    std::atomic<ULONG> m_ref{ 1 };
    std::vector<DWORD> m_code;
};

class D9StateBlock12 final : public IDirect3DStateBlock9 {
public:

    static HRESULT CreateCaptured(D9Device12* dev, D3DSTATEBLOCKTYPE type,
                                  D9StateBlock12** out) noexcept;

    static HRESULT CreateRecording(D9Device12* dev, D9StateBlock12** out) noexcept;

    [[nodiscard]] bool IsRecording() const noexcept { return m_recording; }

    void RecordRenderState(D3DRENDERSTATETYPE type, DWORD value) noexcept;
    void RecordTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) noexcept;
    void RecordSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) noexcept;
    void RecordTexture(DWORD stage, IDirect3DBaseTexture9* tex) noexcept;
    void RecordStreamSource(UINT stream, IDirect3DVertexBuffer9* vb,
                            UINT offset, UINT stride) noexcept;
    void RecordIndices(IDirect3DIndexBuffer9* ib) noexcept;
    void RecordVertexShader(IDirect3DVertexShader9* vs) noexcept;
    void RecordPixelShader(IDirect3DPixelShader9* ps) noexcept;
    void RecordVertexDeclaration(IDirect3DVertexDeclaration9* decl) noexcept;
    void RecordFVF(DWORD fvf) noexcept;
    void RecordViewport(const D3DVIEWPORT9& vp) noexcept;
    void RecordScissor(const RECT& r) noexcept;
    void RecordVSConstF(UINT start, const float* v, UINT count) noexcept;
    void RecordPSConstF(UINT start, const float* v, UINT count) noexcept;
    void RecordVSConstI(UINT start, const int* v, UINT count) noexcept;
    void RecordPSConstI(UINT start, const int* v, UINT count) noexcept;
    void RecordVSConstB(UINT start, const BOOL* v, UINT count) noexcept;
    void RecordPSConstB(UINT start, const BOOL* v, UINT count) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override;
    HRESULT STDMETHODCALLTYPE Capture() override;
    HRESULT STDMETHODCALLTYPE Apply() override;

    struct Snapshot {
        D9RenderState  rs{};
        FFPFixedState  ffp{};
        D3DVIEWPORT9   viewport{};
        RECT           scissor{};
        DWORD          fvf{ 0 };
        float          vsF[256][4]{};
        int            vsI[16][4]{};
        BOOL           vsB[16]{};
        float          psF[224][4]{};
        int            psI[16][4]{};
        BOOL           psB[16]{};
        float          clipPlanes[6][4]{};
        IDirect3DVertexDeclaration9* decl{ nullptr };
        IDirect3DVertexShader9*      vs{ nullptr };
        IDirect3DPixelShader9*       ps{ nullptr };
    };

    struct RecordedSets {
        bool viewport{ false };
        bool scissor{ false };
        bool fvf{ false };
        bool vertexDecl{ false };
        bool vertexShader{ false };
        bool pixelShader{ false };
        bool indexBuffer{ false };
        bool rs[D3DRS_BLENDOPALPHA + 1]{};
        bool tss[8][D3DTSS_CONSTANT + 1]{};
        bool samp[8][D3DSAMP_DMAPOFFSET + 1]{};
        bool textures[8]{};
        bool streams[16]{};
        bool vsConstF[256]{};
        bool psConstF[224]{};
        bool vsConstI[16]{};
        bool psConstI[16]{};
        bool vsConstB[16]{};
        bool psConstB[16]{};
    };

private:
    explicit D9StateBlock12(D9Device12* dev) noexcept : m_dev(dev) {}
    ~D9StateBlock12();

    void CaptureAll() noexcept;
    void ReleaseHeld() noexcept;

    void ApplyRecorded() noexcept;

    D9Device12*        m_dev{ nullptr };
    std::atomic<ULONG> m_ref{ 1 };
    Snapshot           m_snap;
    D3DSTATEBLOCKTYPE  m_type{ D3DSBT_ALL };
    bool               m_recording{ false };
    RecordedSets       m_recorded;
};

class D9Query12 final : public IDirect3DQuery9 {
public:
    static HRESULT Create(D9Device12* dev, D3DQUERYTYPE type,
                          D9Query12** out) noexcept;

    static HRESULT Supported(D3DQUERYTYPE type) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override;
    D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return m_type; }
    DWORD   STDMETHODCALLTYPE GetDataSize() override;
    HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags) override;
    HRESULT STDMETHODCALLTYPE GetData(void* pData, DWORD dwSize, DWORD dwGetDataFlags) override;

private:
    explicit D9Query12(D9Device12* dev, D3DQUERYTYPE type) noexcept
        : m_dev(dev), m_type(type) {}
    ~D9Query12();

    D9Device12*        m_dev{ nullptr };
    std::atomic<ULONG> m_ref{ 1 };
    D3DQUERYTYPE       m_type{ D3DQUERYTYPE_EVENT };

    ComPtr<ID3D12QueryHeap> m_heap;
    ComPtr<ID3D12Resource>  m_readback;
    UINT64                  m_fenceValue{ 0 };
    bool                    m_issued{ false };
    bool                    m_resolved{ false };
};

}

#endif
