#include <Helix/Image/Texture/Implementation/BcCommon.h>
#include <Helix/Image/Texture/Implementation/BuiltInBlockCodecs.h>

#include <cstring>

// BC1 to BC5 blocks to texels, from the block compression formats of Direct3D.
namespace helix::image::bc
{
    void DecodeColor(const u8* block, u8* rgba, bool allowTransparent)
    {
        u32 c0 = block[0] | (block[1] << 8), c1 = block[2] | (block[3] << 8);
        u8 palette[4][4];
        Expand565(c0, palette[0]);
        Expand565(c1, palette[1]);
        palette[0][3] = palette[1][3] = 255;
        if (c0 > c1 || !allowTransparent)
            for (int c = 0; c < 3; c++)
            {
                palette[2][c] = u8((2 * palette[0][c] + palette[1][c] + 1) / 3);
                palette[3][c] = u8((palette[0][c] + 2 * palette[1][c] + 1) / 3);
            }
        else
            for (int c = 0; c < 3; c++)
            {
                palette[2][c] = u8((palette[0][c] + palette[1][c] + 1) / 2);
                palette[3][c] = 0;
            }
        palette[2][3] = 255;
        palette[3][3] = c0 > c1 || !allowTransparent ? 255 : 0;
        u32 indices = block[4] | (block[5] << 8) | (block[6] << 16) | (u32(block[7]) << 24);
        for (int i = 0; i < 16; i++)
            std::memcpy(rgba + i * 4, palette[(indices >> (2 * i)) & 3], 4);
    }

    // One channel: two endpoints and 3 bit indices, 8 values or 6 values with the two extremes.
    void DecodeChannel(const u8* block, u8* out, u32 stride, bool isSigned)
    {
        i32 v[8];
        i32 low = isSigned ? -127 : 0, high = isSigned ? 127 : 255;
        i32 a = isSigned ? i8(block[0]) : block[0], b = isSigned ? i8(block[1]) : block[1];
        if (isSigned)
        {
            a = a < -127 ? -127 : a;
            b = b < -127 ? -127 : b;
        }
        v[0] = a;
        v[1] = b;
        if (a > b)
            for (int i = 1; i < 7; i++)
                v[i + 1] = Divide((7 - i) * a + i * b, 7);
        else
        {
            for (int i = 1; i < 5; i++)
                v[i + 1] = Divide((5 - i) * a + i * b, 5);
            v[6] = low;
            v[7] = high;
        }
        u64 indices = 0;
        for (int i = 0; i < 6; i++)
            indices |= u64(block[2 + i]) << (8 * i);
        for (int i = 0; i < 16; i++)
            out[i * stride] = u8(v[(indices >> (3 * i)) & 7]);
    }

    namespace
    {
        void DecodeBc1(const u8* block, u8* texels) { DecodeColor(block, texels, true); }

        void DecodeBc2(const u8* block, u8* texels)
        {
            DecodeColor(block + 8, texels, false);
            for (int i = 0; i < 16; i++)
            {
                u32 a = (block[i / 2] >> (4 * (i & 1))) & 15;
                texels[i * 4 + 3] = u8(a * 17);
            }
        }

        void DecodeBc3(const u8* block, u8* texels)
        {
            DecodeColor(block + 8, texels, false);
            DecodeChannel(block, texels + 3, 4, false);
        }

        void DecodeBc4(const u8* block, u8* texels) { DecodeChannel(block, texels, 1, false); }
        void DecodeBc4Signed(const u8* block, u8* texels) { DecodeChannel(block, texels, 1, true); }

        void DecodeBc5(const u8* block, u8* texels)
        {
            DecodeChannel(block, texels, 2, false);
            DecodeChannel(block + 8, texels + 1, 2, false);
        }

        void DecodeBc5Signed(const u8* block, u8* texels)
        {
            DecodeChannel(block, texels, 2, true);
            DecodeChannel(block + 8, texels + 1, 2, true);
        }
    }

    void AddDecoders(BlockCodec* codecs, usize& count)
    {
        codecs[count++] = {PixelFormat::BC1, PixelFormat::RGBA8, &DecodeBc1, nullptr};
        codecs[count++] = {PixelFormat::BC2, PixelFormat::RGBA8, &DecodeBc2, nullptr};
        codecs[count++] = {PixelFormat::BC3, PixelFormat::RGBA8, &DecodeBc3, nullptr};
        codecs[count++] = {PixelFormat::BC4, PixelFormat::R8, &DecodeBc4, nullptr};
        codecs[count++] = {PixelFormat::BC4Snorm, PixelFormat::R8Snorm, &DecodeBc4Signed, nullptr};
        codecs[count++] = {PixelFormat::BC5, PixelFormat::RG8, &DecodeBc5, nullptr};
        codecs[count++] = {PixelFormat::BC5Snorm, PixelFormat::RG8Snorm, &DecodeBc5Signed, nullptr};
    }
}
