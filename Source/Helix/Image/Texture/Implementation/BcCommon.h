#pragma once

#include <Helix/Image/Texture/BlockCodec.h>

namespace helix::image::bc
{
    inline void Expand565(u32 c, u8* rgb)
    {
        u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        rgb[0] = u8((r << 3) | (r >> 2));
        rgb[1] = u8((g << 2) | (g >> 4));
        rgb[2] = u8((b << 3) | (b >> 2));
    }

    // Rounded division that works for negative values too.
    inline i32 Divide(i32 value, i32 by) { return value >= 0 ? (value + by / 2) / by : -((-value + by / 2) / by); }

    // The BC1 color block into 16 RGBA texels; without `allowTransparent` (BC2, BC3) it always has 4 colors.
    void DecodeColor(const u8* block, u8* rgba, bool allowTransparent);
    // A BC4 block into 16 values `stride` bytes apart.
    void DecodeChannel(const u8* block, u8* out, u32 stride, bool isSigned);
}
