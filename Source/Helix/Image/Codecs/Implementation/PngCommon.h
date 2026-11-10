#pragma once

#include <Helix/Core/Basics/Types.h>

namespace helix::image::png
{
    inline constexpr u8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    inline u32 ReadBe32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

    inline u8 Paeth(u8 a, u8 b, u8 c)
    {
        i32 p = i32(a) + i32(b) - i32(c);
        i32 pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
        return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
    }

    // Adam7 passes: start x, start y, step x, step y.
    inline constexpr u8 kAdam7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
}
