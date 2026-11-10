#pragma once

#include <Helix/Core/Basics/Types.h>

// The partition and anchor tables of BC6H and BC7.
namespace helix::image::bptc
{
    // Two subsets: bit i set puts texel i in subset 1.
    inline constexpr u16 kPartitions2[64] = {
        0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
        0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
        0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
        0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
    };

    inline constexpr u8 kPartitions3[64][16] = {
        {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1}, {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1},
        {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
        {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2},
        {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2}, {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
        {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2}, {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2}, {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2},
        {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0}, {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2}, {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
        {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1}, {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2},
        {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1}, {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2}, {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
        {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0}, {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2}, {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0},
        {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1}, {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2}, {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
        {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1}, {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1}, {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2},
        {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2}, {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
        {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0}, {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0}, {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0},
        {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1}, {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1}, {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
        {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1}, {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2}, {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1},
        {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1}, {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1}, {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
        {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2}, {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1}, {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2},
        {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2}, {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2}, {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
        {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2}, {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2},
        {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2}, {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 2},
        {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1}, {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2}, {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
        {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0},
    };

    // The texel whose index loses its top bit, for subset 1 of the two subset partitions.
    inline constexpr u8 kAnchors2[64] = {
        15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
        15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
    };

    // For subsets 1 and 2 of the three subset partitions.
    inline constexpr u8 kAnchors3[2][64] = {
        {3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
         8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3},
        {15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
         15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8},
    };

    struct Bc7Mode
    {
        u8 subsets, partitionBits, rotationBits, selectionBits, colorBits, alphaBits, endpointPBits, sharedPBits, indexBits, secondaryBits;
    };

    inline constexpr Bc7Mode kBc7Modes[8] = {
        {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
        {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2}, {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
    };

    inline constexpr u8 kWeights2[4] = {0, 21, 43, 64};
    inline constexpr u8 kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
    inline constexpr u8 kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

    inline u32 Subset(u32 subsets, u32 partition, u32 texel)
    {
        if (subsets == 2)
            return (kPartitions2[partition] >> texel) & 1;
        if (subsets == 3)
            return kPartitions3[partition][texel];
        return 0;
    }

    inline const u8* Weights(u32 bits) { return bits == 2 ? kWeights2 : bits == 3 ? kWeights3 : kWeights4; }

    inline u32 Interpolate(u32 a, u32 b, u32 weight) { return ((64 - weight) * a + weight * b + 32) >> 6; }

    inline u32 Unquantize(u32 value, u32 bits)
    {
        value <<= 8 - bits;
        return value | (value >> bits);
    }

    inline bool IsAnchor(u32 subsets, u32 partition, u32 texel)
    {
        if (texel == 0)
            return true;
        if (subsets == 2)
            return texel == kAnchors2[partition];
        if (subsets == 3)
            return texel == kAnchors3[0][partition] || texel == kAnchors3[1][partition];
        return false;
    }

    // BC6H: the endpoint bits of each mode in stream order, written as in the format description: x[hi:lo] puts lo
    // first, x[lo:hi] (reversed) puts hi first. r0 to r3 are the endpoints w, x, y, z.
    struct Bc6Mode
    {
        u32 code;
        u8 regions;
        bool transformed;
        u8 endpointBits;
        u8 deltaBits[3];
        const char* layout;
    };

    inline constexpr Bc6Mode kBc6Modes[14] = {
        {0, 2, true, 10, {5, 5, 5}, "g2[4] b2[4] b3[4] r0[9:0] g0[9:0] b0[9:0] r1[4:0] g3[4] g2[3:0] g1[4:0] b3[0] g3[3:0] b1[4:0] b3[1] b2[3:0] r2[4:0] b3[2] r3[4:0] b3[3]"},
        {1, 2, true, 7, {6, 6, 6}, "g2[5] g3[4] g3[5] r0[6:0] b3[0] b3[1] b2[4] g0[6:0] b2[5] b3[2] g2[4] b0[6:0] b3[3] b3[5] b3[4] r1[5:0] g2[3:0] g1[5:0] g3[3:0] b1[5:0] b2[3:0] r2[5:0] r3[5:0]"},
        {2, 2, true, 11, {5, 4, 4}, "r0[9:0] g0[9:0] b0[9:0] r1[4:0] r0[10] g2[3:0] g1[3:0] g0[10] b3[0] g3[3:0] b1[3:0] b0[10] b3[1] b2[3:0] r2[4:0] b3[2] r3[4:0] b3[3]"},
        {6, 2, true, 11, {4, 5, 4}, "r0[9:0] g0[9:0] b0[9:0] r1[3:0] r0[10] g3[4] g2[3:0] g1[4:0] g0[10] g3[3:0] b1[3:0] b0[10] b3[1] b2[3:0] r2[3:0] b3[0] b3[2] r3[3:0] g2[4] b3[3]"},
        {10, 2, true, 11, {4, 4, 5}, "r0[9:0] g0[9:0] b0[9:0] r1[3:0] r0[10] b2[4] g2[3:0] g1[3:0] g0[10] b3[0] g3[3:0] b1[4:0] b0[10] b2[3:0] r2[3:0] b3[1] b3[2] r3[3:0] b3[4] b3[3]"},
        {14, 2, true, 9, {5, 5, 5}, "r0[8:0] b2[4] g0[8:0] g2[4] b0[8:0] b3[4] r1[4:0] g3[4] g2[3:0] g1[4:0] b3[0] g3[3:0] b1[4:0] b3[1] b2[3:0] r2[4:0] b3[2] r3[4:0] b3[3]"},
        {18, 2, true, 8, {6, 5, 5}, "r0[7:0] g3[4] b2[4] g0[7:0] b3[2] g2[4] b0[7:0] b3[3] b3[4] r1[5:0] g2[3:0] g1[4:0] b3[0] g3[3:0] b1[4:0] b3[1] b2[3:0] r2[5:0] r3[5:0]"},
        {22, 2, true, 8, {5, 6, 5}, "r0[7:0] b3[0] b2[4] g0[7:0] g2[5] g2[4] b0[7:0] g3[5] b3[4] r1[4:0] g3[4] g2[3:0] g1[5:0] g3[3:0] b1[4:0] b3[1] b2[3:0] r2[4:0] b3[2] r3[4:0] b3[3]"},
        {26, 2, true, 8, {5, 5, 6}, "r0[7:0] b3[1] b2[4] g0[7:0] b2[5] g2[4] b0[7:0] b3[5] b3[4] r1[4:0] g3[4] g2[3:0] g1[4:0] b3[0] g3[3:0] b1[5:0] b2[3:0] r2[4:0] b3[2] r3[4:0] b3[3]"},
        {30, 2, false, 6, {6, 6, 6}, "r0[5:0] g3[4] b3[0] b3[1] b2[4] g0[5:0] g2[5] b2[5] b3[2] g2[4] b0[5:0] g3[5] b3[3] b3[5] b3[4] r1[5:0] g2[3:0] g1[5:0] g3[3:0] b1[5:0] b2[3:0] r2[5:0] r3[5:0]"},
        {3, 1, false, 10, {10, 10, 10}, "r0[9:0] g0[9:0] b0[9:0] r1[9:0] g1[9:0] b1[9:0]"},
        {7, 1, true, 11, {9, 9, 9}, "r0[9:0] g0[9:0] b0[9:0] r1[8:0] r0[10] g1[8:0] g0[10] b1[8:0] b0[10]"},
        {11, 1, true, 12, {8, 8, 8}, "r0[9:0] g0[9:0] b0[9:0] r1[7:0] r0[10:11] g1[7:0] g0[10:11] b1[7:0] b0[10:11]"},
        {15, 1, true, 16, {4, 4, 4}, "r0[9:0] g0[9:0] b0[9:0] r1[3:0] r0[10:15] g1[3:0] g0[10:15] b1[3:0] b0[10:15]"},
    };

    // The layouts as (endpoint * 3 + channel, bit) pairs.
    struct Bc6Layouts
    {
        u8 field[14][80], bit[14][80], count[14] = {};

        Bc6Layouts()
        {
            for (u32 m = 0; m < 14; m++)
            {
                const char* p = kBc6Modes[m].layout;
                while (*p)
                {
                    while (*p == ' ')
                        p++;
                    u32 channel = *p == 'r' ? 0 : *p == 'g' ? 1 : 2, endpoint = u32(p[1] - '0');
                    p += 3;
                    u32 a = 0, b;
                    while (*p >= '0' && *p <= '9')
                        a = a * 10 + u32(*p++ - '0');
                    b = a;
                    if (*p == ':')
                    {
                        p++;
                        b = 0;
                        while (*p >= '0' && *p <= '9')
                            b = b * 10 + u32(*p++ - '0');
                    }
                    p++;
                    // x[a:b] runs from b to a.
                    for (i32 v = i32(b);; v += a >= b ? 1 : -1)
                    {
                        field[m][count[m]] = u8(endpoint * 3 + channel);
                        bit[m][count[m]++] = u8(v);
                        if (v == i32(a))
                            break;
                    }
                }
            }
        }
    };

    inline i32 SignExtend(u32 value, u32 bits) { return bits >= 32 ? i32(value) : i32(value << (32 - bits)) >> (32 - bits); }

    inline i32 UnquantizeHalf(i32 x, u32 bits, bool isSigned)
    {
        if (!isSigned)
        {
            if (bits >= 15)
                return x;
            if (x == 0)
                return 0;
            if (x == (1 << bits) - 1)
                return 0xFFFF;
            return ((x << 16) + 0x8000) >> bits;
        }
        if (bits >= 16)
            return x;
        bool negative = x < 0;
        i32 v = negative ? -x : x;
        if (v >= (1 << (bits - 1)) - 1)
            v = 0x7FFF;
        else if (v)
            v = ((v << 15) + 0x4000) >> (bits - 1);
        return negative ? -v : v;
    }

    inline u16 FinishHalf(i32 x, bool isSigned)
    {
        if (!isSigned)
            return u16((x * 31) >> 6);
        if (x < 0)
            return u16(0x8000 | ((-x * 31) >> 5));
        return u16((x * 31) >> 5);
    }

    inline const Bc6Layouts& Bc6LayoutTable()
    {
        static const Bc6Layouts layouts;
        return layouts;
    }

    // Writes a 128 bit block from its lowest bit on.
    struct BitWriter128
    {
        u8 block[16] = {};
        u32 at = 0;
        void Put(u32 value, u32 bits)
        {
            for (u32 i = 0; i < bits; i++, at++)
                block[at >> 3] |= u8(((value >> i) & 1) << (at & 7));
        }
    };

    // Reads a 128 bit block from its lowest bit on.
    struct BitReader
    {
        const u8* block;
        u32 at = 0;

        u32 Read(u32 count)
        {
            u32 value = 0;
            for (u32 i = 0; i < count; i++, at++)
                value |= u32((block[at >> 3] >> (at & 7)) & 1) << i;
            return value;
        }
    };
}
