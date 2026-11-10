#include <Helix/Image/Texture/Implementation/AstcCommon.h>
#include <Helix/Image/Texture/Implementation/BuiltInBlockCodecs.h>

#include <cstring>

// ASTC LDR blocks to RGBA8 texels, from the ASTC chapter of the Khronos Data Format Specification. Blocks that are not
// valid in the LDR profile (HDR endpoints, reserved modes, too many values) give the error color, magenta.
namespace helix::image::astc
{
    namespace
    {
        u32 Bit(const u8* block, u32 at) { return (block[at >> 3] >> (at & 7)) & 1; }

        u32 Bits(const u8* block, u32 at, u32 count)
        {
            u32 v = 0;
            for (u32 i = 0; i < count; i++)
                v |= Bit(block, at + i) << i;
            return v;
        }

        void Error(u8* texels, u32 count)
        {
            for (u32 i = 0; i < count; i++)
            {
                texels[i * 4] = 255;
                texels[i * 4 + 1] = 0;
                texels[i * 4 + 2] = 255;
                texels[i * 4 + 3] = 255;
            }
        }

        // Splits the high bit of `a` into `b`, as the base and offset endpoint modes do.
        void BitTransferSigned(i32& a, i32& b)
        {
            b >>= 1;
            b |= a & 0x80;
            a >>= 1;
            a &= 0x3F;
            if (a & 0x20)
                a -= 0x40;
        }

        i32 Clamp255(i32 v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

        void BlueContract(i32* c)
        {
            c[0] = (c[0] + c[2]) >> 1;
            c[1] = (c[1] + c[2]) >> 1;
        }

        // The two endpoints of one partition from its unquantized values; false for HDR modes.
        bool Endpoints(u32 cem, const i32* v, i32 e0[4], i32 e1[4])
        {
            i32 a[8];
            std::memcpy(a, v, sizeof(a));
            auto set = [](i32* e, i32 r, i32 g, i32 b, i32 alpha) {
                e[0] = r;
                e[1] = g;
                e[2] = b;
                e[3] = alpha;
            };
            switch (cem)
            {
            case 0:
                set(e0, a[0], a[0], a[0], 255);
                set(e1, a[1], a[1], a[1], 255);
                break;
            case 1:
            {
                i32 l0 = (a[0] >> 2) | (a[1] & 0xC0), l1 = l0 + (a[1] & 0x3F);
                l1 = l1 > 255 ? 255 : l1;
                set(e0, l0, l0, l0, 255);
                set(e1, l1, l1, l1, 255);
                break;
            }
            case 4:
                set(e0, a[0], a[0], a[0], a[2]);
                set(e1, a[1], a[1], a[1], a[3]);
                break;
            case 5:
                BitTransferSigned(a[1], a[0]);
                BitTransferSigned(a[3], a[2]);
                set(e0, a[0], a[0], a[0], a[2]);
                set(e1, a[0] + a[1], a[0] + a[1], a[0] + a[1], a[2] + a[3]);
                break;
            case 6:
                set(e0, (a[0] * a[3]) >> 8, (a[1] * a[3]) >> 8, (a[2] * a[3]) >> 8, 255);
                set(e1, a[0], a[1], a[2], 255);
                break;
            case 8: case 12:
            {
                i32 alpha0 = cem == 12 ? a[6] : 255, alpha1 = cem == 12 ? a[7] : 255;
                if (a[1] + a[3] + a[5] >= a[0] + a[2] + a[4])
                {
                    set(e0, a[0], a[2], a[4], alpha0);
                    set(e1, a[1], a[3], a[5], alpha1);
                }
                else
                {
                    set(e0, a[1], a[3], a[5], alpha1);
                    set(e1, a[0], a[2], a[4], alpha0);
                    BlueContract(e0);
                    BlueContract(e1);
                }
                break;
            }
            case 9: case 13:
            {
                BitTransferSigned(a[1], a[0]);
                BitTransferSigned(a[3], a[2]);
                BitTransferSigned(a[5], a[4]);
                if (cem == 13)
                    BitTransferSigned(a[7], a[6]);
                i32 alpha0 = cem == 13 ? a[6] : 255, alpha1 = cem == 13 ? a[6] + a[7] : 255;
                if (a[1] + a[3] + a[5] >= 0)
                {
                    set(e0, a[0], a[2], a[4], alpha0);
                    set(e1, a[0] + a[1], a[2] + a[3], a[4] + a[5], alpha1);
                }
                else
                {
                    set(e0, a[0] + a[1], a[2] + a[3], a[4] + a[5], alpha1);
                    set(e1, a[0], a[2], a[4], alpha0);
                    BlueContract(e0);
                    BlueContract(e1);
                }
                break;
            }
            case 10:
                set(e0, (a[0] * a[3]) >> 8, (a[1] * a[3]) >> 8, (a[2] * a[3]) >> 8, a[4]);
                set(e1, a[0], a[1], a[2], a[5]);
                break;
            default: return false;
            }
            for (int c = 0; c < 4; c++)
            {
                e0[c] = Clamp255(e0[c]);
                e1[c] = Clamp255(e1[c]);
            }
            return true;
        }

        template <u32 W, u32 H>
        void Decode(const u8* block, u8* texels)
        {
            constexpr u32 kTexels = W * H;
            // The void extent block: one constant color.
            if ((Bits(block, 0, 9) & 0x1FF) == 0x1FC)
            {
                if (Bit(block, 9))
                {
                    Error(texels, kTexels);
                    return;
                }
                u8 rgba[4];
                for (int c = 0; c < 4; c++)
                    rgba[c] = u8(Bits(block, 64 + 16 * c, 16) >> 8);
                for (u32 i = 0; i < kTexels; i++)
                    std::memcpy(texels + i * 4, rgba, 4);
                return;
            }
            BlockMode mode;
            if (!DecodeBlockMode(Bits(block, 0, 11), mode) || mode.width > W || mode.height > H)
            {
                Error(texels, kTexels);
                return;
            }
            u32 partitions = Bits(block, 11, 2) + 1;
            u32 weightCount = mode.width * mode.height * (mode.dualPlane ? 2 : 1);
            u32 weightBits = SequenceBits(mode.weightLevels, weightCount);
            if (weightCount > 64 || weightBits < 24 || weightBits > 96 || (partitions == 4 && mode.dualPlane))
            {
                Error(texels, kTexels);
                return;
            }
            // Color endpoint modes: one for all partitions, or a class with per partition offsets whose upper bits sit
            // below the weights.
            u32 cem[4];
            u32 partitionIndex = 0, colorStart = 17, belowWeights = 128 - weightBits;
            if (partitions == 1)
                cem[0] = Bits(block, 13, 4);
            else
            {
                partitionIndex = Bits(block, 13, 10);
                colorStart = 29;
                u32 field = Bits(block, 23, 6);
                if ((field & 3) == 0)
                    for (u32 p = 0; p < partitions; p++)
                        cem[p] = field >> 2;
                else
                {
                    u32 extra = 3 * partitions - 4;
                    belowWeights -= extra;
                    u32 value = (field >> 2) | (Bits(block, belowWeights, extra) << 4);
                    u32 base = (field & 3) - 1;
                    for (u32 p = 0; p < partitions; p++)
                    {
                        u32 c = (value >> p) & 1, m = (value >> (partitions + 2 * p)) & 3;
                        cem[p] = ((base + c) << 2) | m;
                    }
                }
            }
            u32 plane2Component = 0;
            if (mode.dualPlane)
            {
                belowWeights -= 2;
                plane2Component = Bits(block, belowWeights, 2);
            }
            u32 colorValues = 0;
            for (u32 p = 0; p < partitions; p++)
                colorValues += ((cem[p] >> 2) + 1) * 2;
            if (colorValues > 18 || belowWeights < colorStart)
            {
                Error(texels, kTexels);
                return;
            }
            i32 colorLevels = ColorLevels(colorValues, belowWeights - colorStart);
            if (colorLevels < 0)
            {
                Error(texels, kTexels);
                return;
            }
            u32 colors[18];
            DecodeSequence(block, colorStart, u32(colorLevels), colorValues, colors, false);
            i32 unquantized[18];
            for (u32 i = 0; i < colorValues; i++)
                unquantized[i] = i32(UnquantizeColor(colors[i], u32(colorLevels)));
            i32 e[4][2][4];
            u32 at = 0;
            for (u32 p = 0; p < partitions; p++)
            {
                if (!Endpoints(cem[p], unquantized + at, e[p][0], e[p][1]))
                {
                    Error(texels, kTexels);
                    return;
                }
                at += ((cem[p] >> 2) + 1) * 2;
            }
            u32 weights[64];
            DecodeSequence(block, 0, mode.weightLevels, weightCount, weights, true);
            for (u32 i = 0; i < weightCount; i++)
                weights[i] = UnquantizeWeight(weights[i], mode.weightLevels);
            u32 planes = mode.dualPlane ? 2 : 1;
            for (u32 y = 0; y < H; y++)
                for (u32 x = 0; x < W; x++)
                {
                    u32 p = partitions > 1 ? SelectPartition(partitionIndex, x, y, partitions, kTexels < 31) : 0;
                    u32 w[2];
                    for (u32 plane = 0; plane < planes; plane++)
                        w[plane] = InfillWeight(weights, mode.width, mode.height, W, H, x, y, plane, planes);
                    u8* t = texels + (y * W + x) * 4;
                    for (u32 c = 0; c < 4; c++)
                    {
                        u32 weight = mode.dualPlane && c == plane2Component ? w[1] : w[0];
                        u32 a = u32(e[p][0][c]) * 257, b = u32(e[p][1][c]) * 257;
                        t[c] = u8(((a * (64 - weight) + b * weight + 32) >> 6) >> 8);
                    }
                }
        }
    }

    void AddDecoders(BlockCodec* codecs, usize& count)
    {
        codecs[count++] = {PixelFormat::ASTC4x4, PixelFormat::RGBA8, &Decode<4, 4>, nullptr};
        codecs[count++] = {PixelFormat::ASTC5x4, PixelFormat::RGBA8, &Decode<5, 4>, nullptr};
        codecs[count++] = {PixelFormat::ASTC5x5, PixelFormat::RGBA8, &Decode<5, 5>, nullptr};
        codecs[count++] = {PixelFormat::ASTC6x5, PixelFormat::RGBA8, &Decode<6, 5>, nullptr};
        codecs[count++] = {PixelFormat::ASTC6x6, PixelFormat::RGBA8, &Decode<6, 6>, nullptr};
        codecs[count++] = {PixelFormat::ASTC8x5, PixelFormat::RGBA8, &Decode<8, 5>, nullptr};
        codecs[count++] = {PixelFormat::ASTC8x6, PixelFormat::RGBA8, &Decode<8, 6>, nullptr};
        codecs[count++] = {PixelFormat::ASTC8x8, PixelFormat::RGBA8, &Decode<8, 8>, nullptr};
        codecs[count++] = {PixelFormat::ASTC10x5, PixelFormat::RGBA8, &Decode<10, 5>, nullptr};
        codecs[count++] = {PixelFormat::ASTC10x6, PixelFormat::RGBA8, &Decode<10, 6>, nullptr};
        codecs[count++] = {PixelFormat::ASTC10x8, PixelFormat::RGBA8, &Decode<10, 8>, nullptr};
        codecs[count++] = {PixelFormat::ASTC10x10, PixelFormat::RGBA8, &Decode<10, 10>, nullptr};
        codecs[count++] = {PixelFormat::ASTC12x10, PixelFormat::RGBA8, &Decode<12, 10>, nullptr};
        codecs[count++] = {PixelFormat::ASTC12x12, PixelFormat::RGBA8, &Decode<12, 12>, nullptr};
    }
}
