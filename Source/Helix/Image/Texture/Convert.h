#pragma once

#include <Helix/Image/Formats/PixelFormat.h>

namespace helix::image
{
    // Uncompressed formats convert through RGBA floats: unorm values are 0 to 1, snorm -1 to 1, uint formats keep their
    // integer value. Missing channels read as 0 for G and B and 1 for A, as a GPU samples them.
    bool CanConvert(PixelFormat format);
    void ToFloat(PixelFormat format, const u8* source, float* rgba, usize count);
    void FromFloat(PixelFormat format, const float* rgba, u8* destination, usize count);
    // `count` pixels from one format to another; false when either is a block format.
    bool ConvertPixels(PixelFormat from, const u8* source, PixelFormat to, u8* destination, usize count);

    u16 FloatToHalf(float value);
    float HalfToFloat(u16 value);
}
