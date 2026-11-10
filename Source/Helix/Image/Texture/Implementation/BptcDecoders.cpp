#include <Helix/Image/Texture/Convert.h>
#include <Helix/Image/Texture/Implementation/BcCommon.h>
#include <Helix/Image/Texture/Implementation/BptcTables.h>
#include <Helix/Image/Texture/Implementation/BuiltInBlockCodecs.h>

#include <cstring>

// BC7 (RGBA8) and BC6H (RGBA16Float) blocks to texels, from the block compression formats of Direct3D 11.
namespace helix::image::bptc
{
    namespace
    {
        struct Bc7Mode
        {
            u8 subsets, partitionBits, rotationBits, selectionBits, colorBits, alphaBits, endpointPBits, sharedPBits, indexBits, secondaryBits;
        };

        constexpr Bc7Mode kBc7Modes[8] = {
            {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
            {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2}, {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
        };

        u32 Unquantize(u32 value, u32 bits)
        {
            value <<= 8 - bits;
            return value | (value >> bits);
        }

        u32 Interpolate(u32 a, u32 b, u32 weight) { return ((64 - weight) * a + weight * b + 32) >> 6; }

        const u8* Weights(u32 bits) { return bits == 2 ? kWeights2 : bits == 3 ? kWeights3 : kWeights4; }

        void DecodeBc7(const u8* block, u8* texels)
        {
            u32 mode = 0;
            while (mode < 8 && !(block[0] & (1u << mode)))
                mode++;
            if (mode == 8)
            {
                std::memset(texels, 0, 64);
                return;
            }
            const Bc7Mode& m = kBc7Modes[mode];
            BitReader r{block, mode + 1};
            u32 partition = r.Read(m.partitionBits), rotation = r.Read(m.rotationBits), selection = r.Read(m.selectionBits);
            u32 endpoints[3][2][4] = {};
            for (u32 c = 0; c < 3; c++)
                for (u32 s = 0; s < m.subsets; s++)
                    for (u32 e = 0; e < 2; e++)
                        endpoints[s][e][c] = r.Read(m.colorBits);
            for (u32 s = 0; s < m.subsets; s++)
                for (u32 e = 0; e < 2; e++)
                    endpoints[s][e][3] = m.alphaBits ? r.Read(m.alphaBits) : 255;
            u32 colorBits = m.colorBits, alphaBits = m.alphaBits;
            if (m.endpointPBits || m.sharedPBits)
            {
                u32 p[3][2];
                for (u32 s = 0; s < m.subsets; s++)
                {
                    if (m.sharedPBits)
                        p[s][0] = p[s][1] = r.Read(1);
                    else
                    {
                        p[s][0] = r.Read(1);
                        p[s][1] = r.Read(1);
                    }
                }
                for (u32 s = 0; s < m.subsets; s++)
                    for (u32 e = 0; e < 2; e++)
                        for (u32 c = 0; c < 4; c++)
                            if (c < 3 || m.alphaBits)
                                endpoints[s][e][c] = (endpoints[s][e][c] << 1) | p[s][e];
                colorBits++;
                alphaBits += m.alphaBits ? 1 : 0;
            }
            for (u32 s = 0; s < m.subsets; s++)
                for (u32 e = 0; e < 2; e++)
                {
                    for (u32 c = 0; c < 3; c++)
                        endpoints[s][e][c] = Unquantize(endpoints[s][e][c], colorBits);
                    if (m.alphaBits)
                        endpoints[s][e][3] = Unquantize(endpoints[s][e][3], alphaBits);
                }
            u32 primary[16], secondary[16];
            for (u32 i = 0; i < 16; i++)
                primary[i] = r.Read(m.indexBits - (IsAnchor(m.subsets, partition, i) && (i == 0 || m.subsets > 1) ? 1 : 0));
            if (m.secondaryBits)
                for (u32 i = 0; i < 16; i++)
                    secondary[i] = r.Read(m.secondaryBits - (i == 0 ? 1 : 0));
            for (u32 i = 0; i < 16; i++)
            {
                u32 s = Subset(m.subsets, partition, i);
                u32 colorIndex = primary[i], alphaIndex = primary[i], colorBitsUsed = m.indexBits, alphaBitsUsed = m.indexBits;
                if (m.secondaryBits)
                {
                    alphaIndex = secondary[i];
                    alphaBitsUsed = m.secondaryBits;
                    if (selection)
                    {
                        colorIndex = secondary[i];
                        colorBitsUsed = m.secondaryBits;
                        alphaIndex = primary[i];
                        alphaBitsUsed = m.indexBits;
                    }
                }
                u8* t = texels + i * 4;
                for (u32 c = 0; c < 3; c++)
                    t[c] = u8(Interpolate(endpoints[s][0][c], endpoints[s][1][c], Weights(colorBitsUsed)[colorIndex]));
                t[3] = u8(Interpolate(endpoints[s][0][3], endpoints[s][1][3], Weights(alphaBitsUsed)[alphaIndex]));
                if (rotation)
                {
                    u8 a = t[3];
                    t[3] = t[rotation - 1];
                    t[rotation - 1] = a;
                }
            }
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

        constexpr Bc6Mode kBc6Modes[14] = {
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
        struct Layouts
        {
            u8 field[14][80], bit[14][80], count[14] = {};

            Layouts()
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

        i32 SignExtend(u32 value, u32 bits) { return bits >= 32 ? i32(value) : i32(value << (32 - bits)) >> (32 - bits); }

        i32 UnquantizeHalf(i32 x, u32 bits, bool isSigned)
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

        u16 FinishHalf(i32 x, bool isSigned)
        {
            if (!isSigned)
                return u16((x * 31) >> 6);
            if (x < 0)
                return u16(0x8000 | ((-x * 31) >> 5));
            return u16((x * 31) >> 5);
        }

        template <bool Signed>
        void DecodeBc6(const u8* block, u8* texels)
        {
            static const Layouts layouts;
            u16* out = reinterpret_cast<u16*>(texels);
            BitReader r{block};
            u32 code = r.Read(2);
            if (code > 1)
                code |= r.Read(3) << 2;
            i32 mode = -1;
            for (u32 m = 0; m < 14; m++)
                if (kBc6Modes[m].code == code)
                    mode = i32(m);
            if (mode < 0)
            {
                for (u32 i = 0; i < 16; i++)
                {
                    out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = 0;
                    out[i * 4 + 3] = 0x3C00;
                }
                return;
            }
            const Bc6Mode& m = kBc6Modes[mode];
            u32 e[4][3] = {};
            for (u32 i = 0; i < layouts.count[mode]; i++)
                e[layouts.field[mode][i] / 3][layouts.field[mode][i] % 3] |= r.Read(1) << layouts.bit[mode][i];
            u32 partition = m.regions == 2 ? r.Read(5) : 0;
            u32 endpoints = m.regions * 2u;
            i32 v[4][3];
            u32 mask = (1u << m.endpointBits) - 1;
            for (u32 c = 0; c < 3; c++)
            {
                v[0][c] = Signed ? SignExtend(e[0][c], m.endpointBits) : i32(e[0][c]);
                for (u32 k = 1; k < endpoints; k++)
                {
                    if (m.transformed)
                    {
                        i32 sum = i32((u32(e[0][c]) + u32(SignExtend(e[k][c], m.deltaBits[c]))) & mask);
                        v[k][c] = Signed ? SignExtend(u32(sum), m.endpointBits) : sum;
                    }
                    else
                        v[k][c] = Signed ? SignExtend(e[k][c], m.endpointBits) : i32(e[k][c]);
                }
                for (u32 k = 0; k < endpoints; k++)
                    v[k][c] = UnquantizeHalf(v[k][c], m.endpointBits, Signed);
            }
            u32 indexBits = m.regions == 2 ? 3 : 4;
            const u8* weights = Weights(indexBits);
            for (u32 i = 0; i < 16; i++)
            {
                bool anchor = i == 0 || (m.regions == 2 && i == kAnchors2[partition]);
                u32 index = r.Read(indexBits - (anchor ? 1 : 0));
                u32 s = m.regions == 2 ? (kPartitions2[partition] >> i) & 1 : 0;
                for (u32 c = 0; c < 3; c++)
                {
                    i32 a = v[s * 2][c], b = v[s * 2 + 1][c], w = weights[index];
                    i32 x = ((64 - w) * a + w * b + 32) >> 6;
                    out[i * 4 + c] = FinishHalf(x, Signed);
                }
                out[i * 4 + 3] = 0x3C00;
            }
        }
    }

    void AddDecoders(BlockCodec* codecs, usize& count)
    {
        codecs[count++] = {PixelFormat::BC6H, PixelFormat::RGBA16Float, &DecodeBc6<false>, nullptr};
        codecs[count++] = {PixelFormat::BC6HSigned, PixelFormat::RGBA16Float, &DecodeBc6<true>, nullptr};
        codecs[count++] = {PixelFormat::BC7, PixelFormat::RGBA8, &DecodeBc7, nullptr};
    }
}
