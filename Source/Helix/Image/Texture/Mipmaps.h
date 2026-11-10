#pragma once

#include <Helix/Core/Strings/String.h>
#include <Helix/Image/Texture/TextureData.h>

namespace helix::image
{
    // The mip filters of the texture importer.
    enum class MipFilter : u8
    {
        Box,
        Kaiser,
    };

    // The mip settings of the texture importer.
    struct MipmapSettings
    {
        MipFilter filter = MipFilter::Box;
        // Color in sRGB: filtered in linear light.
        bool srgb = false;
        // mipMapsPreserveCoverage: each level keeps the share of texels whose alpha passes `alphaReference`.
        bool preserveCoverage = false;
        float alphaReference = 0.5f;
        // RGB holds a normal (0 to 1 for -1 to 1), renormalized on every level.
        bool normalMap = false;
        // borderMipmap: the edge texels of every level keep the edge of the top level.
        bool borderMipmap = false;
        // fadeout: levels fade to gray (a flat normal for normal maps) from `fadeStart` to `fadeEnd`.
        bool fadeout = false;
        u32 fadeStart = 1;
        u32 fadeEnd = 3;
    };

    // The number of levels down to 1 x 1.
    u32 MipCount(u32 width, u32 height);

    // A full mip chain for every layer and face of the top level of an uncompressed 2D, cube or array texture, in the
    // same format. Each level is made from the one above it; the top level is copied unchanged unless it fades.
    bool GenerateMipmaps(const TextureData& source, const MipmapSettings& settings, TextureData& out, String& error);
}
