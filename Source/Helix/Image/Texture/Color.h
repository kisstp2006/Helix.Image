#pragma once

#include <Helix/Core/Basics/Types.h>

namespace helix::image
{
    // The sRGB transfer curves (IEC 61966-2-1), values 0 to 1.
    float SrgbToLinear(float value);
    float LinearToSrgb(float value);
    // An 8 bit sRGB value in linear light, from a table.
    float SrgbByteToLinear(u8 value);
    // The 8 bit sRGB value of a linear value: the same byte as rounding LinearToSrgb(value) * 255, from a table of the
    // linear values halfway between bytes.
    u8 LinearToSrgbByte(float value);

    // Converts the RGB of RGBA float pixels in place; alpha is linear in both.
    void SrgbToLinear(float* rgba, usize count);
    void LinearToSrgb(float* rgba, usize count);
}
