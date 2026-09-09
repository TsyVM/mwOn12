#pragma once

#ifndef MWON12_FORMAT_CONVERTER_H
#define MWON12_FORMAT_CONVERTER_H

#include <d3d9.h>
#include <dxgi.h>
#include <cstdint>

// D3DFORMAT -> DXGI_FORMAT, and the compromises that mapping requires.
//
// Not every D3D9 format has a DXGI equivalent. Some need a channel swizzle
// (D3D9's ARGB byte order against DXGI's RGBA), some have to be expanded on
// the CPU at upload time because no hardware format matches, and a few cannot
// be represented at all and are rejected. FormatNote records which of those
// applies so callers can take the right path and the log can explain what
// happened to a texture that did not arrive intact.
//
// Rejecting must be rare and must be loud. A format quietly mapped to
// something approximate produces wrong pixels with no failure anywhere, which
// is the hardest kind of bug this project has.
namespace mwon12 {

// FourCC formats that never had a D3DFORMAT enumerant. Vendors introduced
// these as extensions and games detect them by calling CheckDeviceFormat with
// the code, so they have to be recognised by name here.
//
// INTZ and DF24 are depth formats a game samples as a texture, used for shadow
// maps and depth-based effects. MWOn12 does not synthesise them — the R channel
// is sampled directly rather than reconstructing D3D9's exact encoding, which
// is correct for straightforward depth reads and approximate for anything that
// depends on the packed bit layout.
inline constexpr D3DFORMAT D3DFMT_INTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I','N','T','Z'));
inline constexpr D3DFORMAT D3DFMT_RAWZ = static_cast<D3DFORMAT>(MAKEFOURCC('R','A','W','Z'));
inline constexpr D3DFORMAT D3DFMT_DF24 = static_cast<D3DFORMAT>(MAKEFOURCC('D','F','2','4'));
inline constexpr D3DFORMAT D3DFMT_DF16 = static_cast<D3DFORMAT>(MAKEFOURCC('D','F','1','6'));
inline constexpr D3DFORMAT D3DFMT_NULL = static_cast<D3DFORMAT>(MAKEFOURCC('N','U','L','L'));
inline constexpr D3DFORMAT D3DFMT_ATI1 = static_cast<D3DFORMAT>(MAKEFOURCC('A','T','I','1'));
inline constexpr D3DFORMAT D3DFMT_ATI2 = static_cast<D3DFORMAT>(MAKEFOURCC('A','T','I','2'));

enum class FormatNote : uint32_t {
    None            = 0,
    RequiresSwizzle = 1u << 0,
    Emulated        = 1u << 1,
    Rejected        = 1u << 2,
    CpuExpanded     = 1u << 3,

};

[[nodiscard]] constexpr FormatNote operator|(FormatNote a, FormatNote b) noexcept
{
    return static_cast<FormatNote>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
[[nodiscard]] constexpr bool HasNote(FormatNote mask, FormatNote bit) noexcept
{
    return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(bit)) != 0;
}

struct FormatMapping {
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
    FormatNote  notes      = FormatNote::Rejected;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return dxgiFormat != DXGI_FORMAT_UNKNOWN && !HasNote(notes, FormatNote::Rejected);
    }
};

class FormatConverter {
public:
    FormatConverter()                                   = delete;

    [[nodiscard]] static FormatMapping ToDxgi(D3DFORMAT format) noexcept;

    [[nodiscard]] static D3DFORMAT ToD3D9(DXGI_FORMAT format) noexcept;

    [[nodiscard]] static bool IsBlockCompressed(D3DFORMAT format) noexcept;

    [[nodiscard]] static bool IsDepthFormat(D3DFORMAT format) noexcept;

    [[nodiscard]] static bool IsFourCC(D3DFORMAT format) noexcept;

    [[nodiscard]] static bool IsNullSurfaceFormat(D3DFORMAT format) noexcept;

    struct DepthFormatViews {
        DXGI_FORMAT resource   = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT dsv        = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT srv        = DXGI_FORMAT_UNKNOWN;
        bool        srvReadable = false;
    };

    [[nodiscard]] static bool GetDepthFormatViews(D3DFORMAT format, DepthFormatViews& out) noexcept;

    // Whether a depth-stencil view format carries a stencil plane. A read-only
    // DSV must ask for READ_ONLY_STENCIL only when there is a stencil to make
    // read-only; requesting it on a depth-only format is rejected.
    [[nodiscard]] static bool DsvFormatHasStencil(DXGI_FORMAT dsvFormat) noexcept;

    [[nodiscard]] static UINT BlockOrPixelSize(D3DFORMAT format) noexcept;

    [[nodiscard]] static UINT RowPitch(D3DFORMAT format, UINT width) noexcept;

    [[nodiscard]] static UINT RowCount(D3DFORMAT format, UINT height) noexcept;

    [[nodiscard]] static bool NeedsRB10Swap(D3DFORMAT format) noexcept;

    static void SwapRB10Rect(void* bits, UINT rowPitchBytes,
                             UINT widthPx, UINT heightPx) noexcept;

    [[nodiscard]] static DXGI_FORMAT BackBufferFormat(D3DFORMAT requested) noexcept;

    [[nodiscard]] static bool HasSRGBVariant(DXGI_FORMAT format) noexcept;

    [[nodiscard]] static DXGI_FORMAT ToTypeless(DXGI_FORMAT format) noexcept;

    [[nodiscard]] static DXGI_FORMAT ToLinearView(DXGI_FORMAT format) noexcept;

    [[nodiscard]] static DXGI_FORMAT ToSRGBView(DXGI_FORMAT format) noexcept;

    [[nodiscard]] static bool IsCpuExpandedLuminance(D3DFORMAT format) noexcept;

    [[nodiscard]] static FormatMapping ToDxgiVolume(D3DFORMAT format) noexcept;

    [[nodiscard]] static UINT ExpandedBytesPerPixel(D3DFORMAT format) noexcept;

    static void ExpandLumRect(D3DFORMAT format,
                              const void* src, UINT srcPitch,
                              void* dst, UINT dstPitch,
                              UINT widthPx, UINT heightPx) noexcept;

    static void ContractLumRect(D3DFORMAT format,
                                const void* src, UINT srcPitch,
                                void* dst, UINT dstPitch,
                                UINT widthPx, UINT heightPx) noexcept;
};

}

#endif
