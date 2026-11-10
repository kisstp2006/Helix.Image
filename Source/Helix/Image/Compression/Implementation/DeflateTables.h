#pragma once

#include <Helix/Core/Basics/Types.h>

namespace helix::image::deflate
{
    // Length codes 257..285 and distance codes 0..29: base values and extra bits from RFC 1951 section 3.2.5.
    inline constexpr u16 kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    inline constexpr u8 kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    inline constexpr u16 kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                          193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    inline constexpr u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    // Order in which code length code lengths are stored.
    inline constexpr u8 kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    inline u32 ReverseBits(u32 code, u32 length)
    {
        u32 r = 0;
        for (u32 i = 0; i < length; i++)
        {
            r = (r << 1) | (code & 1);
            code >>= 1;
        }
        return r;
    }

    // Fixed Huffman code lengths of RFC 1951 section 3.2.6.
    inline void FixedLengths(u8* literal, u8* distance)
    {
        for (u32 i = 0; i < 288; i++)
            literal[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        for (u32 i = 0; i < 30; i++)
            distance[i] = 5;
    }
}
