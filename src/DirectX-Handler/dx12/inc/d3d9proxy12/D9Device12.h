// The D3D12 implementation of IDirect3DDevice9.
//
// The counterpart to the D3D11 device, and the same overall shape: setters
// record D3D9 state and raise dirty bits, and the draw path reconciles the
// pipeline once immediately before submitting. Implementation is split by
// concern across D9Device12.cpp, _State, _Resource, _Draw and _Present.
//
// D3D12 moves several responsibilities from the driver to the application, and
// that is where this diverges from the D3D11 backend:
//
//   Command lists. Nothing executes when it is called. Work is recorded into a
//   command list and submitted at the end of the frame, which means anything
//   the GPU will read must stay alive until that submission has completed, not
//   merely until the call that used it returned.
//
//   Explicit synchronisation. There is no automatic protection against writing
//   a resource the GPU is still reading. Frames are pipelined with a fence and
//   per-frame allocators, and every resource has to be tracked accordingly.
//
//   Resource states. Every resource is in exactly one state, and using it in
//   another requires an explicit barrier. Getting one wrong produces undefined
//   contents rather than an error, which is why states are tracked per
//   resource and barriers are batched rather than issued ad hoc.
//
//   Descriptors. Textures are not bound; descriptors are written into heaps
//   and the heap is bound. Populating a descriptor table per draw is far too
//   slow, so tables are cached and reused.
//
// This backend has no equivalent of the D3D11 path's depth-stencil detachment
// rules and does not need them.

#pragma once

#ifndef MWON12_DX12_D9DEVICE12_H
#define MWON12_DX12_D9DEVICE12_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <core/Blitter12.h>
#include <core/DeviceContext12.h>
#include <core/FFPEmulator12.h>
#include <core/FFPShaderGen.h>
#include <core/PSOCache12.h>
#include <core/PrivateData12.h>
#include <core/RenderStateTracker.h>
#include <core/Resource12.h>
#include <core/ShaderCache12.h>
#include <d3d9proxy12/Resources12.h>
#include <d3d9proxy12/Shaders12.h>

namespace mwon12 {

using Microsoft::WRL::ComPtr;

class D9Root12;
class D9SwapChain12;

#pragma pack(push, 16)
struct Dx9EmuCB12 {
    float g_emuMisc[4];
    float g_bumpMat[8][4];
    float g_bumpScaleOff[8][4];
    float g_fogParams[4];
    float g_fogColor[4];
    float g_clipPlanes[6][4];
};
#pragma pack(pop)
static_assert(sizeof(Dx9EmuCB12) == 400, "Dx9EmuCB12 must match the translator's b1");

#pragma pack(push, 16)
struct VSConstants12 {
    float c[256][4];
    int   ic[16][4];
    UINT  bc[4][4];
};
struct PSConstants12 {
    float c[224][4];
    int   ic[16][4];
    UINT  bc[4][4];
};
#pragma pack(pop)

class D9Device12 final : public IDirect3DDevice9Ex {
public:
    D9Device12(std::unique_ptr<DeviceContext12> ctx, D9Root12* root,
               DWORD behaviourFlags, const D3DPRESENT_PARAMETERS& pp,
               bool isEx) noexcept;
    D9Device12(const D9Device12&)            = delete;
    D9Device12& operator=(const D9Device12&) = delete;

    HRESULT FinishInit() noexcept;

    // Binds the back buffer and hands the finished frame to any plugin that
    // asked for it. Called from Present, immediately before the swap.
    void RunPresentPlugins() noexcept;

    [[nodiscard]] DeviceContext12* Ctx() const noexcept { return m_ctx.get(); }
    [[nodiscard]] ShaderCache12&   Shaders()  noexcept { return m_shaders; }
    [[nodiscard]] PSOCache12&      Pipelines() noexcept { return m_psoCache; }
    [[nodiscard]] D9RenderState&   State()    noexcept { return m_rst.State(); }
    [[nodiscard]] FFPFixedState&   FixedState() noexcept { return m_fixed; }

    HRESULT GenerateMips(TextureStorage12* storage) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override;
    UINT    STDMETHODCALLTYPE GetAvailableTextureMem() override;
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override;
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9**) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9*) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT, D3DDISPLAYMODE*) override;
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS*) override;
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT, UINT, IDirect3DSurface9*) override;
    void    STDMETHODCALLTYPE SetCursorPosition(int, int, DWORD) override;
    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL) override;
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**) override;
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT, IDirect3DSwapChain9**) override;
    UINT    STDMETHODCALLTYPE GetNumberOfSwapChains() override;
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS*) override;
    HRESULT STDMETHODCALLTYPE Present(CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9**) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT, D3DRASTER_STATUS*) override;
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL) override;
    void    STDMETHODCALLTYPE SetGammaRamp(UINT, DWORD, CONST D3DGAMMARAMP*) override;
    void    STDMETHODCALLTYPE GetGammaRamp(UINT, D3DGAMMARAMP*) override;
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9*, CONST RECT*, IDirect3DSurface9*, CONST POINT*) override;
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9*, IDirect3DBaseTexture9*) override;
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9*, IDirect3DSurface9*) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT, IDirect3DSurface9*) override;
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9*, CONST RECT*, IDirect3DSurface9*, CONST RECT*, D3DTEXTUREFILTERTYPE) override;
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9*, CONST RECT*, D3DCOLOR) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD, IDirect3DSurface9*) override;
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD, IDirect3DSurface9**) override;
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9*) override;
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9**) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override;
    HRESULT STDMETHODCALLTYPE EndScene() override;
    HRESULT STDMETHODCALLTYPE Clear(DWORD, CONST D3DRECT*, DWORD, D3DCOLOR, float, DWORD) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX*) override;
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX*) override;
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX*) override;
    HRESULT STDMETHODCALLTYPE SetViewport(CONST D3DVIEWPORT9*) override;
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9*) override;
    HRESULT STDMETHODCALLTYPE SetMaterial(CONST D3DMATERIAL9*) override;
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9*) override;
    HRESULT STDMETHODCALLTYPE SetLight(DWORD, CONST D3DLIGHT9*) override;
    HRESULT STDMETHODCALLTYPE GetLight(DWORD, D3DLIGHT9*) override;
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD, BOOL) override;
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD, BOOL*) override;
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD, CONST float*) override;
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD, float*) override;
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE, DWORD*) override;
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**) override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9**) override;
    HRESULT STDMETHODCALLTYPE SetClipStatus(CONST D3DCLIPSTATUS9*) override;
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9*) override;
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD, IDirect3DBaseTexture9**) override;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD, IDirect3DBaseTexture9*) override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD*) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD*) override;
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD) override;
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD*) override;
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT, CONST PALETTEENTRY*) override;
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT, PALETTEENTRY*) override;
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT) override;
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT*) override;
    HRESULT STDMETHODCALLTYPE SetScissorRect(CONST RECT*) override;
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT*) override;
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL) override;
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() override;
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float) override;
    float   STDMETHODCALLTYPE GetNPatchMode() override;
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE, UINT, UINT) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) override;
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, CONST void*, UINT) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE, UINT, UINT, UINT, CONST void*, D3DFORMAT, CONST void*, UINT) override;
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer9*, IDirect3DVertexDeclaration9*, DWORD) override;
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(CONST D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9**) override;
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9*) override;
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9**) override;
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD) override;
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD*) override;
    HRESULT STDMETHODCALLTYPE CreateVertexShader(CONST DWORD*, IDirect3DVertexShader9**) override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9*) override;
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9**) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT, CONST float*, UINT) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT, float*, UINT) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT, CONST int*, UINT) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT, int*, UINT) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT, CONST BOOL*, UINT) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT, BOOL*, UINT) override;
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT, IDirect3DVertexBuffer9*, UINT, UINT) override;
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT, IDirect3DVertexBuffer9**, UINT*, UINT*) override;
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT, UINT) override;
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT, UINT*) override;
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9*) override;
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9**) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(CONST DWORD*, IDirect3DPixelShader9**) override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9*) override;
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9**) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT, CONST float*, UINT) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT, float*, UINT) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT, CONST int*, UINT) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT, int*, UINT) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT, CONST BOOL*, UINT) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT, BOOL*, UINT) override;
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, CONST float*, CONST D3DRECTPATCH_INFO*) override;
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, CONST float*, CONST D3DTRIPATCH_INFO*) override;
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) override;
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE, IDirect3DQuery9**) override;

    HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT, UINT, float*, float*) override;
    HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9*, IDirect3DSurface9*, IDirect3DVertexBuffer9*, UINT, IDirect3DVertexBuffer9*, D3DCOMPOSERECTSOP, int, int) override;
    HRESULT STDMETHODCALLTYPE PresentEx(CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT*) override;
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) override;
    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT) override;
    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9**, UINT32) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT*) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*, DWORD) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**, HANDLE*, DWORD) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*, DWORD) override;
    HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*) override;
    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT, D3DDISPLAYMODEEX*, D3DDISPLAYROTATION*) override;

    void CaptureInto(D9StateBlock12::Snapshot& s) noexcept;

    void ApplyFrom(const D9StateBlock12::Snapshot& s,
                   D3DSTATEBLOCKTYPE type) noexcept;

private:
    ~D9Device12();

    HRESULT PreDrawFlush(D3DPRIMITIVETYPE primType, bool indexed) noexcept;
    HRESULT ResolveShaders(const CompiledShader12** outVS,
                           const CompiledShader12** outPS) noexcept;
    HRESULT ResolveDescriptorTables(D3D12_GPU_DESCRIPTOR_HANDLE* outSrv,
                                    D3D12_GPU_DESCRIPTOR_HANDLE* outSampler) noexcept;
    HRESULT UploadConstants() noexcept;
    void    BuildPipelineKey(PipelineKey12& key, D3DPRIMITIVETYPE primType,
                             const CompiledShader12& vs,
                             const CompiledShader12* ps) noexcept;
    void    ApplyRenderTargets(std::vector<D3D12_RESOURCE_BARRIER>& barriers) noexcept;

    [[nodiscard]] bool DepthStencilWritesEnabled() const noexcept;
    [[nodiscard]] D3D12_RESOURCE_STATES DepthStateFor(bool readOnly) noexcept;
    void    ApplyViewportScissor() noexcept;

    const D3DVERTEXELEMENT9* EffectiveDeclaration() noexcept;

    HRESULT BuildFanIndices(UINT primCount, UINT startVertex,
                            D3D12_INDEX_BUFFER_VIEW* outView) noexcept;

    HRESULT BindDefaultTargets() noexcept;
    void    ReleaseImplicitSurfaces() noexcept;

    std::unique_ptr<DeviceContext12> m_ctx;
    D9Root12*                        m_root{ nullptr };
    std::atomic<ULONG>               m_refCount{ 1 };
    DWORD                            m_behaviourFlags{ 0 };
    D3DPRESENT_PARAMETERS            m_pp{};
    bool                             m_isEx{ false };
    bool                             m_inScene{ false };
    PrivateDataStore                 m_privateData;

    BOOL            m_cursorVisible{ FALSE };
    D3DGAMMARAMP    m_gammaRamp{};
    BOOL            m_softwareVP{ FALSE };
    float           m_nPatchMode{ 0.0f };
    D3DCLIPSTATUS9  m_clipStatus{};
    UINT            m_maxFrameLatency{ 3 };

    RenderStateTracker m_rst;
    FFPFixedState      m_fixed;
    ShaderCache12      m_shaders;
    PSOCache12         m_psoCache;
    FFPEmulator12      m_ffp;
    Blitter12          m_blitter;

    D9Surface12* m_renderTargets[kMaxRenderTargets12]{};
    D9Surface12* m_depthStencil{ nullptr };

    D9Surface12* m_implicitBackBuffer{ nullptr };
    D9Surface12* m_implicitDepth{ nullptr };

    D3DVIEWPORT9 m_viewport{};
    RECT         m_scissor{};

    DWORD                        m_fvf{ 0 };
    D9VertexDecl12*              m_decl{ nullptr };
    D9VertexShader12*            m_vs{ nullptr };
    D9PixelShader12*             m_ps{ nullptr };
    UINT                         m_streamFreq[16]{};
    D3DFORMAT                    m_ibFormat{ D3DFMT_INDEX16 };

    VSConstants12 m_vsConst{};
    PSConstants12 m_psConst{};
    Dx9EmuCB12    m_emu{};
    float         m_clipPlanes[6][4]{};
    FFPConstantData m_ffpConst{};

    bool m_vsConstDirty{ true };
    bool m_psConstDirty{ true };
    bool m_emuDirty{ true };
    bool m_ffpConstDirty{ true };

    D3D12_GPU_VIRTUAL_ADDRESS m_vsConstGpu{ 0 };
    D3D12_GPU_VIRTUAL_ADDRESS m_psConstGpu{ 0 };
    D3D12_GPU_VIRTUAL_ADDRESS m_emuGpu{ 0 };
    D3D12_GPU_VIRTUAL_ADDRESS m_ffpGpu{ 0 };
    UINT64                    m_constFrame{ UINT64(-1) };

    ID3D12PipelineState*        m_boundPso{ nullptr };
    D3D12_PRIMITIVE_TOPOLOGY    m_boundTopology{ D3D_PRIMITIVE_TOPOLOGY_UNDEFINED };
    D3D12_GPU_DESCRIPTOR_HANDLE m_boundSrvTable{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_boundSamplerTable{};
    UINT                        m_boundStencilRef{ UINT(-1) };
    DWORD                       m_boundBlendFactor{ 0xFFFFFFFF };
    bool                        m_targetsDirty{ true };
    bool                        m_dsvReadOnlyBound{ false };

    std::vector<D3D12_RESOURCE_BARRIER>   m_barrierScratch;

    struct InputLayout12 {
        std::vector<std::string>              names;
        std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
    };
    std::unordered_map<uint64_t, std::unique_ptr<InputLayout12>> m_inputLayouts;

    const InputLayout12* ResolveInputLayout(const CompiledShader12& vs) noexcept;
    D3DVERTEXELEMENT9                     m_fvfDecl[MAXD3DDECLLENGTH + 1]{};
    DWORD                                 m_fvfDeclCached{ 0xFFFFFFFF };

    Resource12 m_zeroStream;

    D9StateBlock12* m_recordingBlock{ nullptr };

    uint64_t m_drawCount{ 0 };
    uint64_t m_posTDraws{ 0 };
    uint64_t m_ffpDraws{ 0 };
    uint64_t m_drawCalls{ 0 };
    uint64_t m_frameCount{ 0 };

    friend class D9StateBlock12;
    friend class D9Query12;
};

}

#endif
