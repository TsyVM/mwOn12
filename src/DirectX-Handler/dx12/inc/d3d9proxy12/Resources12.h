// The D3D12 resource proxies -- textures, surfaces, buffers.
//
// These implement the same D3D9 interfaces as their D3D11 counterparts, over
// Resource12 instead of D3D11 objects. Two differences drive the design.
//
// Lifetime. A resource referenced by a recorded command list must survive
// until the GPU has finished executing it, which is typically two or three
// frames after the game released its last reference. Releasing a D3D9 resource
// therefore retires it into a deferred queue rather than destroying it.
//
// Locking. There is no equivalent of D3D11's map-with-discard on a default
// resource, so a lockable resource keeps a CPU-side copy and the contents are
// moved between it and the GPU explicitly through the upload and readback
// paths.

#pragma once

#ifndef MWON12_DX12_RESOURCES12_H
#define MWON12_DX12_RESOURCES12_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <vector>

#include <core/DeviceContext12.h>
#include <core/Dx12Common.h>
#include <core/PrivateData12.h>
#include <core/Resource12.h>

namespace mwon12 {

using Microsoft::WRL::ComPtr;

class DeviceContext12;
class D9Device12;
class D9Texture12;
class D9CubeTexture12;
class D9VolumeTexture12;

struct SubresourceStage12 {
    std::vector<uint8_t> shadow;
    UINT                 rowPitch{0};
    UINT                 slicePitch{0};
    RECT                 lockedRect{};
    D3DBOX               lockedBox{};
    DWORD                lockFlags{0};
    bool                 locked{false};
    bool                 wholeResource{true};
    bool                 everWritten{false};
};

class TextureStorage12 {
public:
    TextureStorage12() noexcept = default;
    ~TextureStorage12();

    HRESULT Init(DeviceContext12* ctx, D3DRESOURCETYPE type,
                 UINT width, UINT height, UINT depth,
                 UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                 UINT faces) noexcept;

    [[nodiscard]] DeviceContext12* Ctx()      const noexcept { return m_ctx; }
    [[nodiscard]] Resource12&      Gpu()            noexcept { return m_gpu; }
    [[nodiscard]] bool             HasGpu()   const noexcept { return m_gpu.Valid(); }
    [[nodiscard]] D3DFORMAT        Format()   const noexcept { return m_format; }
    [[nodiscard]] DXGI_FORMAT      DxgiResourceFormat() const noexcept { return m_resFormat; }
    [[nodiscard]] DWORD            Usage()    const noexcept { return m_usage; }
    [[nodiscard]] D3DPOOL          Pool()     const noexcept { return m_pool; }
    [[nodiscard]] UINT             Levels()   const noexcept { return m_levels; }
    [[nodiscard]] UINT             Faces()    const noexcept { return m_faces; }
    [[nodiscard]] UINT             Width()    const noexcept { return m_width; }
    [[nodiscard]] UINT             Height()   const noexcept { return m_height; }
    [[nodiscard]] UINT             Depth()    const noexcept { return m_depth; }
    [[nodiscard]] Placement12      Placement()const noexcept { return m_placement; }
    [[nodiscard]] bool             IsDepth()  const noexcept { return m_isDepth; }

    void LevelSize(UINT level, UINT* w, UINT* h, UINT* d) const noexcept;
    [[nodiscard]] UINT Subresource(UINT face, UINT level) const noexcept;

    HRESULT LockRegion(UINT face, UINT level, const RECT* pRect, const D3DBOX* pBox,
                       DWORD flags, void** ppBits, UINT* pRowPitch, UINT* pSlicePitch) noexcept;
    HRESULT UnlockRegion(UINT face, UINT level) noexcept;
    void    AddDirty(UINT face, UINT level, const RECT* pRect) noexcept;

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Srv(bool srgb) noexcept;

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT face, UINT level, bool srgb) noexcept;

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT face, UINT level,
                                                  bool readOnly = false) noexcept;

    HRESULT FlushDirty() noexcept;

    [[nodiscard]] UINT LevelRowPitch(UINT level) const noexcept;
    [[nodiscard]] UINT LevelSlicePitch(UINT level) const noexcept;
    [[nodiscard]] SubresourceStage12& Stage(UINT face, UINT level) noexcept;

private:
    HRESULT EnsureShadow(UINT face, UINT level, bool needExistingContents) noexcept;
    HRESULT UploadStage(UINT face, UINT level, const RECT* rect) noexcept;

    DeviceContext12* m_ctx{ nullptr };
    Resource12       m_gpu;

    D3DRESOURCETYPE m_type{ D3DRTYPE_TEXTURE };
    D3DFORMAT       m_format{ D3DFMT_UNKNOWN };
    DXGI_FORMAT     m_resFormat{ DXGI_FORMAT_UNKNOWN };
    DXGI_FORMAT     m_srvFormat{ DXGI_FORMAT_UNKNOWN };
    DXGI_FORMAT     m_srvSrgbFormat{ DXGI_FORMAT_UNKNOWN };
    DXGI_FORMAT     m_rtvFormat{ DXGI_FORMAT_UNKNOWN };
    DXGI_FORMAT     m_dsvFormat{ DXGI_FORMAT_UNKNOWN };
    DWORD           m_usage{ 0 };
    D3DPOOL         m_pool{ D3DPOOL_MANAGED };
    Placement12     m_placement{ Placement12::GpuOnly };
    UINT            m_width{ 0 }, m_height{ 0 }, m_depth{ 1 };
    UINT            m_levels{ 1 }, m_faces{ 1 };
    bool            m_isDepth{ false };
    bool            m_expandedLuminance{ false };

    std::vector<SubresourceStage12> m_stages;
    std::vector<bool>               m_dirty;

    D3D12_CPU_DESCRIPTOR_HANDLE m_srv{ SIZE_T(-1) };
    D3D12_CPU_DESCRIPTOR_HANDLE m_srvSrgb{ SIZE_T(-1) };
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_rtv;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_rtvSrgb;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_dsv;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_dsvReadOnly;
};

class D9Surface12 final : public IDirect3DSurface9 {
public:

    static HRESULT CreateStandalone(D9Device12* dev, UINT w, UINT h,
                                    D3DFORMAT fmt, DWORD usage, D3DPOOL pool,
                                    BOOL lockable, D9Surface12** out) noexcept;

    static HRESULT CreateBackBuffer(D9Device12* dev, D9Surface12** out) noexcept;

    D9Surface12(D9Device12* dev, IDirect3DBaseTexture9* container,
                TextureStorage12* storage, UINT face, UINT level) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD) override;
    DWORD   STDMETHODCALLTYPE GetPriority() override;
    void    STDMETHODCALLTYPE PreLoad() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;

    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** ppContainer) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect() override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC* phdc) override;
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override;

    [[nodiscard]] TextureStorage12* Storage() const noexcept { return m_storage; }
    [[nodiscard]] Resource12*       Gpu()     const noexcept;
    [[nodiscard]] UINT              Face()    const noexcept { return m_face; }
    [[nodiscard]] UINT              Level()   const noexcept { return m_level; }
    [[nodiscard]] UINT              Subresource() const noexcept;
    [[nodiscard]] UINT              Width()   const noexcept { return m_width; }
    [[nodiscard]] UINT              Height()  const noexcept { return m_height; }
    [[nodiscard]] D3DFORMAT         Format()  const noexcept { return m_format; }
    [[nodiscard]] bool              IsBackBuffer() const noexcept { return m_backBufferIndex >= 0; }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Rtv(bool srgb) noexcept;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Dsv(bool readOnly = false) noexcept;

    void AppendTransition(std::vector<D3D12_RESOURCE_BARRIER>& out,
                          D3D12_RESOURCE_STATES after) noexcept;

    struct OwnedDeleter {
        void operator()(D9Surface12* s) const noexcept { delete s; }
    };
    using Owned = std::unique_ptr<D9Surface12, OwnedDeleter>;

private:
    D9Surface12(D9Device12* dev) noexcept;
    ~D9Surface12();

    void SyncBackBuffer() noexcept;

    [[nodiscard]] UINT CurrentBackBufferIndex() const noexcept;

    D9Device12*            m_dev{ nullptr };
    IDirect3DBaseTexture9* m_container{ nullptr };
    TextureStorage12*      m_storage{ nullptr };
    std::unique_ptr<TextureStorage12> m_ownStorage;
    std::atomic<ULONG>     m_ref{ 1 };
    UINT                   m_face{ 0 };
    UINT                   m_level{ 0 };
    UINT                   m_width{ 0 }, m_height{ 0 };
    D3DFORMAT              m_format{ D3DFMT_UNKNOWN };
    DWORD                  m_usage{ 0 };
    D3DPOOL                m_pool{ D3DPOOL_DEFAULT };

    int                    m_backBufferIndex{ -1 };
    Resource12             m_backBufferRes;
    UINT                   m_bbAdopted{ UINT(-1) };
    D3D12_CPU_DESCRIPTOR_HANDLE m_bbRtv[k_dx12FrameCount];

    bool                   m_lockable{ false };
    PrivateDataStore       m_privateData;
    SubresourceStage12     m_bbStage;
};

class D9BaseTexture12 {
public:
    virtual ~D9BaseTexture12() = default;
    [[nodiscard]] virtual TextureStorage12* Storage() noexcept = 0;
};

class D9Texture12 final : public IDirect3DTexture9, public D9BaseTexture12 {
public:
    static HRESULT Create(D9Device12* dev, UINT w, UINT h, UINT levels, DWORD usage,
                          D3DFORMAT fmt, D3DPOOL pool, D9Texture12** out) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD) override;
    DWORD   STDMETHODCALLTYPE GetPriority() override;
    void    STDMETHODCALLTYPE PreLoad() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;
    DWORD   STDMETHODCALLTYPE SetLOD(DWORD) override;
    DWORD   STDMETHODCALLTYPE GetLOD() override;
    DWORD   STDMETHODCALLTYPE GetLevelCount() override;
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override;
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override;
    void    STDMETHODCALLTYPE GenerateMipSubLevels() override;
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9** ppSurfaceLevel) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(CONST RECT* pDirtyRect) override;

    [[nodiscard]] TextureStorage12* Storage() noexcept override { return &m_storage; }
    void ReleaseSubSurface() noexcept { m_ref.fetch_sub(1, std::memory_order_acq_rel); }
    ULONG AddRefFromSubSurface() noexcept { return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

private:
    explicit D9Texture12(D9Device12* dev) noexcept;
    ~D9Texture12();

    D9Device12*        m_dev{ nullptr };
    std::atomic<ULONG> m_ref{ 1 };
    TextureStorage12   m_storage;
    DWORD              m_lod{ 0 };
    D3DTEXTUREFILTERTYPE m_autoGenFilter{ D3DTEXF_LINEAR };
    std::vector<D9Surface12::Owned>          m_surfaces;
    PrivateDataStore   m_privateData;
};

class D9CubeTexture12 final : public IDirect3DCubeTexture9, public D9BaseTexture12 {
public:
    static HRESULT Create(D9Device12* dev, UINT edge, UINT levels, DWORD usage,
                          D3DFORMAT fmt, D3DPOOL pool, D9CubeTexture12** out) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD) override;
    DWORD   STDMETHODCALLTYPE GetPriority() override;
    void    STDMETHODCALLTYPE PreLoad() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;
    DWORD   STDMETHODCALLTYPE SetLOD(DWORD) override;
    DWORD   STDMETHODCALLTYPE GetLOD() override;
    DWORD   STDMETHODCALLTYPE GetLevelCount() override;
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override;
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override;
    void    STDMETHODCALLTYPE GenerateMipSubLevels() override;
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES Face, UINT Level, IDirect3DSurface9** ppSurfaceLevel) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES Face, UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES Face, UINT Level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES Face, CONST RECT* pDirtyRect) override;

    [[nodiscard]] TextureStorage12* Storage() noexcept override { return &m_storage; }
    void ReleaseSubSurface() noexcept { m_ref.fetch_sub(1, std::memory_order_acq_rel); }
    ULONG AddRefFromSubSurface() noexcept { return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }

private:
    explicit D9CubeTexture12(D9Device12* dev) noexcept;
    ~D9CubeTexture12();

    D9Device12*        m_dev{ nullptr };
    std::atomic<ULONG> m_ref{ 1 };
    TextureStorage12   m_storage;
    DWORD              m_lod{ 0 };
    D3DTEXTUREFILTERTYPE m_autoGenFilter{ D3DTEXF_LINEAR };
    std::vector<D9Surface12::Owned>          m_surfaces;
    PrivateDataStore   m_privateData;
};

// One mip level of a volume texture, handed out by GetVolumeLevel. Unlike
// IDirect3DSurface9 this derives straight from IUnknown, so there is no
// SetPriority / GetType / PreLoad on it. Owned by the volume texture and
// reference-counted through it, the same way D9Surface12 levels are.
class D9Volume12 final : public IDirect3DVolume9 {
public:
    D9Volume12(D9Device12* dev, IDirect3DVolumeTexture9* container,
               TextureStorage12* storage, UINT level) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void** ppContainer) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX* pLockedVolume,
                                      CONST D3DBOX* pBox, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockBox() override;

    struct OwnedDeleter {
        void operator()(D9Volume12* v) const noexcept { delete v; }
    };
    using Owned = std::unique_ptr<D9Volume12, OwnedDeleter>;

private:
    ~D9Volume12();

    D9Device12*              m_dev{ nullptr };
    IDirect3DVolumeTexture9* m_container{ nullptr };
    TextureStorage12*        m_storage{ nullptr };
    std::atomic<ULONG>       m_ref{ 1 };
    UINT                     m_level{ 0 };
    PrivateDataStore         m_privateData;
};

class D9VolumeTexture12 final : public IDirect3DVolumeTexture9, public D9BaseTexture12 {
public:
    static HRESULT Create(D9Device12* dev, UINT w, UINT h, UINT d, UINT levels,
                          DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                          D9VolumeTexture12** out) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD) override;
    DWORD   STDMETHODCALLTYPE GetPriority() override;
    void    STDMETHODCALLTYPE PreLoad() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;
    DWORD   STDMETHODCALLTYPE SetLOD(DWORD) override;
    DWORD   STDMETHODCALLTYPE GetLOD() override;
    DWORD   STDMETHODCALLTYPE GetLevelCount() override;
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override;
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override;
    void    STDMETHODCALLTYPE GenerateMipSubLevels() override;
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DVOLUME_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT Level, IDirect3DVolume9** ppVolumeLevel) override;
    HRESULT STDMETHODCALLTYPE LockBox(UINT Level, D3DLOCKED_BOX* pLockedVolume, CONST D3DBOX* pBox, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT Level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyBox(CONST D3DBOX* pDirtyBox) override;

    [[nodiscard]] TextureStorage12* Storage() noexcept override { return &m_storage; }

private:
    explicit D9VolumeTexture12(D9Device12* dev) noexcept;
    ~D9VolumeTexture12();

    D9Device12*        m_dev{ nullptr };
    std::atomic<ULONG> m_ref{ 1 };
    TextureStorage12   m_storage;
    DWORD              m_lod{ 0 };
    D3DTEXTUREFILTERTYPE m_autoGenFilter{ D3DTEXF_LINEAR };
    PrivateDataStore   m_privateData;
    std::vector<D9Volume12::Owned> m_volumes;
};

class D9BufferBase12 {
public:
    [[nodiscard]] Resource12&  Gpu() noexcept { return m_gpu; }
    [[nodiscard]] UINT         ByteSize()  const noexcept { return m_size; }
    [[nodiscard]] DWORD        Usage()     const noexcept { return m_usage; }
    [[nodiscard]] D3DPOOL      Pool()      const noexcept { return m_pool; }

    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS CurrentGpuAddress() const noexcept;

    HRESULT FlushPending() noexcept;

protected:
    HRESULT InitBuffer(DeviceContext12* ctx, UINT bytes, DWORD usage,
                       D3DPOOL pool, const wchar_t* name) noexcept;
    HRESULT LockImpl(UINT offset, UINT size, void** ppbData, DWORD flags) noexcept;
    HRESULT UnlockImpl() noexcept;

    DeviceContext12*     m_ctx{ nullptr };
    Resource12           m_gpu;
    std::vector<uint8_t> m_shadow;
    UINT                 m_size{ 0 };
    DWORD                m_usage{ 0 };
    D3DPOOL              m_pool{ D3DPOOL_DEFAULT };
    Placement12          m_placement{ Placement12::GpuOnly };

    D3D12_GPU_VIRTUAL_ADDRESS m_renameGpu{ 0 };
    uint8_t*                  m_renameCpu{ nullptr };
    UINT64                    m_renameFrame{ UINT64(-1) };

    UINT   m_lockOffset{ 0 };
    UINT   m_lockSize{ 0 };
    DWORD  m_lockFlags{ 0 };
    bool   m_locked{ false };
};

class D9VertexBuffer12 final : public IDirect3DVertexBuffer9, public D9BufferBase12 {
public:
    static HRESULT Create(D9Device12* dev, UINT length, DWORD usage, DWORD fvf,
                          D3DPOOL pool, D9VertexBuffer12** out) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD) override;
    DWORD   STDMETHODCALLTYPE GetPriority() override;
    void    STDMETHODCALLTYPE PreLoad() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;
    HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE Unlock() override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* pDesc) override;

    [[nodiscard]] DWORD FVF() const noexcept { return m_fvf; }

private:
    explicit D9VertexBuffer12(D9Device12* dev) noexcept;
    ~D9VertexBuffer12();

    D9Device12*        m_dev{ nullptr };
    std::atomic<ULONG> m_ref{ 1 };
    DWORD              m_fvf{ 0 };
    PrivateDataStore   m_privateData;
};

class D9IndexBuffer12 final : public IDirect3DIndexBuffer9, public D9BufferBase12 {
public:
    static HRESULT Create(D9Device12* dev, UINT length, DWORD usage, D3DFORMAT fmt,
                          D3DPOOL pool, D9IndexBuffer12** out) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD) override;
    DWORD   STDMETHODCALLTYPE GetPriority() override;
    void    STDMETHODCALLTYPE PreLoad() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;
    HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE Unlock() override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* pDesc) override;

    [[nodiscard]] D3DFORMAT IndexFormat() const noexcept { return m_fmt; }
    [[nodiscard]] DXGI_FORMAT DxgiIndexFormat() const noexcept
    {
        return m_fmt == D3DFMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    }

private:
    explicit D9IndexBuffer12(D9Device12* dev) noexcept;
    ~D9IndexBuffer12();

    D9Device12*        m_dev{ nullptr };
    std::atomic<ULONG> m_ref{ 1 };
    D3DFORMAT          m_fmt{ D3DFMT_INDEX16 };
    PrivateDataStore   m_privateData;
};

class D9VertexDecl12 final : public IDirect3DVertexDeclaration9 {
public:
    static HRESULT Create(D9Device12* dev, const D3DVERTEXELEMENT9* elements,
                          D9VertexDecl12** out) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override;
    HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9* pElement, UINT* pNumElements) override;

    [[nodiscard]] const std::vector<D3DVERTEXELEMENT9>& Elements() const noexcept { return m_elements; }

    [[nodiscard]] uint64_t Hash() const noexcept { return m_hash; }

private:
    explicit D9VertexDecl12(D9Device12* dev) noexcept;
    ~D9VertexDecl12();

    D9Device12*                    m_dev{ nullptr };
    std::atomic<ULONG>             m_ref{ 1 };
    std::vector<D3DVERTEXELEMENT9> m_elements;
    uint64_t                       m_hash{ 0 };
};

}

#endif
