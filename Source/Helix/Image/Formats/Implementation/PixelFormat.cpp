#include <Helix/Image/Formats/PixelFormat.h>

namespace helix::image
{
    namespace
    {
        using K = PixelKind;

        constexpr PixelFormatInfo kInfo[] = {
            {"Unknown", K::Unorm, 0, 1, 1, 0},
            {"R8", K::Unorm, 1, 1, 1, 1},
            {"RG8", K::Unorm, 2, 1, 1, 2},
            {"RGB8", K::Unorm, 3, 1, 1, 3},
            {"RGBA8", K::Unorm, 4, 1, 1, 4},
            {"BGRA8", K::Unorm, 4, 1, 1, 4},
            {"R8Snorm", K::Snorm, 1, 1, 1, 1},
            {"RG8Snorm", K::Snorm, 2, 1, 1, 2},
            {"RGBA8Snorm", K::Snorm, 4, 1, 1, 4},
            {"R16", K::Unorm, 1, 1, 1, 2},
            {"RG16", K::Unorm, 2, 1, 1, 4},
            {"RGB16", K::Unorm, 3, 1, 1, 6},
            {"RGBA16", K::Unorm, 4, 1, 1, 8},
            {"R16Float", K::Float, 1, 1, 1, 2},
            {"RG16Float", K::Float, 2, 1, 1, 4},
            {"RGB16Float", K::Float, 3, 1, 1, 6},
            {"RGBA16Float", K::Float, 4, 1, 1, 8},
            {"R32Float", K::Float, 1, 1, 1, 4},
            {"RG32Float", K::Float, 2, 1, 1, 8},
            {"RGB32Float", K::Float, 3, 1, 1, 12},
            {"RGBA32Float", K::Float, 4, 1, 1, 16},
            {"R32Uint", K::Uint, 1, 1, 1, 4},
            {"RGB10A2", K::Packed, 4, 1, 1, 4},
            {"RG11B10Float", K::Packed, 3, 1, 1, 4},
            {"RGB9E5Float", K::Packed, 3, 1, 1, 4},
            {"B5G6R5", K::Packed, 3, 1, 1, 2},
            {"B5G5R5A1", K::Packed, 4, 1, 1, 2},
            {"B4G4R4A4", K::Packed, 4, 1, 1, 2},
            {"BC1", K::Block, 4, 4, 4, 8},
            {"BC2", K::Block, 4, 4, 4, 16},
            {"BC3", K::Block, 4, 4, 4, 16},
            {"BC4", K::Block, 1, 4, 4, 8},
            {"BC4Snorm", K::Block, 1, 4, 4, 8},
            {"BC5", K::Block, 2, 4, 4, 16},
            {"BC5Snorm", K::Block, 2, 4, 4, 16},
            {"BC6H", K::Block, 3, 4, 4, 16},
            {"BC6HSigned", K::Block, 3, 4, 4, 16},
            {"BC7", K::Block, 4, 4, 4, 16},
            {"ETC2RGB", K::Block, 3, 4, 4, 8},
            {"ETC2RGBA1", K::Block, 4, 4, 4, 8},
            {"ETC2RGBA", K::Block, 4, 4, 4, 16},
            {"EACR11", K::Block, 1, 4, 4, 8},
            {"EACR11Snorm", K::Block, 1, 4, 4, 8},
            {"EACRG11", K::Block, 2, 4, 4, 16},
            {"EACRG11Snorm", K::Block, 2, 4, 4, 16},
            {"ASTC4x4", K::Block, 4, 4, 4, 16},
            {"ASTC5x4", K::Block, 4, 5, 4, 16},
            {"ASTC5x5", K::Block, 4, 5, 5, 16},
            {"ASTC6x5", K::Block, 4, 6, 5, 16},
            {"ASTC6x6", K::Block, 4, 6, 6, 16},
            {"ASTC8x5", K::Block, 4, 8, 5, 16},
            {"ASTC8x6", K::Block, 4, 8, 6, 16},
            {"ASTC8x8", K::Block, 4, 8, 8, 16},
            {"ASTC10x5", K::Block, 4, 10, 5, 16},
            {"ASTC10x6", K::Block, 4, 10, 6, 16},
            {"ASTC10x8", K::Block, 4, 10, 8, 16},
            {"ASTC10x10", K::Block, 4, 10, 10, 16},
            {"ASTC12x10", K::Block, 4, 12, 10, 16},
            {"ASTC12x12", K::Block, 4, 12, 12, 16},
        };
        static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == usize(PixelFormat::Count));
    }

    const PixelFormatInfo& FormatInfo(PixelFormat format) { return kInfo[format < PixelFormat::Count ? usize(format) : 0]; }

    u64 SurfaceBytes(PixelFormat format, u32 width, u32 height)
    {
        const PixelFormatInfo& info = FormatInfo(format);
        u64 columns = (u64(width) + info.blockWidth - 1) / info.blockWidth;
        u64 rows = (u64(height) + info.blockHeight - 1) / info.blockHeight;
        return columns * rows * info.bytesPerBlock;
    }
}
