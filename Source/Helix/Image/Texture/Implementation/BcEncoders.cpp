#include <Helix/Image/Texture/Implementation/BcCommon.h>
#include <Helix/Image/Texture/Implementation/BuiltInBlockCodecs.h>

#include <cstring>

// BC1 to BC5 encoders: the BC1 color block from the public domain stb_dxt (ThirdParty/stb_dxt, see THIRD-PARTY.txt), the
// alpha of BC2 and the BC4 channel blocks (also BC3 alpha and BC5) our own.
#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#pragma warning(disable : 4244 4245 4456 4457 4701 4702 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#endif
#include <stb_dxt/stb_dxt.h>
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace helix::image::bc
{
    namespace
    {
        // The 8 values of a BC4 block for its two endpoints.
        void Palette(i32 a, i32 b, bool isSigned, i32* v)
        {
            v[0] = a;
            v[1] = b;
            if (a > b)
                for (int i = 1; i < 7; i++)
                    v[i + 1] = Divide((7 - i) * a + i * b, 7);
            else
            {
                for (int i = 1; i < 5; i++)
                    v[i + 1] = Divide((5 - i) * a + i * b, 5);
                v[6] = isSigned ? -127 : 0;
                v[7] = isSigned ? 127 : 255;
            }
        }

        // The squared error of the best index of every value, and the indices.
        u64 Indices(const i32* values, i32 a, i32 b, bool isSigned, u32* indices)
        {
            i32 v[8];
            Palette(a, b, isSigned, v);
            u64 total = 0;
            for (int i = 0; i < 16; i++)
            {
                u32 best = 0;
                i32 bestError = 1 << 30;
                for (u32 k = 0; k < 8; k++)
                {
                    i32 d = values[i] - v[k], e = d * d;
                    if (e < bestError)
                    {
                        bestError = e;
                        best = k;
                    }
                }
                indices[i] = best;
                total += u64(bestError);
            }
            return total;
        }

        // One channel block: both modes (8 values; 6 values with the extremes) from the value range, then a search of
        // the endpoints around it, wider at higher quality.
        void EncodeChannel(const u8* texels, u32 stride, bool isSigned, CompressionQuality quality, u8* block)
        {
            i32 values[16];
            for (int i = 0; i < 16; i++)
                values[i] = isSigned ? i8(texels[i * stride]) : texels[i * stride];
            i32 low = isSigned ? -127 : 0, high = isSigned ? 127 : 255;
            i32 lo = high, hi = low, innerLo = high, innerHi = low;
            for (i32 v : values)
            {
                v = v < low ? low : v;
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
                if (v != low && v != high)
                {
                    innerLo = v < innerLo ? v : innerLo;
                    innerHi = v > innerHi ? v : innerHi;
                }
            }
            if (innerLo > innerHi)
                innerLo = innerHi = lo;
            i32 radius = quality == CompressionQuality::Fast ? 0 : quality == CompressionQuality::Normal ? 2 : 6;
            u32 bestIndices[16], indices[16];
            i32 bestA = hi, bestB = lo;
            u64 best = ~0ull;
            auto tryPair = [&](i32 a, i32 b) {
                a = a < low ? low : a > high ? high : a;
                b = b < low ? low : b > high ? high : b;
                u64 error = Indices(values, a, b, isSigned, indices);
                if (error < best)
                {
                    best = error;
                    bestA = a;
                    bestB = b;
                    std::memcpy(bestIndices, indices, sizeof(indices));
                }
            };
            for (i32 da = -radius; da <= radius; da++)
                for (i32 db = -radius; db <= radius; db++)
                {
                    // 8 values: the first endpoint the larger one.
                    if (hi + da > lo + db || hi == lo)
                        tryPair(hi + da, lo + db);
                    // 6 values and the extremes: the first endpoint the smaller one.
                    if (innerLo + da <= innerHi + db)
                        tryPair(innerLo + da, innerHi + db);
                }
            block[0] = u8(bestA);
            block[1] = u8(bestB);
            u64 bits = 0;
            for (int i = 0; i < 16; i++)
                bits |= u64(bestIndices[i]) << (3 * i);
            for (int i = 0; i < 6; i++)
                block[2 + i] = u8(bits >> (8 * i));
        }

        void EncodeColor(const u8* rgba, CompressionQuality quality, u8* block)
        {
            stb_compress_dxt_block(block, rgba, 0, quality == CompressionQuality::Fast ? STB_DXT_NORMAL : STB_DXT_HIGHQUAL);
        }

        void EncodeBc1(const u8* texels, u8* block, CompressionQuality quality) { EncodeColor(texels, quality, block); }

        void EncodeBc2(const u8* texels, u8* block, CompressionQuality quality)
        {
            std::memset(block, 0, 8);
            for (int i = 0; i < 16; i++)
            {
                u32 a = (u32(texels[i * 4 + 3]) * 15 + 127) / 255;
                block[i / 2] |= u8(a << (4 * (i & 1)));
            }
            EncodeColor(texels, quality, block + 8);
        }

        void EncodeBc3(const u8* texels, u8* block, CompressionQuality quality)
        {
            EncodeChannel(texels + 3, 4, false, quality, block);
            EncodeColor(texels, quality, block + 8);
        }

        void EncodeBc4(const u8* texels, u8* block, CompressionQuality quality) { EncodeChannel(texels, 1, false, quality, block); }
        void EncodeBc4Signed(const u8* texels, u8* block, CompressionQuality quality) { EncodeChannel(texels, 1, true, quality, block); }

        void EncodeBc5(const u8* texels, u8* block, CompressionQuality quality)
        {
            EncodeChannel(texels, 2, false, quality, block);
            EncodeChannel(texels + 1, 2, false, quality, block + 8);
        }

        void EncodeBc5Signed(const u8* texels, u8* block, CompressionQuality quality)
        {
            EncodeChannel(texels, 2, true, quality, block);
            EncodeChannel(texels + 1, 2, true, quality, block + 8);
        }
    }

    void AddEncoders(BlockCodec* codecs, usize count)
    {
        SetEncoder(codecs, count, PixelFormat::BC1, &EncodeBc1);
        SetEncoder(codecs, count, PixelFormat::BC2, &EncodeBc2);
        SetEncoder(codecs, count, PixelFormat::BC3, &EncodeBc3);
        SetEncoder(codecs, count, PixelFormat::BC4, &EncodeBc4);
        SetEncoder(codecs, count, PixelFormat::BC4Snorm, &EncodeBc4Signed);
        SetEncoder(codecs, count, PixelFormat::BC5, &EncodeBc5);
        SetEncoder(codecs, count, PixelFormat::BC5Snorm, &EncodeBc5Signed);
    }
}
