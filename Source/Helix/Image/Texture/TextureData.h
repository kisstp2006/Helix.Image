#pragma once

#include <Helix/Core/Containers/Array.h>
#include <Helix/Image/Formats/PixelFormat.h>

namespace helix::image
{
    enum class TextureDimension : u8
    {
        Texture2D,
        Cube,
        Texture3D,
    };

    // What the file says about its color values; images that say nothing leave the choice to the importer.
    enum class ColorSpace : u8
    {
        Unspecified,
        Linear,
        Srgb,
    };

    struct Subresource
    {
        u64 offset = 0;
        u64 size = 0;
        u32 width = 0, height = 0, depth = 0;
    };

    // A decoded image or texture file: every layer, face and mip in one block, rows top first, with a table that places
    // each of them. Subresources are ordered layer, then face, then mip.
    struct TextureData
    {
        PixelFormat format = PixelFormat::Unknown;
        ColorSpace colorSpace = ColorSpace::Unspecified;
        TextureDimension dimension = TextureDimension::Texture2D;
        u32 width = 0, height = 0, depth = 1;
        u32 layers = 1, faces = 1, mips = 1;
        Array<u8> data;
        Array<Subresource> subresources;
        // Animated images (GIF): the time each layer is shown, in milliseconds.
        Array<u32> frameDelays;

        // Sizes the block and the table for the given shape; false when the sizes are zero or too large to hold.
        bool Allocate(PixelFormat pixelFormat, u32 w, u32 h, u32 d = 1, u32 layerCount = 1, u32 faceCount = 1, u32 mipCount = 1);

        u32 Index(u32 layer, u32 face, u32 mip) const { return (layer * faces + face) * mips + mip; }
        const Subresource& At(u32 layer, u32 face, u32 mip) const { return subresources[Index(layer, face, mip)]; }
        u8* Pixels(u32 layer = 0, u32 face = 0, u32 mip = 0) { return data.Data() + At(layer, face, mip).offset; }
        const u8* Pixels(u32 layer = 0, u32 face = 0, u32 mip = 0) const { return data.Data() + At(layer, face, mip).offset; }
    };

    // The largest texture a file may describe: larger sizes are treated as damaged input.
    inline constexpr u64 kMaxTextureBytes = u64(1) << 32;
}
