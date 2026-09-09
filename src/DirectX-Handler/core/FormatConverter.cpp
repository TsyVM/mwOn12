// The D3DFORMAT to DXGI_FORMAT mapping table.
//
// Three kinds of mismatch have to be handled. Some formats differ only in
// channel order, because D3D9 names colours in ARGB byte order and DXGI in
// RGBA, and need a swizzle when sampled or written. Some have no hardware
// equivalent at all and are expanded on the CPU during upload -- the
// palettised and low-precision formats in particular. A few cannot be
// represented and are rejected.
//
// A rejection must be logged. Mapping an unknown format to something
// approximate produces wrong pixels with no failure anywhere, which is the
// most expensive kind of fault to track down; refusing it and saying so gives
// something to search for.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <core/FormatConverter.h>

namespace mwon12 {

namespace {

struct TableEntry {
    D3DFORMAT   d3d9;
    DXGI_FORMAT dxgi;
    FormatNote  notes;
};

constexpr TableEntry kTable[] = {

    { D3DFMT_A8R8G8B8,      DXGI_FORMAT_B8G8R8A8_UNORM,        FormatNote::None },
    { D3DFMT_X8R8G8B8,      DXGI_FORMAT_B8G8R8X8_UNORM,        FormatNote::None },
    { D3DFMT_A8B8G8R8,      DXGI_FORMAT_R8G8B8A8_UNORM,        FormatNote::None },
    { D3DFMT_X8B8G8R8,      DXGI_FORMAT_R8G8B8A8_UNORM,        FormatNote::Emulated },
    { D3DFMT_A2B10G10R10,   DXGI_FORMAT_R10G10B10A2_UNORM,     FormatNote::None },
    { D3DFMT_A2R10G10B10,   DXGI_FORMAT_R10G10B10A2_UNORM,     FormatNote::Emulated | FormatNote::RequiresSwizzle },
    { D3DFMT_A2B10G10R10_XR_BIAS, DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, FormatNote::Emulated },
    { D3DFMT_G16R16,        DXGI_FORMAT_R16G16_UNORM,          FormatNote::None },

    { D3DFMT_R8G8B8,        DXGI_FORMAT_UNKNOWN,               FormatNote::Rejected },

    { D3DFMT_R5G6B5,        DXGI_FORMAT_B5G6R5_UNORM,          FormatNote::None },
    { D3DFMT_A1R5G5B5,      DXGI_FORMAT_B5G5R5A1_UNORM,        FormatNote::None },
    { D3DFMT_X1R5G5B5,      DXGI_FORMAT_B5G5R5A1_UNORM,        FormatNote::Emulated },
    { D3DFMT_A4R4G4B4,      DXGI_FORMAT_B4G4R4A4_UNORM,        FormatNote::None },
    { D3DFMT_X4R4G4B4,      DXGI_FORMAT_B4G4R4A4_UNORM,        FormatNote::Emulated },

    { D3DFMT_A8L8,          DXGI_FORMAT_R8G8B8A8_UNORM,        FormatNote::Emulated | FormatNote::CpuExpanded },

    { D3DFMT_A8,            DXGI_FORMAT_A8_UNORM,              FormatNote::None },
    { D3DFMT_L8,            DXGI_FORMAT_R8G8B8A8_UNORM,        FormatNote::Emulated | FormatNote::CpuExpanded },
    { D3DFMT_L16,           DXGI_FORMAT_R16G16B16A16_UNORM,    FormatNote::Emulated | FormatNote::CpuExpanded },
    { D3DFMT_A8P8,          DXGI_FORMAT_UNKNOWN,                FormatNote::Rejected },
    { D3DFMT_P8,            DXGI_FORMAT_UNKNOWN,                FormatNote::Rejected },
    { D3DFMT_R3G3B2,        DXGI_FORMAT_UNKNOWN,                FormatNote::Rejected },
    { D3DFMT_A8R3G3B2,      DXGI_FORMAT_UNKNOWN,                FormatNote::Rejected },

    { D3DFMT_V8U8,          DXGI_FORMAT_R8G8_SNORM,            FormatNote::None },
    { D3DFMT_Q8W8V8U8,      DXGI_FORMAT_R8G8B8A8_SNORM,        FormatNote::None },
    { D3DFMT_V16U16,        DXGI_FORMAT_R16G16_SNORM,          FormatNote::None },
    { D3DFMT_CxV8U8,        DXGI_FORMAT_R8G8_SNORM,            FormatNote::Emulated },
    { D3DFMT_A2W10V10U10,   DXGI_FORMAT_UNKNOWN,                FormatNote::Rejected },

    { D3DFMT_R16F,          DXGI_FORMAT_R16_FLOAT,             FormatNote::None },
    { D3DFMT_G16R16F,       DXGI_FORMAT_R16G16_FLOAT,          FormatNote::None },
    { D3DFMT_A16B16G16R16F, DXGI_FORMAT_R16G16B16A16_FLOAT,    FormatNote::None },
    { D3DFMT_R32F,          DXGI_FORMAT_R32_FLOAT,             FormatNote::None },
    { D3DFMT_G32R32F,       DXGI_FORMAT_R32G32_FLOAT,          FormatNote::None },
    { D3DFMT_A32B32G32R32F, DXGI_FORMAT_R32G32B32A32_FLOAT,    FormatNote::None },

    { D3DFMT_A16B16G16R16,  DXGI_FORMAT_R16G16B16A16_UNORM,    FormatNote::None },

    { D3DFMT_DXT1,          DXGI_FORMAT_BC1_UNORM,             FormatNote::None },
    { D3DFMT_DXT2,          DXGI_FORMAT_BC2_UNORM,             FormatNote::Emulated },
    { D3DFMT_DXT3,          DXGI_FORMAT_BC2_UNORM,             FormatNote::None },
    { D3DFMT_DXT4,          DXGI_FORMAT_BC3_UNORM,             FormatNote::Emulated },
    { D3DFMT_DXT5,          DXGI_FORMAT_BC3_UNORM,             FormatNote::None },

    { D3DFMT_ATI1,          DXGI_FORMAT_BC4_UNORM,             FormatNote::None },
    { D3DFMT_ATI2,          DXGI_FORMAT_BC5_UNORM,             FormatNote::None },

    { D3DFMT_D16_LOCKABLE,  DXGI_FORMAT_D16_UNORM,             FormatNote::None },
    { D3DFMT_D16,           DXGI_FORMAT_D16_UNORM,             FormatNote::None },
    { D3DFMT_D15S1,         DXGI_FORMAT_D24_UNORM_S8_UINT,     FormatNote::Emulated },
    { D3DFMT_D24X8,         DXGI_FORMAT_D24_UNORM_S8_UINT,     FormatNote::Emulated },
    { D3DFMT_D24S8,         DXGI_FORMAT_D24_UNORM_S8_UINT,     FormatNote::None },
    { D3DFMT_D24X4S4,       DXGI_FORMAT_D24_UNORM_S8_UINT,     FormatNote::Emulated },
    { D3DFMT_D24FS8,        DXGI_FORMAT_D24_UNORM_S8_UINT,     FormatNote::Emulated },
    { D3DFMT_D32,           DXGI_FORMAT_D32_FLOAT,             FormatNote::Emulated },
    { D3DFMT_D32F_LOCKABLE, DXGI_FORMAT_D32_FLOAT,             FormatNote::None },
    { D3DFMT_D32_LOCKABLE,  DXGI_FORMAT_D32_FLOAT,             FormatNote::Emulated },
    { D3DFMT_S8_LOCKABLE,   DXGI_FORMAT_UNKNOWN,                FormatNote::Rejected },

    { D3DFMT_INDEX16,       DXGI_FORMAT_R16_UINT,              FormatNote::None },
    { D3DFMT_INDEX32,       DXGI_FORMAT_R32_UINT,              FormatNote::None },
};

constexpr size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);

}

FormatMapping FormatConverter::ToDxgi(D3DFORMAT format) noexcept
{
    for (const TableEntry& e : kTable) {
        if (e.d3d9 == format) {
            return FormatMapping{ e.dxgi, e.notes };
        }
    }

    DepthFormatViews dv{};
    if (IsFourCC(format) && GetDepthFormatViews(format, dv)) {
        return FormatMapping{ dv.srv, FormatNote::Emulated };
    }

    return FormatMapping{ DXGI_FORMAT_UNKNOWN, FormatNote::Rejected };
}

D3DFORMAT FormatConverter::ToD3D9(DXGI_FORMAT format) noexcept
{

    for (const TableEntry& e : kTable) {
        if (e.dxgi == format && !HasNote(e.notes, FormatNote::Emulated)) {
            return e.d3d9;
        }
    }
    for (const TableEntry& e : kTable) {
        if (e.dxgi == format) {
            return e.d3d9;
        }
    }
    return D3DFMT_UNKNOWN;
}

bool FormatConverter::IsBlockCompressed(D3DFORMAT format) noexcept
{
    switch (format) {
        case D3DFMT_DXT1:
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:
        case D3DFMT_DXT4:
        case D3DFMT_DXT5:
            return true;
        default:

            return format == D3DFMT_ATI1 || format == D3DFMT_ATI2;
    }
}

bool FormatConverter::IsDepthFormat(D3DFORMAT format) noexcept
{
    switch (format) {
        case D3DFMT_D16_LOCKABLE:
        case D3DFMT_D16:
        case D3DFMT_D15S1:
        case D3DFMT_D24X8:
        case D3DFMT_D24S8:
        case D3DFMT_D24X4S4:
        case D3DFMT_D24FS8:
        case D3DFMT_D32:
        case D3DFMT_D32F_LOCKABLE:
        case D3DFMT_D32_LOCKABLE:
        case D3DFMT_S8_LOCKABLE:
            return true;
        default:

            return format == D3DFMT_INTZ || format == D3DFMT_RAWZ ||
                   format == D3DFMT_DF24 || format == D3DFMT_DF16;
    }
}

bool FormatConverter::IsFourCC(D3DFORMAT format) noexcept
{
    return format == D3DFMT_INTZ || format == D3DFMT_RAWZ ||
           format == D3DFMT_DF24 || format == D3DFMT_DF16 ||
           format == D3DFMT_NULL ||
           format == D3DFMT_ATI1 || format == D3DFMT_ATI2;
}

bool FormatConverter::IsNullSurfaceFormat(D3DFORMAT format) noexcept
{
    return format == D3DFMT_NULL;
}

bool FormatConverter::DsvFormatHasStencil(DXGI_FORMAT dsvFormat) noexcept
{
    switch (dsvFormat) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return true;
    default:
        return false;
    }
}

bool FormatConverter::GetDepthFormatViews(D3DFORMAT format, DepthFormatViews& out) noexcept
{
    switch (format) {

        case D3DFMT_D16:
        case D3DFMT_D16_LOCKABLE:
            out = { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM,
                    DXGI_FORMAT_R16_UNORM, true };
            return true;

        case D3DFMT_D32:
        case D3DFMT_D32_LOCKABLE:
        case D3DFMT_D32F_LOCKABLE:
            out = { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT,
                    DXGI_FORMAT_R32_FLOAT, true };
            return true;

        case D3DFMT_D24S8:
        case D3DFMT_D24X8:
        case D3DFMT_D24X4S4:
        case D3DFMT_D15S1:
        case D3DFMT_D24FS8:
            out = { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT,
                    DXGI_FORMAT_R24_UNORM_X8_TYPELESS, true };
            return true;

        default:
            break;
    }

    if (format == D3DFMT_DF16) {
        out = { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM,
                DXGI_FORMAT_R16_UNORM, true };
        return true;
    }
    if (format == D3DFMT_INTZ || format == D3DFMT_DF24 || format == D3DFMT_RAWZ) {

        out = { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT,
                DXGI_FORMAT_R24_UNORM_X8_TYPELESS, true };
        return true;
    }
    return false;
}

UINT FormatConverter::BlockOrPixelSize(D3DFORMAT format) noexcept
{
    if (IsBlockCompressed(format)) {

        return (format == D3DFMT_DXT1 || format == D3DFMT_ATI1) ? 8u : 16u;
    }

    if (format == D3DFMT_INTZ || format == D3DFMT_DF24 || format == D3DFMT_RAWZ)
        return 4u;
    if (format == D3DFMT_DF16)
        return 2u;

    switch (format) {
        case D3DFMT_A8:
        case D3DFMT_L8:
        case D3DFMT_P8:
        case D3DFMT_R3G3B2:
            return 1u;

        case D3DFMT_R5G6B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        case D3DFMT_A8L8:
        case D3DFMT_A8P8:
        case D3DFMT_A8R3G3B2:
        case D3DFMT_V8U8:
        case D3DFMT_R16F:
        case D3DFMT_D16_LOCKABLE:
        case D3DFMT_D16:
        case D3DFMT_D15S1:
        case D3DFMT_L16:
        case D3DFMT_INDEX16:
            return 2u;

        case D3DFMT_R8G8B8:
            return 3u;

        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
        case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8:
        case D3DFMT_A2B10G10R10:
        case D3DFMT_A2R10G10B10:
        case D3DFMT_A2B10G10R10_XR_BIAS:
        case D3DFMT_G16R16:
        case D3DFMT_Q8W8V8U8:
        case D3DFMT_V16U16:
        case D3DFMT_CxV8U8:
        case D3DFMT_G16R16F:
        case D3DFMT_R32F:
        case D3DFMT_D24X8:
        case D3DFMT_D24S8:
        case D3DFMT_D24X4S4:
        case D3DFMT_D24FS8:
        case D3DFMT_D32:
        case D3DFMT_D32F_LOCKABLE:
        case D3DFMT_D32_LOCKABLE:
        case D3DFMT_INDEX32:
            return 4u;

        case D3DFMT_A16B16G16R16:
        case D3DFMT_A16B16G16R16F:
        case D3DFMT_G32R32F:
            return 8u;

        case D3DFMT_A32B32G32R32F:
            return 16u;

        default:

            return 0u;
    }
}

UINT FormatConverter::RowPitch(D3DFORMAT format, UINT width) noexcept
{
    if (IsBlockCompressed(format)) {
        const UINT blocksWide = (width + 3u) / 4u;
        return blocksWide * BlockOrPixelSize(format);
    }
    return width * BlockOrPixelSize(format);
}

UINT FormatConverter::RowCount(D3DFORMAT format, UINT height) noexcept
{
    if (IsBlockCompressed(format)) {
        return (height + 3u) / 4u;
    }
    return height;
}

bool FormatConverter::NeedsRB10Swap(D3DFORMAT format) noexcept
{
    return format == D3DFMT_A2R10G10B10;
}

void FormatConverter::SwapRB10Rect(void* bits, UINT rowPitchBytes,
                                   UINT widthPx, UINT heightPx) noexcept
{

    auto* row = static_cast<uint8_t*>(bits);
    for (UINT y = 0; y < heightPx; ++y, row += rowPitchBytes) {
        auto* px = reinterpret_cast<uint32_t*>(row);
        for (UINT x = 0; x < widthPx; ++x) {
            const uint32_t v = px[x];
            px[x] = (v & 0xC00FFC00u)
                  | ((v >> 20) & 0x3FFu)
                  | ((v & 0x3FFu) << 20);
        }
    }
}

DXGI_FORMAT FormatConverter::BackBufferFormat(D3DFORMAT requested) noexcept
{
    switch (requested) {

        case D3DFMT_A2R10G10B10:
        case D3DFMT_A2B10G10R10:
        case D3DFMT_A2B10G10R10_XR_BIAS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;

        default:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

bool FormatConverter::HasSRGBVariant(DXGI_FORMAT format) noexcept
{
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC7_UNORM:
            return true;
        default:
            return false;
    }
}

DXGI_FORMAT FormatConverter::ToTypeless(DXGI_FORMAT format) noexcept
{
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8X8_TYPELESS;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            return DXGI_FORMAT_BC1_TYPELESS;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
            return DXGI_FORMAT_BC2_TYPELESS;
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            return DXGI_FORMAT_BC3_TYPELESS;
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return DXGI_FORMAT_BC7_TYPELESS;
        default:

            return format;
    }
}

DXGI_FORMAT FormatConverter::ToLinearView(DXGI_FORMAT format) noexcept
{
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_BC1_TYPELESS:      return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS:      return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS:      return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC7_TYPELESS:      return DXGI_FORMAT_BC7_UNORM;
        default:                            return format;
    }
}

bool FormatConverter::IsCpuExpandedLuminance(D3DFORMAT format) noexcept
{
    return format == D3DFMT_L8 || format == D3DFMT_A8L8 || format == D3DFMT_L16;
}

FormatMapping FormatConverter::ToDxgiVolume(D3DFORMAT format) noexcept
{

    switch (format) {
        case D3DFMT_L8:
            return FormatMapping{ DXGI_FORMAT_R8_UNORM,
                                  FormatNote::Emulated | FormatNote::RequiresSwizzle };
        case D3DFMT_A8L8:
            return FormatMapping{ DXGI_FORMAT_R8G8_UNORM,
                                  FormatNote::Emulated | FormatNote::RequiresSwizzle };
        case D3DFMT_L16:
            return FormatMapping{ DXGI_FORMAT_R16_UNORM,
                                  FormatNote::Emulated | FormatNote::RequiresSwizzle };
        default:
            return ToDxgi(format);
    }
}

UINT FormatConverter::ExpandedBytesPerPixel(D3DFORMAT format) noexcept
{
    switch (format) {
        case D3DFMT_L8:
        case D3DFMT_A8L8: return 4u;
        case D3DFMT_L16:  return 8u;
        default:          return 0u;
    }
}

void FormatConverter::ExpandLumRect(D3DFORMAT format,
                                    const void* src, UINT srcPitch,
                                    void* dst, UINT dstPitch,
                                    UINT widthPx, UINT heightPx) noexcept
{
    const auto* srcRow = static_cast<const uint8_t*>(src);
    auto*       dstRow = static_cast<uint8_t*>(dst);

    switch (format) {
        case D3DFMT_L8:
            for (UINT y = 0; y < heightPx; ++y, srcRow += srcPitch, dstRow += dstPitch) {
                auto* d = reinterpret_cast<uint32_t*>(dstRow);
                for (UINT x = 0; x < widthPx; ++x) {
                    const uint32_t l = srcRow[x];
                    d[x] = 0xFF000000u | (l * 0x00010101u);
                }
            }
            break;

        case D3DFMT_A8L8:

            for (UINT y = 0; y < heightPx; ++y, srcRow += srcPitch, dstRow += dstPitch) {
                auto* d = reinterpret_cast<uint32_t*>(dstRow);
                for (UINT x = 0; x < widthPx; ++x) {
                    const uint32_t l = srcRow[x * 2 + 0];
                    const uint32_t a = srcRow[x * 2 + 1];
                    d[x] = (a << 24) | (l * 0x00010101u);
                }
            }
            break;

        case D3DFMT_L16:
            for (UINT y = 0; y < heightPx; ++y, srcRow += srcPitch, dstRow += dstPitch) {
                const auto* s = reinterpret_cast<const uint16_t*>(srcRow);
                auto*       d = reinterpret_cast<uint16_t*>(dstRow);
                for (UINT x = 0; x < widthPx; ++x) {
                    const uint16_t l = s[x];
                    d[x * 4 + 0] = l;
                    d[x * 4 + 1] = l;
                    d[x * 4 + 2] = l;
                    d[x * 4 + 3] = 0xFFFFu;
                }
            }
            break;

        default:
            break;
    }
}

void FormatConverter::ContractLumRect(D3DFORMAT format,
                                      const void* src, UINT srcPitch,
                                      void* dst, UINT dstPitch,
                                      UINT widthPx, UINT heightPx) noexcept
{
    const auto* srcRow = static_cast<const uint8_t*>(src);
    auto*       dstRow = static_cast<uint8_t*>(dst);

    switch (format) {
        case D3DFMT_L8:
            for (UINT y = 0; y < heightPx; ++y, srcRow += srcPitch, dstRow += dstPitch) {
                const auto* s = reinterpret_cast<const uint32_t*>(srcRow);
                for (UINT x = 0; x < widthPx; ++x)
                    dstRow[x] = static_cast<uint8_t>(s[x] & 0xFFu);
            }
            break;

        case D3DFMT_A8L8:
            for (UINT y = 0; y < heightPx; ++y, srcRow += srcPitch, dstRow += dstPitch) {
                const auto* s = reinterpret_cast<const uint32_t*>(srcRow);
                for (UINT x = 0; x < widthPx; ++x) {
                    dstRow[x * 2 + 0] = static_cast<uint8_t>(s[x] & 0xFFu);
                    dstRow[x * 2 + 1] = static_cast<uint8_t>(s[x] >> 24);
                }
            }
            break;

        case D3DFMT_L16:
            for (UINT y = 0; y < heightPx; ++y, srcRow += srcPitch, dstRow += dstPitch) {
                const auto* s = reinterpret_cast<const uint16_t*>(srcRow);
                auto*       d = reinterpret_cast<uint16_t*>(dstRow);
                for (UINT x = 0; x < widthPx; ++x)
                    d[x] = s[x * 4 + 0];
            }
            break;

        default:
            break;
    }
}

DXGI_FORMAT FormatConverter::ToSRGBView(DXGI_FORMAT format) noexcept
{
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
            return DXGI_FORMAT_BC1_UNORM_SRGB;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
            return DXGI_FORMAT_BC2_UNORM_SRGB;
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
            return DXGI_FORMAT_BC3_UNORM_SRGB;
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
            return DXGI_FORMAT_BC7_UNORM_SRGB;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

}
