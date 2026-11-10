#pragma once

#include <Helix/Core/Strings/StringView.h>

namespace helix::image
{
    // The layouts a decoded texture can have: uncompressed formats, packed formats and GPU block formats.
    enum class PixelFormat : u8
    {
        Unknown,
        R8,
        RG8,
        RGB8,
        RGBA8,
        BGRA8,
        R8Snorm,
        RG8Snorm,
        RGBA8Snorm,
        R16,
        RG16,
        RGB16,
        RGBA16,
        R16Float,
        RG16Float,
        RGB16Float,
        RGBA16Float,
        R32Float,
        RG32Float,
        RGB32Float,
        RGBA32Float,
        R32Uint,
        RGB10A2,
        RG11B10Float,
        RGB9E5Float,
        B5G6R5,
        B5G5R5A1,
        B4G4R4A4,
        BC1,
        BC2,
        BC3,
        BC4,
        BC4Snorm,
        BC5,
        BC5Snorm,
        BC6H,
        BC6HSigned,
        BC7,
        ETC2RGB,
        ETC2RGBA1,
        ETC2RGBA,
        EACR11,
        EACR11Snorm,
        EACRG11,
        EACRG11Snorm,
        ASTC4x4,
        ASTC5x4,
        ASTC5x5,
        ASTC6x5,
        ASTC6x6,
        ASTC8x5,
        ASTC8x6,
        ASTC8x8,
        ASTC10x5,
        ASTC10x6,
        ASTC10x8,
        ASTC10x10,
        ASTC12x10,
        ASTC12x12,
        Count,
    };

    enum class PixelKind : u8
    {
        Unorm,
        Snorm,
        Uint,
        Float,
        Packed,
        Block,
    };

    struct PixelFormatInfo
    {
        StringView name;
        PixelKind kind;
        u8 channels;
        // Uncompressed formats are 1 x 1 blocks of one pixel.
        u8 blockWidth;
        u8 blockHeight;
        u8 bytesPerBlock;
    };

    const PixelFormatInfo& FormatInfo(PixelFormat format);
    // Bytes of one 2D surface (one depth slice), whole blocks for block formats.
    u64 SurfaceBytes(PixelFormat format, u32 width, u32 height);
    inline bool IsBlockFormat(PixelFormat format) { return FormatInfo(format).kind == PixelKind::Block; }
}
