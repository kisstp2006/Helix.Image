#pragma once

#include <Helix/Core/Strings/String.h>
#include <Helix/Image/Texture/TextureData.h>

namespace helix::image
{
    // The resampling filters: TextureResizeAlgorithm (Mitchell, Bilinear) and the mip filters (Box, Kaiser).
    enum class ResampleFilter : u8
    {
        Mitchell,
        Bilinear,
        // The area each target texel covers, so halving a side averages 2 texels and an odd side takes 3.
        Box,
        // A Kaiser windowed sinc of radius 3.
        Kaiser,
    };

    // The size the Max Size setting leaves: unchanged when both sides fit, else the larger side at `maxSize` and the
    // other scaled with it (at least 1).
    void FitToMaxSize(u32 width, u32 height, u32 maxSize, u32& outWidth, u32& outHeight);

    // RGBA float pixels, rows top first, from one size to another: one pass per axis, the edges clamped. Rows run as
    // jobs when Helix.Jobs is initialized, with the same result.
    void Resample(const float* source, u32 width, u32 height, float* destination, u32 newWidth, u32 newHeight, ResampleFilter filter);

    // Every layer and face of the top level of an uncompressed texture at a new size, in the same format (sRGB data
    // filtered in linear light). The result has one mip.
    bool ResizeTexture(const TextureData& source, u32 width, u32 height, ResampleFilter filter, bool srgb, TextureData& out, String& error);
}
