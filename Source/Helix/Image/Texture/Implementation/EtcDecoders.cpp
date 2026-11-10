#include <Helix/Image/Texture/Convert.h>
#include <Helix/Image/Texture/Implementation/BuiltInBlockCodecs.h>

#include <cstring>

// ETC2 and EAC blocks to texels, from the compressed texture formats of OpenGL ES 3.0. Blocks are big endian and their
// texels are numbered by columns: texel x * 4 + y.
namespace helix::image::etc
{
    namespace
    {
        constexpr i32 kModifiers[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
        constexpr i32 kDistances[8] = {3, 6, 11, 16, 23, 32, 41, 64};

        u64 Big64(const u8* p)
        {
            u64 v = 0;
            for (int i = 0; i < 8; i++)
                v = (v << 8) | p[i];
            return v;
        }

        u32 Bits(u64 v, u32 high, u32 low) { return u32((v >> low) & ((u64(1) << (high - low + 1)) - 1)); }
        u8 Clamp(i32 v) { return u8(v < 0 ? 0 : v > 255 ? 255 : v); }
        u32 Extend4(u32 v) { return v * 17; }
        u32 Extend5(u32 v) { return (v << 3) | (v >> 2); }

        void Put(u8* rgba, u32 texel, i32 r, i32 g, i32 b, u8 a)
        {
            // Column order in the block, row order in the texels.
            u32 x = texel / 4, y = texel % 4;
            u8* t = rgba + (y * 4 + x) * 4;
            t[0] = Clamp(r);
            t[1] = Clamp(g);
            t[2] = Clamp(b);
            t[3] = a;
        }

        // The color part of ETC2 RGB and of ETC2 RGBA1 (`punchThrough`); alpha is 255 except for transparent texels.
        void DecodeColor(const u8* block, u8* rgba, bool punchThrough)
        {
            u64 v = Big64(block);
            bool flip = (v >> 32) & 1, bit33 = (v >> 33) & 1;
            bool differential = punchThrough || bit33;
            bool opaque = !punchThrough || bit33;
            auto index = [&](u32 i) { return (Bits(v, 16 + i, 16 + i) << 1) | Bits(v, i, i); };
            i32 base[2][3];
            if (differential)
            {
                i32 r = i32(Bits(v, 63, 59)), g = i32(Bits(v, 55, 51)), b = i32(Bits(v, 47, 43));
                i32 dr = i32(Bits(v, 58, 56)), dg = i32(Bits(v, 50, 48)), db = i32(Bits(v, 42, 40));
                dr = dr >= 4 ? dr - 8 : dr;
                dg = dg >= 4 ? dg - 8 : dg;
                db = db >= 4 ? db - 8 : db;
                if (r + dr < 0 || r + dr > 31)
                {
                    // T mode: two colors, the second one moved up and down by a distance.
                    i32 c1[3] = {i32(Extend4((Bits(v, 60, 59) << 2) | Bits(v, 57, 56))), i32(Extend4(Bits(v, 55, 52))), i32(Extend4(Bits(v, 51, 48)))};
                    i32 c2[3] = {i32(Extend4(Bits(v, 47, 44))), i32(Extend4(Bits(v, 43, 40))), i32(Extend4(Bits(v, 39, 36)))};
                    i32 d = kDistances[(Bits(v, 35, 34) << 1) | Bits(v, 32, 32)];
                    i32 paint[4][3];
                    for (int c = 0; c < 3; c++)
                    {
                        paint[0][c] = c1[c];
                        paint[1][c] = c2[c] + d;
                        paint[2][c] = c2[c];
                        paint[3][c] = c2[c] - d;
                    }
                    for (u32 i = 0; i < 16; i++)
                    {
                        u32 k = index(i);
                        if (!opaque && k == 2)
                            Put(rgba, i, 0, 0, 0, 0);
                        else
                            Put(rgba, i, paint[k][0], paint[k][1], paint[k][2], 255);
                    }
                    return;
                }
                if (g + dg < 0 || g + dg > 31)
                {
                    // H mode: two colors, each moved up and down by a distance.
                    u32 r1 = Bits(v, 62, 59), g1 = (Bits(v, 58, 56) << 1) | Bits(v, 52, 52), b1 = (Bits(v, 51, 51) << 3) | Bits(v, 49, 47);
                    u32 r2 = Bits(v, 46, 43), g2 = Bits(v, 42, 39), b2 = Bits(v, 38, 35);
                    u32 order = ((r1 << 8) | (g1 << 4) | b1) >= ((r2 << 8) | (g2 << 4) | b2) ? 1 : 0;
                    i32 d = kDistances[(Bits(v, 34, 34) << 2) | (Bits(v, 32, 32) << 1) | order];
                    i32 c1[3] = {i32(Extend4(r1)), i32(Extend4(g1)), i32(Extend4(b1))}, c2[3] = {i32(Extend4(r2)), i32(Extend4(g2)), i32(Extend4(b2))};
                    i32 paint[4][3];
                    for (int c = 0; c < 3; c++)
                    {
                        paint[0][c] = c1[c] + d;
                        paint[1][c] = c1[c] - d;
                        paint[2][c] = c2[c] + d;
                        paint[3][c] = c2[c] - d;
                    }
                    for (u32 i = 0; i < 16; i++)
                    {
                        u32 k = index(i);
                        if (!opaque && k == 2)
                            Put(rgba, i, 0, 0, 0, 0);
                        else
                            Put(rgba, i, paint[k][0], paint[k][1], paint[k][2], 255);
                    }
                    return;
                }
                if (b + db < 0 || b + db > 31)
                {
                    // Planar mode: a color at the origin and at the right and bottom edges, interpolated.
                    auto e6 = [](u32 x) { return i32((x << 2) | (x >> 4)); };
                    auto e7 = [](u32 x) { return i32((x << 1) | (x >> 6)); };
                    i32 o[3] = {e6(Bits(v, 62, 57)), e7((Bits(v, 56, 56) << 6) | Bits(v, 54, 49)),
                                e6((Bits(v, 48, 48) << 5) | (Bits(v, 44, 43) << 3) | Bits(v, 41, 39))};
                    i32 h[3] = {e6((Bits(v, 38, 34) << 1) | Bits(v, 32, 32)), e7(Bits(v, 31, 25)), e6(Bits(v, 24, 19))};
                    i32 w[3] = {e6(Bits(v, 18, 13)), e7(Bits(v, 12, 6)), e6(Bits(v, 5, 0))};
                    for (u32 y = 0; y < 4; y++)
                        for (u32 x = 0; x < 4; x++)
                        {
                            i32 c[3];
                            for (int k = 0; k < 3; k++)
                                c[k] = (i32(x) * (h[k] - o[k]) + i32(y) * (w[k] - o[k]) + 4 * o[k] + 2) >> 2;
                            Put(rgba, x * 4 + y, c[0], c[1], c[2], 255);
                        }
                    return;
                }
                base[0][0] = i32(Extend5(u32(r)));
                base[0][1] = i32(Extend5(u32(g)));
                base[0][2] = i32(Extend5(u32(b)));
                base[1][0] = i32(Extend5(u32(r + dr)));
                base[1][1] = i32(Extend5(u32(g + dg)));
                base[1][2] = i32(Extend5(u32(b + db)));
            }
            else
            {
                base[0][0] = i32(Extend4(Bits(v, 63, 60)));
                base[1][0] = i32(Extend4(Bits(v, 59, 56)));
                base[0][1] = i32(Extend4(Bits(v, 55, 52)));
                base[1][1] = i32(Extend4(Bits(v, 51, 48)));
                base[0][2] = i32(Extend4(Bits(v, 47, 44)));
                base[1][2] = i32(Extend4(Bits(v, 43, 40)));
            }
            u32 tables[2] = {Bits(v, 39, 37), Bits(v, 36, 34)};
            for (u32 i = 0; i < 16; i++)
            {
                u32 x = i / 4, y = i % 4;
                u32 sub = flip ? (y >= 2 ? 1 : 0) : (x >= 2 ? 1 : 0);
                u32 k = index(i);
                if (!opaque && k == 2)
                {
                    Put(rgba, i, 0, 0, 0, 0);
                    continue;
                }
                const i32* m = kModifiers[tables[sub]];
                i32 modifier = k == 0 ? (opaque ? m[0] : 0) : k == 1 ? m[1] : k == 2 ? -m[0] : -m[1];
                Put(rgba, i, base[sub][0] + modifier, base[sub][1] + modifier, base[sub][2] + modifier, 255);
            }
        }

        constexpr i32 kAlphaModifiers[16][8] = {
            {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12}, {-2, -4, -6, -13, 1, 3, 5, 12},
            {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},  {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},
            {-2, -6, -8, -10, 1, 5, 7, 9},  {-2, -5, -8, -10, 1, 4, 7, 9},   {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
            {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},   {-3, -5, -7, -9, 2, 4, 6, 8},
        };

        // An EAC block: 8 bit alpha, or 11 bit values (unsigned or signed) for R11 / RG11, in row order.
        enum class Eac
        {
            Alpha,
            Unsigned,
            Signed,
        };

        void DecodeEac(const u8* block, i32* out, Eac kind)
        {
            u64 v = Big64(block);
            i32 base = kind == Eac::Signed ? i32(i8(block[0])) : i32(block[0]);
            base = base < -127 ? -127 : base;
            i32 multiplier = i32(Bits(v, 55, 52));
            const i32* table = kAlphaModifiers[Bits(v, 51, 48)];
            for (u32 i = 0; i < 16; i++)
            {
                i32 m = table[Bits(v, 47 - 3 * i, 45 - 3 * i)];
                u32 x = i / 4, y = i % 4;
                i32 value;
                if (kind == Eac::Alpha)
                    value = Clamp(base + m * multiplier);
                else if (kind == Eac::Unsigned)
                {
                    value = multiplier ? base * 8 + 4 + m * multiplier * 8 : base * 8 + 4 + m;
                    value = value < 0 ? 0 : value > 2047 ? 2047 : value;
                }
                else
                {
                    value = multiplier ? base * 8 + m * multiplier * 8 : base * 8 + m;
                    value = value < -1023 ? -1023 : value > 1023 ? 1023 : value;
                }
                out[y * 4 + x] = value;
            }
        }

        void DecodeRgb(const u8* block, u8* texels) { DecodeColor(block, texels, false); }
        void DecodeRgba1(const u8* block, u8* texels) { DecodeColor(block, texels, true); }

        void DecodeRgba(const u8* block, u8* texels)
        {
            DecodeColor(block + 8, texels, false);
            i32 alpha[16];
            DecodeEac(block, alpha, Eac::Alpha);
            for (u32 i = 0; i < 16; i++)
                texels[i * 4 + 3] = u8(alpha[i]);
        }

        // R11 and RG11 give 16 bit unorm values, the signed forms halves from -1 to 1.
        template <u32 Channels, bool Signed>
        void DecodeR11(const u8* block, u8* texels)
        {
            for (u32 c = 0; c < Channels; c++)
            {
                i32 values[16];
                DecodeEac(block + 8 * c, values, Signed ? Eac::Signed : Eac::Unsigned);
                for (u32 i = 0; i < 16; i++)
                {
                    u16 out = Signed ? FloatToHalf(float(values[i]) / 1023.0f) : u16((u32(values[i]) << 5) | (u32(values[i]) >> 6));
                    std::memcpy(texels + (i * Channels + c) * 2, &out, 2);
                }
            }
        }
    }

    void AddDecoders(BlockCodec* codecs, usize& count)
    {
        codecs[count++] = {PixelFormat::ETC2RGB, PixelFormat::RGBA8, &DecodeRgb, nullptr};
        codecs[count++] = {PixelFormat::ETC2RGBA1, PixelFormat::RGBA8, &DecodeRgba1, nullptr};
        codecs[count++] = {PixelFormat::ETC2RGBA, PixelFormat::RGBA8, &DecodeRgba, nullptr};
        codecs[count++] = {PixelFormat::EACR11, PixelFormat::R16, &DecodeR11<1, false>, nullptr};
        codecs[count++] = {PixelFormat::EACR11Snorm, PixelFormat::R16Float, &DecodeR11<1, true>, nullptr};
        codecs[count++] = {PixelFormat::EACRG11, PixelFormat::RG16, &DecodeR11<2, false>, nullptr};
        codecs[count++] = {PixelFormat::EACRG11Snorm, PixelFormat::RG16Float, &DecodeR11<2, true>, nullptr};
    }
}
