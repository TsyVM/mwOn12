// The D3D12 texture behind the 2-D, cube and volume texture proxies.
//
// Owns the resource and the per-level geometry all three share: level
// dimensions, subresource indices, and the row and slice pitches used when
// uploading.
//
// The pitch arithmetic is the part worth care. D3D12 requires each row of an
// upload to start on a 256-byte boundary and each texture's placement in the
// upload buffer to be 512-byte aligned, so the pitch of data in the upload
// buffer is almost never the pitch of the texture. An upload therefore copies
// row by row, and treating it as one contiguous block corrupts every row after
// the first.
//
// Depth formats need two views of the same resource -- a typed depth-stencil
// view for writing, and a shader resource view for reading -- so the resource
// itself is created with a typeless format and the views supply the type.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy12/Resources12.h>
#include <core/DeviceContext12.h>
#include <core/FormatConverter.h>
#include <core/Log.h>

#include <algorithm>
#include <cstring>

namespace mwon12 {

namespace {

D3D12_RESOURCE_FLAGS FlagsForUsage(DWORD usage, bool isDepth, bool depthReadable) noexcept
{
    D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_NONE;
    if (usage & D3DUSAGE_RENDERTARGET)
        f |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (isDepth || (usage & D3DUSAGE_DEPTHSTENCIL)) {
        f |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        if (!depthReadable)
            f |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }
    return f;
}

D3D12_RESOURCE_STATES InitialState(DWORD usage, bool isDepth) noexcept
{
    if (isDepth || (usage & D3DUSAGE_DEPTHSTENCIL))
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (usage & D3DUSAGE_RENDERTARGET)
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    return D3D12_RESOURCE_STATE_COPY_DEST;
}

}

TextureStorage12::~TextureStorage12()
{
    if (!m_ctx) return;
    auto freeAll = [&](std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>& v, CpuDescriptorHeap& heap) {
        for (auto h : v) heap.Free(h);
        v.clear();
    };
    freeAll(m_rtv,     m_ctx->RtvHeap());
    freeAll(m_rtvSrgb, m_ctx->RtvHeap());
    freeAll(m_dsv,        m_ctx->DsvHeap());
    freeAll(m_dsvReadOnly, m_ctx->DsvHeap());
    m_ctx->SrvStaging().Free(m_srv);
    m_ctx->SrvStaging().Free(m_srvSrgb);
}

HRESULT TextureStorage12::Init(DeviceContext12* ctx, D3DRESOURCETYPE type,
                               UINT width, UINT height, UINT depth,
                               UINT levels, DWORD usage, D3DFORMAT format,
                               D3DPOOL pool, UINT faces) noexcept
{
    if (!ctx || width == 0 || height == 0) return D3DERR_INVALIDCALL;

    m_ctx    = ctx;
    m_type   = type;
    m_format = format;
    m_usage  = usage;
    m_pool   = pool;
    m_width  = width;
    m_height = height;
    m_depth  = depth ? depth : 1;
    m_faces  = faces ? faces : 1;

    m_levels = levels ? levels
                      : dx12::FullMipCount(width, height,
                                           type == D3DRTYPE_VOLUMETEXTURE ? m_depth : 1);

    m_isDepth = FormatConverter::IsDepthFormat(format) ||
                (usage & D3DUSAGE_DEPTHSTENCIL) != 0;

    FormatConverter::DepthFormatViews dv{};
    bool depthReadable = false;
    if (m_isDepth && FormatConverter::GetDepthFormatViews(format, dv)) {
        m_resFormat     = dv.resource;
        m_dsvFormat     = dv.dsv;
        m_srvFormat     = dv.srv;
        m_srvSrgbFormat = dv.srv;
        depthReadable   = dv.srvReadable;
    } else {
        const FormatMapping fm = (type == D3DRTYPE_VOLUMETEXTURE)
                               ? FormatConverter::ToDxgiVolume(format)
                               : FormatConverter::ToDxgi(format);
        if (!fm.IsValid()) {
            DXLOG_WARN("[dx12] CreateTexture rejected: D3DFORMAT %d (0x%08X) has no "
                       "DXGI mapping (%ux%u usage=0x%X)",
                       (int)format, (unsigned)format, width, height, (unsigned)usage);
            return D3DERR_NOTAVAILABLE;
        }
        m_expandedLuminance = (type != D3DRTYPE_VOLUMETEXTURE) &&
                              FormatConverter::IsCpuExpandedLuminance(format);

        if (FormatConverter::HasSRGBVariant(fm.dxgiFormat)) {
            m_resFormat     = FormatConverter::ToTypeless(fm.dxgiFormat);
            m_srvFormat     = FormatConverter::ToLinearView(m_resFormat);
            m_srvSrgbFormat = FormatConverter::ToSRGBView(m_resFormat);
            m_rtvFormat     = m_srvFormat;
        } else {
            m_resFormat     = fm.dxgiFormat;
            m_srvFormat     = fm.dxgiFormat;
            m_srvSrgbFormat = fm.dxgiFormat;
            m_rtvFormat     = fm.dxgiFormat;
        }
    }

    m_placement = ClassifyPlacement(pool, usage,   false);

    const UINT stageCount = m_faces * m_levels;
    m_stages.resize(stageCount);
    m_dirty.assign(stageCount, false);
    m_rtv.assign(stageCount, D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) });
    m_rtvSrgb.assign(stageCount, D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) });
    m_dsv.assign(stageCount, D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) });
    m_dsvReadOnly.assign(stageCount, D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) });

    if (m_placement == Placement12::CpuOnly) {
        for (UINT f = 0; f < m_faces; ++f) {
            for (UINT l = 0; l < m_levels; ++l) {
                auto& st = m_stages[f * m_levels + l];
                st.rowPitch   = LevelRowPitch(l);
                UINT lw, lh, ld;
                LevelSize(l, &lw, &lh, &ld);
                st.slicePitch = st.rowPitch * FormatConverter::RowCount(m_format, lh);
                st.shadow.assign(size_t(st.slicePitch) * ld, 0);
            }
        }
        return D3D_OK;
    }

    const D3D12_RESOURCE_FLAGS flags = FlagsForUsage(usage, m_isDepth, depthReadable);
    D3D12_RESOURCE_DESC rd = (type == D3DRTYPE_VOLUMETEXTURE)
        ? dx12::Tex3DDesc(m_resFormat, width, height,
                          static_cast<UINT16>(m_depth), static_cast<UINT16>(m_levels), flags)
        : dx12::Tex2DDesc(m_resFormat, width, height,
                          static_cast<UINT16>(m_levels), static_cast<UINT16>(m_faces), flags);

    D3D12_CLEAR_VALUE clear{};
    const D3D12_CLEAR_VALUE* pClear = nullptr;
    if (m_isDepth || (usage & D3DUSAGE_DEPTHSTENCIL)) {
        clear.Format               = m_dsvFormat;
        clear.DepthStencil.Depth   = 1.0f;
        clear.DepthStencil.Stencil = 0;
        pClear = &clear;
    }

    wchar_t name[64];
    _snwprintf_s(name, _TRUNCATE, L"D3D9Tex_%ux%u_fmt%d", width, height, (int)format);

    HRESULT hr = m_gpu.Init(ctx, rd, D3D12_HEAP_TYPE_DEFAULT,
                            InitialState(usage, m_isDepth), pClear, name);
    if (FAILED(hr)) {
        // Whatever D3D12 objected to, the caller is a 2005 D3D9 game that has
        // never seen E_INVALIDARG from CreateTexture and has no branch for it.
        // D3DERR_OUTOFVIDEOMEMORY is the failure it does handle -- it drops
        // detail and retries -- so a texture we cannot create costs quality
        // rather than a crash. The real HRESULT is already in the log.
        DXLOG_WARN("[dx12] texture %ux%u d3d9fmt=%d levels=%u usage=0x%X pool=%d "
                   "could not be created; reporting D3DERR_OUTOFVIDEOMEMORY so "
                   "the game takes its own fallback",
                   width, height, (int)format, m_levels, (unsigned)usage, (int)pool);
        return D3DERR_OUTOFVIDEOMEMORY;
    }

    for (UINT f = 0; f < m_faces; ++f) {
        for (UINT l = 0; l < m_levels; ++l) {
            auto& st = m_stages[f * m_levels + l];
            st.rowPitch = LevelRowPitch(l);
            UINT lw, lh, ld;
            LevelSize(l, &lw, &lh, &ld);
            st.slicePitch = st.rowPitch * FormatConverter::RowCount(m_format, lh);
        }
    }

    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
        DXLOG_INFO("[dx12] %s created: %ux%u levels=%u d3d9fmt=%d dxgi=%d flags=0x%X pool=%d",
                   (usage & D3DUSAGE_RENDERTARGET) ? "RENDERTARGET" : "DEPTHSTENCIL",
                   width, height, m_levels, (int)format, (int)m_resFormat,
                   (unsigned)flags, (int)pool);
    }
    return D3D_OK;
}

void TextureStorage12::LevelSize(UINT level, UINT* w, UINT* h, UINT* d) const noexcept
{
    const UINT lw = std::max(1u, m_width  >> level);
    const UINT lh = std::max(1u, m_height >> level);
    const UINT ld = (m_type == D3DRTYPE_VOLUMETEXTURE)
                  ? std::max(1u, m_depth >> level) : 1u;
    if (w) *w = lw;
    if (h) *h = lh;
    if (d) *d = ld;
}

UINT TextureStorage12::Subresource(UINT face, UINT level) const noexcept
{
    return dx12::CalcSubresource(level, face, 0, m_levels, m_faces);
}

UINT TextureStorage12::LevelRowPitch(UINT level) const noexcept
{
    UINT lw, lh, ld;
    LevelSize(level, &lw, &lh, &ld);
    return FormatConverter::RowPitch(m_format, lw);
}

UINT TextureStorage12::LevelSlicePitch(UINT level) const noexcept
{
    UINT lw, lh, ld;
    LevelSize(level, &lw, &lh, &ld);
    return LevelRowPitch(level) * FormatConverter::RowCount(m_format, lh);
}

SubresourceStage12& TextureStorage12::Stage(UINT face, UINT level) noexcept
{
    const UINT idx = std::min<UINT>(face * m_levels + level,
                                    static_cast<UINT>(m_stages.size()) - 1);
    return m_stages[idx];
}

HRESULT TextureStorage12::EnsureShadow(UINT face, UINT level,
                                       bool needExistingContents) noexcept
{
    auto& st = Stage(face, level);
    UINT lw, lh, ld;
    LevelSize(level, &lw, &lh, &ld);

    st.rowPitch   = LevelRowPitch(level);
    st.slicePitch = st.rowPitch * FormatConverter::RowCount(m_format, lh);
    const size_t bytes = size_t(st.slicePitch) * ld;

    if (st.shadow.size() < bytes)
        st.shadow.assign(bytes, 0);

    if (!needExistingContents || !m_gpu.Valid())
        return D3D_OK;

    if (m_expandedLuminance) {
        const UINT expBpp = FormatConverter::ExpandedBytesPerPixel(m_format);
        std::vector<uint8_t> tmp(size_t(expBpp) * lw * lh);
        HRESULT hr = m_gpu.ReadbackSubresource(Subresource(face, level), tmp.data(),
                                               expBpp * lw, expBpp * lw * lh);
        if (FAILED(hr)) return hr;
        FormatConverter::ContractLumRect(m_format, tmp.data(), expBpp * lw,
                                         st.shadow.data(), st.rowPitch, lw, lh);
        return D3D_OK;
    }
    return m_gpu.ReadbackSubresource(Subresource(face, level), st.shadow.data(),
                                     st.rowPitch, st.slicePitch);
}

HRESULT TextureStorage12::LockRegion(UINT face, UINT level,
                                     const RECT* pRect, const D3DBOX* pBox,
                                     DWORD flags, void** ppBits,
                                     UINT* pRowPitch, UINT* pSlicePitch) noexcept
{
    if (!ppBits) return D3DERR_INVALIDCALL;
    if (level >= m_levels || face >= m_faces) return D3DERR_INVALIDCALL;

    auto& st = Stage(face, level);
    if (st.locked) {
        DXLOG_WARN("[dx12] nested Lock on face %u level %u rejected", face, level);
        return D3DERR_INVALIDCALL;
    }

    if (m_placement == Placement12::GpuOnly &&
        !(m_usage & (D3DUSAGE_DYNAMIC | D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {
        return D3DERR_INVALIDCALL;
    }

    UINT lw, lh, ld;
    LevelSize(level, &lw, &lh, &ld);

    const bool discard  = (flags & D3DLOCK_DISCARD) != 0;
    const bool readOnly = (flags & D3DLOCK_READONLY) != 0;
    const bool needRead = !discard && st.everWritten;

    HRESULT hr = EnsureShadow(face, level, needRead);
    if (FAILED(hr)) return hr;

    if (pBox) {
        st.lockedBox     = *pBox;
        st.lockedRect    = RECT{ LONG(pBox->Left), LONG(pBox->Top),
                                 LONG(pBox->Right), LONG(pBox->Bottom) };
        st.wholeResource = (pBox->Left == 0 && pBox->Top == 0 && pBox->Front == 0 &&
                            pBox->Right == lw && pBox->Bottom == lh && pBox->Back == ld);
    } else if (pRect) {
        st.lockedRect    = *pRect;
        st.lockedBox     = D3DBOX{ UINT(pRect->left), UINT(pRect->top),
                                   UINT(pRect->right), UINT(pRect->bottom), 0, ld };
        st.wholeResource = (pRect->left == 0 && pRect->top == 0 &&
                            pRect->right == LONG(lw) && pRect->bottom == LONG(lh));
    } else {
        st.lockedRect    = RECT{ 0, 0, LONG(lw), LONG(lh) };
        st.lockedBox     = D3DBOX{ 0, 0, lw, lh, 0, ld };
        st.wholeResource = true;
    }

    st.lockFlags = flags;
    st.locked    = true;

    const UINT bytesPerBlock = FormatConverter::BlockOrPixelSize(m_format);
    const bool bc            = FormatConverter::IsBlockCompressed(m_format);
    const LONG xBlocks       = bc ? (st.lockedRect.left / 4) : st.lockedRect.left;
    const LONG yBlocks       = bc ? (st.lockedRect.top  / 4) : st.lockedRect.top;

    size_t byteOffset = size_t(yBlocks) * st.rowPitch + size_t(xBlocks) * bytesPerBlock;
    if (pBox) byteOffset += size_t(pBox->Front) * st.slicePitch;
    if (byteOffset >= st.shadow.size()) byteOffset = 0;

    *ppBits = st.shadow.data() + byteOffset;
    if (pRowPitch)   *pRowPitch   = st.rowPitch;
    if (pSlicePitch) *pSlicePitch = st.slicePitch;

    if (!readOnly)
        m_dirty[face * m_levels + level] = true;
    return D3D_OK;
}

HRESULT TextureStorage12::UnlockRegion(UINT face, UINT level) noexcept
{
    if (level >= m_levels || face >= m_faces) return D3DERR_INVALIDCALL;
    auto& st = Stage(face, level);
    if (!st.locked) return D3DERR_INVALIDCALL;

    st.locked = false;
    const bool readOnly = (st.lockFlags & D3DLOCK_READONLY) != 0;
    if (readOnly) {

        if (m_placement != Placement12::CpuOnly)
            st.shadow.clear();
        return D3D_OK;
    }

    st.everWritten = true;

    HRESULT hr = D3D_OK;
    if (m_gpu.Valid())
        hr = UploadStage(face, level, st.wholeResource ? nullptr : &st.lockedRect);

    m_dirty[face * m_levels + level] = false;
    if (m_placement != Placement12::CpuOnly)
        st.shadow.clear();
    return hr;
}

HRESULT TextureStorage12::UploadStage(UINT face, UINT level, const RECT* rect) noexcept
{
    auto& st = Stage(face, level);
    if (st.shadow.empty()) return D3D_OK;

    UINT lw, lh, ld;
    LevelSize(level, &lw, &lh, &ld);
    const UINT sub = Subresource(face, level);

    if (m_expandedLuminance) {
        const UINT expBpp   = FormatConverter::ExpandedBytesPerPixel(m_format);
        const UINT expPitch = expBpp * lw;
        std::vector<uint8_t> tmp(size_t(expPitch) * lh);
        FormatConverter::ExpandLumRect(m_format, st.shadow.data(), st.rowPitch,
                                       tmp.data(), expPitch, lw, lh);
        return m_gpu.UploadSubresource(sub, tmp.data(), expPitch, expPitch * lh);
    }

    std::vector<uint8_t> swapped;
    const uint8_t* srcBits = st.shadow.data();
    if (FormatConverter::NeedsRB10Swap(m_format)) {
        swapped.assign(st.shadow.begin(), st.shadow.end());
        FormatConverter::SwapRB10Rect(swapped.data(), st.rowPitch, lw, lh);
        srcBits = swapped.data();
    }

    if (!rect)
        return m_gpu.UploadSubresource(sub, srcBits, st.rowPitch, st.slicePitch);

    const bool bc = FormatConverter::IsBlockCompressed(m_format);
    LONG left   = rect->left, top = rect->top;
    LONG right  = rect->right, bottom = rect->bottom;
    if (bc) {
        left   &= ~3; top    &= ~3;
        right   = (right  + 3) & ~3;
        bottom  = (bottom + 3) & ~3;
    }
    left   = std::clamp<LONG>(left,   0, LONG(lw));
    top    = std::clamp<LONG>(top,    0, LONG(lh));
    right  = std::clamp<LONG>(right,  left, LONG(lw));
    bottom = std::clamp<LONG>(bottom, top,  LONG(lh));
    if (right <= left || bottom <= top) return D3D_OK;

    const UINT bytesPerBlock = FormatConverter::BlockOrPixelSize(m_format);
    const size_t srcOffset =
        size_t(bc ? top / 4 : top) * st.rowPitch +
        size_t(bc ? left / 4 : left) * bytesPerBlock;

    return m_gpu.UploadSubresourceRegion(
        sub, UINT(left), UINT(top), 0,
        UINT(right - left), UINT(bottom - top), 1,
        srcBits + srcOffset, st.rowPitch, st.slicePitch);
}

void TextureStorage12::AddDirty(UINT face, UINT level, const RECT*) noexcept
{
    if (face < m_faces && level < m_levels)
        m_dirty[face * m_levels + level] = true;
}

HRESULT TextureStorage12::FlushDirty() noexcept
{
    if (!m_gpu.Valid()) return D3D_OK;
    HRESULT hr = D3D_OK;
    for (UINT f = 0; f < m_faces; ++f) {
        for (UINT l = 0; l < m_levels; ++l) {
            const UINT idx = f * m_levels + l;
            if (!m_dirty[idx]) continue;
            auto& st = m_stages[idx];
            if (st.locked || st.shadow.empty()) continue;
            const HRESULT one = UploadStage(f, l, nullptr);
            if (FAILED(one)) hr = one;
            m_dirty[idx] = false;
        }
    }
    return hr;
}

D3D12_CPU_DESCRIPTOR_HANDLE TextureStorage12::Srv(bool srgb) noexcept
{
    if (!m_ctx || !m_gpu.Valid())
        return D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };

    const bool wantSrgb = srgb && (m_srvSrgbFormat != m_srvFormat) &&
                          m_srvSrgbFormat != DXGI_FORMAT_UNKNOWN;
    auto& slot = wantSrgb ? m_srvSrgb : m_srv;
    if (slot.ptr != SIZE_T(-1))
        return slot;

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format                  = wantSrgb ? m_srvSrgbFormat : m_srvFormat;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    switch (m_type) {
    case D3DRTYPE_CUBETEXTURE:
        sd.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MostDetailedMip   = 0;
        sd.TextureCube.MipLevels         = m_levels;
        sd.TextureCube.ResourceMinLODClamp = 0.0f;
        break;
    case D3DRTYPE_VOLUMETEXTURE:
        sd.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D.MostDetailedMip     = 0;
        sd.Texture3D.MipLevels           = m_levels;
        sd.Texture3D.ResourceMinLODClamp = 0.0f;
        break;
    default:
        sd.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip     = 0;
        sd.Texture2D.MipLevels           = m_levels;
        sd.Texture2D.PlaneSlice          = 0;
        sd.Texture2D.ResourceMinLODClamp = 0.0f;
        break;
    }

    const auto h = m_ctx->SrvStaging().Alloc();
    if (h.ptr == SIZE_T(-1)) {
        DXLOG_ERROR("[dx12] SRV staging heap exhausted");
        return h;
    }
    m_ctx->Device()->CreateShaderResourceView(m_gpu.Native(), &sd, h);
    slot = h;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE TextureStorage12::Rtv(UINT face, UINT level, bool srgb) noexcept
{
    if (!m_ctx || !m_gpu.Valid() || face >= m_faces || level >= m_levels)
        return D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };
    if (!(m_usage & D3DUSAGE_RENDERTARGET))
        return D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };

    const DXGI_FORMAT srgbFmt = FormatConverter::ToSRGBView(m_resFormat);
    const bool wantSrgb = srgb && srgbFmt != DXGI_FORMAT_UNKNOWN && srgbFmt != m_rtvFormat;
    auto& table = wantSrgb ? m_rtvSrgb : m_rtv;
    const UINT idx = face * m_levels + level;
    if (table[idx].ptr != SIZE_T(-1))
        return table[idx];

    D3D12_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = wantSrgb ? srgbFmt : m_rtvFormat;
    if (m_type == D3DRTYPE_VOLUMETEXTURE) {
        rd.ViewDimension        = D3D12_RTV_DIMENSION_TEXTURE3D;
        rd.Texture3D.MipSlice   = level;
        rd.Texture3D.FirstWSlice = 0;
        rd.Texture3D.WSize      = static_cast<UINT>(-1);
    } else if (m_faces > 1) {
        rd.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.MipSlice        = level;
        rd.Texture2DArray.FirstArraySlice = face;
        rd.Texture2DArray.ArraySize       = 1;
        rd.Texture2DArray.PlaneSlice      = 0;
    } else {
        rd.ViewDimension        = D3D12_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice   = level;
        rd.Texture2D.PlaneSlice = 0;
    }

    const auto h = m_ctx->RtvHeap().Alloc();
    if (h.ptr == SIZE_T(-1)) {
        DXLOG_ERROR("[dx12] RTV heap exhausted");
        return h;
    }
    m_ctx->Device()->CreateRenderTargetView(m_gpu.Native(), &rd, h);
    table[idx] = h;
    return h;
}

// A read-only DSV is the only legal way to have a depth buffer bound for
// testing while the same texture is also bound as a shader resource. D3D12
// rejects the combination otherwise, and D3D11 silently unbinds the SRV --
// which is how "the depth texture reads as nothing" happens with no error
// anywhere. With READ_ONLY_DEPTH|READ_ONLY_STENCIL the resource can sit in the
// combined DEPTH_READ | *_SHADER_RESOURCE state and serve both.
D3D12_CPU_DESCRIPTOR_HANDLE TextureStorage12::Dsv(UINT face, UINT level,
                                                  bool readOnly) noexcept
{
    if (!m_ctx || !m_gpu.Valid() || !m_isDepth || face >= m_faces || level >= m_levels)
        return D3D12_CPU_DESCRIPTOR_HANDLE{ SIZE_T(-1) };

    const UINT idx = face * m_levels + level;
    auto& slot = readOnly ? m_dsvReadOnly[idx] : m_dsv[idx];
    if (slot.ptr != SIZE_T(-1))
        return slot;

    D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = m_dsvFormat;
    dd.Flags  = D3D12_DSV_FLAG_NONE;
    if (readOnly) {
        dd.Flags |= D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        if (FormatConverter::DsvFormatHasStencil(m_dsvFormat))
            dd.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    }
    if (m_faces > 1) {
        dd.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dd.Texture2DArray.MipSlice        = level;
        dd.Texture2DArray.FirstArraySlice = face;
        dd.Texture2DArray.ArraySize       = 1;
    } else {
        dd.ViewDimension      = D3D12_DSV_DIMENSION_TEXTURE2D;
        dd.Texture2D.MipSlice = level;
    }

    const auto h = m_ctx->DsvHeap().Alloc();
    if (h.ptr == SIZE_T(-1)) {
        DXLOG_ERROR("[dx12] DSV heap exhausted");
        return h;
    }
    m_ctx->Device()->CreateDepthStencilView(m_gpu.Native(), &dd, h);
    slot = h;
    return h;
}

}
