// Passthrough wrapper around the real IDirect3DDevice9 (Backend=1).
//
// The device counterpart to WrappedIDirect3D9: every method forwards to the
// genuine runtime, so the game runs on its original renderer with MWOn12 present
// but not interfering.
//
// Resources are a deliberate exception to the wrapping rule. Textures, buffers
// and surfaces created through this device are returned to the game as the
// real interfaces, unwrapped. There is no reason to intercept them — nothing
// here inspects or modifies resource contents — and wrapping every resource
// would mean an allocation and an indirection on each one for no benefit. Note
// that this means resource pointers the game holds are the runtime's own; do
// not add logic that assumes otherwise.
//
// The parent is reference-counted properly rather than held raw: a D3D9 device
// keeps its factory alive, and games do release the IDirect3D9 while
// continuing to use the device.

#pragma once
#include <d3d9.h>
#include <atomic>

class WrappedIDirect3D9;

class WrappedIDirect3DDevice9 : public IDirect3DDevice9 {
public:
    WrappedIDirect3DDevice9(IDirect3DDevice9* pReal,
                            WrappedIDirect3D9* pParent,
                            HWND hFocusWindow,
                            D3DPRESENT_PARAMETERS* pPP);
    ~WrappedIDirect3DDevice9();

    IDirect3DDevice9* Real() const { return m_pReal; }

    HRESULT __stdcall QueryInterface(REFIID, void**) override;
    ULONG   __stdcall AddRef() override;
    ULONG   __stdcall Release() override;

    HRESULT __stdcall Present(const RECT*, const RECT*, HWND, const RGNDATA*) override;
    HRESULT __stdcall BeginScene() override;
    HRESULT __stdcall EndScene() override;
    HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS*) override;

    HRESULT __stdcall TestCooperativeLevel() override { return m_pReal->TestCooperativeLevel(); }
    UINT    __stdcall GetAvailableTextureMem() override { return m_pReal->GetAvailableTextureMem(); }
    HRESULT __stdcall EvictManagedResources() override { return m_pReal->EvictManagedResources(); }
    HRESULT __stdcall GetDirect3D(IDirect3D9** d) override;
    HRESULT __stdcall GetDeviceCaps(D3DCAPS9* c) override { return m_pReal->GetDeviceCaps(c); }
    HRESULT __stdcall GetDisplayMode(UINT s, D3DDISPLAYMODE* m) override { return m_pReal->GetDisplayMode(s,m); }
    HRESULT __stdcall GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) override { return m_pReal->GetCreationParameters(p); }
    HRESULT __stdcall SetCursorProperties(UINT x,UINT y,IDirect3DSurface9* s) override { return m_pReal->SetCursorProperties(x,y,s); }
    void    __stdcall SetCursorPosition(int x,int y,DWORD f) override { m_pReal->SetCursorPosition(x,y,f); }
    BOOL    __stdcall ShowCursor(BOOL b) override { return m_pReal->ShowCursor(b); }
    HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* p,IDirect3DSwapChain9** sc) override { return m_pReal->CreateAdditionalSwapChain(p,sc); }
    HRESULT __stdcall GetSwapChain(UINT i,IDirect3DSwapChain9** sc) override { return m_pReal->GetSwapChain(i,sc); }
    UINT    __stdcall GetNumberOfSwapChains() override { return m_pReal->GetNumberOfSwapChains(); }
    HRESULT __stdcall GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) override { return m_pReal->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }
    HRESULT __stdcall GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override { return m_pReal->GetRasterStatus(iSwapChain, pRasterStatus); }
    HRESULT __stdcall SetDialogBoxMode(BOOL b) override { return m_pReal->SetDialogBoxMode(b); }
    void    __stdcall SetGammaRamp(UINT s,DWORD f,const D3DGAMMARAMP* r) override { m_pReal->SetGammaRamp(s,f,r); }
    void    __stdcall GetGammaRamp(UINT s,D3DGAMMARAMP* r) override { m_pReal->GetGammaRamp(s,r); }
    HRESULT __stdcall CreateTexture(UINT w,UINT h,UINT l,DWORD u,D3DFORMAT f,D3DPOOL p,IDirect3DTexture9** t,HANDLE* sh) override { return m_pReal->CreateTexture(w,h,l,u,f,p,t,sh); }
    HRESULT __stdcall CreateVolumeTexture(UINT w,UINT h,UINT d,UINT l,DWORD u,D3DFORMAT f,D3DPOOL p,IDirect3DVolumeTexture9** t,HANDLE* sh) override { return m_pReal->CreateVolumeTexture(w,h,d,l,u,f,p,t,sh); }
    HRESULT __stdcall CreateCubeTexture(UINT s,UINT l,DWORD u,D3DFORMAT f,D3DPOOL p,IDirect3DCubeTexture9** t,HANDLE* sh) override { return m_pReal->CreateCubeTexture(s,l,u,f,p,t,sh); }
    HRESULT __stdcall CreateVertexBuffer(UINT s,DWORD u,DWORD fvf,D3DPOOL p,IDirect3DVertexBuffer9** vb,HANDLE* sh) override { return m_pReal->CreateVertexBuffer(s,u,fvf,p,vb,sh); }
    HRESULT __stdcall CreateIndexBuffer(UINT s,DWORD u,D3DFORMAT f,D3DPOOL p,IDirect3DIndexBuffer9** ib,HANDLE* sh) override { return m_pReal->CreateIndexBuffer(s,u,f,p,ib,sh); }
    HRESULT __stdcall CreateRenderTarget(UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE ms,DWORD mq,BOOL l,IDirect3DSurface9** s,HANDLE* sh) override { return m_pReal->CreateRenderTarget(w,h,f,ms,mq,l,s,sh); }
    HRESULT __stdcall CreateDepthStencilSurface(UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE ms,DWORD mq,BOOL d,IDirect3DSurface9** s,HANDLE* sh) override { return m_pReal->CreateDepthStencilSurface(w,h,f,ms,mq,d,s,sh); }
    HRESULT __stdcall UpdateSurface(IDirect3DSurface9* s,const RECT* r,IDirect3DSurface9* d,const POINT* p) override { return m_pReal->UpdateSurface(s,r,d,p); }
    HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture9* s,IDirect3DBaseTexture9* d) override { return m_pReal->UpdateTexture(s,d); }
    HRESULT __stdcall GetRenderTargetData(IDirect3DSurface9* rt,IDirect3DSurface9* d) override { return m_pReal->GetRenderTargetData(rt,d); }
    HRESULT __stdcall GetFrontBufferData(UINT i,IDirect3DSurface9* s) override { return m_pReal->GetFrontBufferData(i,s); }
    HRESULT __stdcall StretchRect(IDirect3DSurface9* s,const RECT* sr,IDirect3DSurface9* d,const RECT* dr,D3DTEXTUREFILTERTYPE f) override { return m_pReal->StretchRect(s,sr,d,dr,f); }
    HRESULT __stdcall ColorFill(IDirect3DSurface9* s,const RECT* r,D3DCOLOR c) override { return m_pReal->ColorFill(s,r,c); }
    HRESULT __stdcall CreateOffscreenPlainSurface(UINT w,UINT h,D3DFORMAT f,D3DPOOL p,IDirect3DSurface9** s,HANDLE* sh) override { return m_pReal->CreateOffscreenPlainSurface(w,h,f,p,s,sh); }
    HRESULT __stdcall SetRenderTarget(DWORD i,IDirect3DSurface9* s) override { return m_pReal->SetRenderTarget(i,s); }
    HRESULT __stdcall GetRenderTarget(DWORD i,IDirect3DSurface9** s) override { return m_pReal->GetRenderTarget(i,s); }
    HRESULT __stdcall SetDepthStencilSurface(IDirect3DSurface9* s) override { return m_pReal->SetDepthStencilSurface(s); }
    HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface9** s) override { return m_pReal->GetDepthStencilSurface(s); }
    HRESULT __stdcall Clear(DWORD c,const D3DRECT* r,DWORD f,D3DCOLOR col,float z,DWORD s) override { return m_pReal->Clear(c,r,f,col,z,s); }
    HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE t,const D3DMATRIX* m) override { return m_pReal->SetTransform(t,m); }
    HRESULT __stdcall GetTransform(D3DTRANSFORMSTATETYPE t,D3DMATRIX* m) override { return m_pReal->GetTransform(t,m); }
    HRESULT __stdcall MultiplyTransform(D3DTRANSFORMSTATETYPE t,const D3DMATRIX* m) override { return m_pReal->MultiplyTransform(t,m); }
    HRESULT __stdcall SetViewport(const D3DVIEWPORT9* v) override { return m_pReal->SetViewport(v); }
    HRESULT __stdcall GetViewport(D3DVIEWPORT9* v) override { return m_pReal->GetViewport(v); }
    HRESULT __stdcall SetMaterial(const D3DMATERIAL9* m) override { return m_pReal->SetMaterial(m); }
    HRESULT __stdcall GetMaterial(D3DMATERIAL9* m) override { return m_pReal->GetMaterial(m); }
    HRESULT __stdcall SetLight(DWORD i,const D3DLIGHT9* l) override { return m_pReal->SetLight(i,l); }
    HRESULT __stdcall GetLight(DWORD i,D3DLIGHT9* l) override { return m_pReal->GetLight(i,l); }
    HRESULT __stdcall LightEnable(DWORD i,BOOL e) override { return m_pReal->LightEnable(i,e); }
    HRESULT __stdcall GetLightEnable(DWORD i,BOOL* e) override { return m_pReal->GetLightEnable(i,e); }
    HRESULT __stdcall SetClipPlane(DWORD i,const float* p) override { return m_pReal->SetClipPlane(i,p); }
    HRESULT __stdcall GetClipPlane(DWORD i,float* p) override { return m_pReal->GetClipPlane(i,p); }
    HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE s,DWORD v) override { return m_pReal->SetRenderState(s,v); }
    HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE s,DWORD* v) override { return m_pReal->GetRenderState(s,v); }
    HRESULT __stdcall CreateStateBlock(D3DSTATEBLOCKTYPE t,IDirect3DStateBlock9** sb) override { return m_pReal->CreateStateBlock(t,sb); }
    HRESULT __stdcall BeginStateBlock() override { return m_pReal->BeginStateBlock(); }
    HRESULT __stdcall EndStateBlock(IDirect3DStateBlock9** sb) override { return m_pReal->EndStateBlock(sb); }
    HRESULT __stdcall SetClipStatus(const D3DCLIPSTATUS9* s) override { return m_pReal->SetClipStatus(s); }
    HRESULT __stdcall GetClipStatus(D3DCLIPSTATUS9* s) override { return m_pReal->GetClipStatus(s); }
    HRESULT __stdcall GetTexture(DWORD s,IDirect3DBaseTexture9** t) override { return m_pReal->GetTexture(s,t); }
    HRESULT __stdcall SetTexture(DWORD s,IDirect3DBaseTexture9* t) override { return m_pReal->SetTexture(s,t); }
    HRESULT __stdcall GetTextureStageState(DWORD s,D3DTEXTURESTAGESTATETYPE t,DWORD* v) override { return m_pReal->GetTextureStageState(s,t,v); }
    HRESULT __stdcall SetTextureStageState(DWORD s,D3DTEXTURESTAGESTATETYPE t,DWORD v) override { return m_pReal->SetTextureStageState(s,t,v); }
    HRESULT __stdcall GetSamplerState(DWORD s,D3DSAMPLERSTATETYPE t,DWORD* v) override { return m_pReal->GetSamplerState(s,t,v); }
    HRESULT __stdcall SetSamplerState(DWORD s,D3DSAMPLERSTATETYPE t,DWORD v) override { return m_pReal->SetSamplerState(s,t,v); }
    HRESULT __stdcall ValidateDevice(DWORD* n) override { return m_pReal->ValidateDevice(n); }
    HRESULT __stdcall SetPaletteEntries(UINT p,const PALETTEENTRY* e) override { return m_pReal->SetPaletteEntries(p,e); }
    HRESULT __stdcall GetPaletteEntries(UINT p,PALETTEENTRY* e) override { return m_pReal->GetPaletteEntries(p,e); }
    HRESULT __stdcall SetCurrentTexturePalette(UINT p) override { return m_pReal->SetCurrentTexturePalette(p); }
    HRESULT __stdcall GetCurrentTexturePalette(UINT* p) override { return m_pReal->GetCurrentTexturePalette(p); }
    HRESULT __stdcall SetScissorRect(const RECT* r) override { return m_pReal->SetScissorRect(r); }
    HRESULT __stdcall GetScissorRect(RECT* r) override { return m_pReal->GetScissorRect(r); }
    HRESULT __stdcall SetSoftwareVertexProcessing(BOOL b) override { return m_pReal->SetSoftwareVertexProcessing(b); }
    BOOL    __stdcall GetSoftwareVertexProcessing() override { return m_pReal->GetSoftwareVertexProcessing(); }
    HRESULT __stdcall SetNPatchMode(float n) override { return m_pReal->SetNPatchMode(n); }
    float   __stdcall GetNPatchMode() override { return m_pReal->GetNPatchMode(); }
    HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE t,UINT sv,UINT pc) override { return m_pReal->DrawPrimitive(t,sv,pc); }
    HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE t,INT bv,UINT mv,UINT vc,UINT si,UINT pc) override { return m_pReal->DrawIndexedPrimitive(t,bv,mv,vc,si,pc); }
    HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE t,UINT pc,const void* d,UINT s) override { return m_pReal->DrawPrimitiveUP(t,pc,d,s); }
    HRESULT __stdcall DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t,UINT mv,UINT vc,UINT pc,const void* id,D3DFORMAT f,const void* vd,UINT vs) override { return m_pReal->DrawIndexedPrimitiveUP(t,mv,vc,pc,id,f,vd,vs); }
    HRESULT __stdcall ProcessVertices(UINT sv,UINT dv,UINT vc,IDirect3DVertexBuffer9* vb,IDirect3DVertexDeclaration9* vd,DWORD f) override { return m_pReal->ProcessVertices(sv,dv,vc,vb,vd,f); }
    HRESULT __stdcall CreateVertexDeclaration(const D3DVERTEXELEMENT9* e,IDirect3DVertexDeclaration9** d) override { return m_pReal->CreateVertexDeclaration(e,d); }
    HRESULT __stdcall SetVertexDeclaration(IDirect3DVertexDeclaration9* d) override { return m_pReal->SetVertexDeclaration(d); }
    HRESULT __stdcall GetVertexDeclaration(IDirect3DVertexDeclaration9** d) override { return m_pReal->GetVertexDeclaration(d); }
    HRESULT __stdcall SetFVF(DWORD f) override { return m_pReal->SetFVF(f); }
    HRESULT __stdcall GetFVF(DWORD* f) override { return m_pReal->GetFVF(f); }
    HRESULT __stdcall CreateVertexShader(const DWORD* t,IDirect3DVertexShader9** s) override { return m_pReal->CreateVertexShader(t,s); }
    HRESULT __stdcall SetVertexShader(IDirect3DVertexShader9* s) override { return m_pReal->SetVertexShader(s); }
    HRESULT __stdcall GetVertexShader(IDirect3DVertexShader9** s) override { return m_pReal->GetVertexShader(s); }
    HRESULT __stdcall SetVertexShaderConstantF(UINT r,const float* d,UINT c) override { return m_pReal->SetVertexShaderConstantF(r,d,c); }
    HRESULT __stdcall GetVertexShaderConstantF(UINT r,float* d,UINT c) override { return m_pReal->GetVertexShaderConstantF(r,d,c); }
    HRESULT __stdcall SetVertexShaderConstantI(UINT r,const int* d,UINT c) override { return m_pReal->SetVertexShaderConstantI(r,d,c); }
    HRESULT __stdcall GetVertexShaderConstantI(UINT r,int* d,UINT c) override { return m_pReal->GetVertexShaderConstantI(r,d,c); }
    HRESULT __stdcall SetVertexShaderConstantB(UINT r,const BOOL* d,UINT c) override { return m_pReal->SetVertexShaderConstantB(r,d,c); }
    HRESULT __stdcall GetVertexShaderConstantB(UINT r,BOOL* d,UINT c) override { return m_pReal->GetVertexShaderConstantB(r,d,c); }
    HRESULT __stdcall SetStreamSource(UINT n,IDirect3DVertexBuffer9* vb,UINT o,UINT s) override { return m_pReal->SetStreamSource(n,vb,o,s); }
    HRESULT __stdcall GetStreamSource(UINT n,IDirect3DVertexBuffer9** vb,UINT* o,UINT* s) override { return m_pReal->GetStreamSource(n,vb,o,s); }
    HRESULT __stdcall SetStreamSourceFreq(UINT n,UINT f) override { return m_pReal->SetStreamSourceFreq(n,f); }
    HRESULT __stdcall GetStreamSourceFreq(UINT n,UINT* f) override { return m_pReal->GetStreamSourceFreq(n,f); }
    HRESULT __stdcall SetIndices(IDirect3DIndexBuffer9* ib) override { return m_pReal->SetIndices(ib); }
    HRESULT __stdcall GetIndices(IDirect3DIndexBuffer9** ib) override { return m_pReal->GetIndices(ib); }
    HRESULT __stdcall CreatePixelShader(const DWORD* t,IDirect3DPixelShader9** s) override { return m_pReal->CreatePixelShader(t,s); }
    HRESULT __stdcall SetPixelShader(IDirect3DPixelShader9* s) override { return m_pReal->SetPixelShader(s); }
    HRESULT __stdcall GetPixelShader(IDirect3DPixelShader9** s) override { return m_pReal->GetPixelShader(s); }
    HRESULT __stdcall SetPixelShaderConstantF(UINT r,const float* d,UINT c) override { return m_pReal->SetPixelShaderConstantF(r,d,c); }
    HRESULT __stdcall GetPixelShaderConstantF(UINT r,float* d,UINT c) override { return m_pReal->GetPixelShaderConstantF(r,d,c); }
    HRESULT __stdcall SetPixelShaderConstantI(UINT r,const int* d,UINT c) override { return m_pReal->SetPixelShaderConstantI(r,d,c); }
    HRESULT __stdcall GetPixelShaderConstantI(UINT r,int* d,UINT c) override { return m_pReal->GetPixelShaderConstantI(r,d,c); }
    HRESULT __stdcall SetPixelShaderConstantB(UINT r,const BOOL* d,UINT c) override { return m_pReal->SetPixelShaderConstantB(r,d,c); }
    HRESULT __stdcall GetPixelShaderConstantB(UINT r,BOOL* d,UINT c) override { return m_pReal->GetPixelShaderConstantB(r,d,c); }
    HRESULT __stdcall DrawRectPatch(UINT h,const float* n,const D3DRECTPATCH_INFO* i) override { return m_pReal->DrawRectPatch(h,n,i); }
    HRESULT __stdcall DrawTriPatch(UINT h,const float* n,const D3DTRIPATCH_INFO* i) override { return m_pReal->DrawTriPatch(h,n,i); }
    HRESULT __stdcall DeletePatch(UINT h) override { return m_pReal->DeletePatch(h); }
    HRESULT __stdcall CreateQuery(D3DQUERYTYPE t,IDirect3DQuery9** q) override { return m_pReal->CreateQuery(t,q); }

private:
    IDirect3DDevice9*  m_pReal;
    WrappedIDirect3D9* m_pParent;
    HWND               m_hFocus;
    D3DPRESENT_PARAMETERS m_pp;
    std::atomic<ULONG> m_refCount;
};
