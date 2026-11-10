#include <Helix/Image/Texture/BlockCodec.h>
#include <Helix/Image/Texture/Convert.h>
#include <Helix/Image/Texture/Implementation/BptcTables.h>
#include <Helix/Testing/Test.h>

#include <cstring>

using namespace helix;
using namespace helix::image;

namespace
{
    TextureData Blocks(PixelFormat format, u32 w, u32 h, const u8* block, u32 mips = 1)
    {
        TextureData t;
        t.Allocate(format, w, h, 1, 1, 1, mips);
        u32 size = FormatInfo(format).bytesPerBlock;
        for (usize i = 0; i < t.data.Size(); i += size)
            std::memcpy(t.data.Data() + i, block, size);
        return t;
    }

    // Writes values into a 128 bit block from its lowest bit on.
    struct BitWriter
    {
        u8 block[16] = {};
        u32 at = 0;
        void Put(u32 value, u32 bits)
        {
            for (u32 i = 0; i < bits; i++, at++)
                block[at >> 3] |= u8(((value >> i) & 1) << (at & 7));
        }
    };
}

HX_TEST(BlockDecoder, AnchorsBelongToTheirSubsets)
{
    using namespace bptc;
    bool ok = true;
    for (u32 p = 0; p < 64; p++)
    {
        ok = ok && Subset(2, p, 0) == 0 && Subset(2, p, kAnchors2[p]) == 1;
        ok = ok && Subset(3, p, 0) == 0 && Subset(3, p, kAnchors3[0][p]) == 1 && Subset(3, p, kAnchors3[1][p]) == 2;
    }
    HX_CHECK(ok);
}

HX_TEST(BlockDecoder, Bc1ColorsAndTransparency)
{
    // Red and blue endpoints; texel 0 index 0, texel 1 index 1, texel 2 index 2, texel 3 index 3.
    const u8 four[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0, 0, 0};
    TextureData out;
    String error;
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::BC1, 4, 4, four), out, error));
    const u8* t = out.data.Data();
    HX_CHECK(t[0] == 255 && t[2] == 0 && t[3] == 255);
    HX_CHECK(t[4] == 0 && t[6] == 255);
    HX_CHECK(t[8] == 170 && t[10] == 85 && t[12] == 85 && t[14] == 170);
    // c0 <= c1: the third color is the middle, the fourth transparent black.
    const u8 three[8] = {0x1F, 0x00, 0x00, 0xF8, 0xE4, 0, 0, 0};
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::BC1, 4, 4, three), out, error));
    t = out.data.Data();
    HX_CHECK(t[8] == 128 && t[10] == 128 && t[11] == 255);
    HX_CHECK(t[12] == 0 && t[13] == 0 && t[14] == 0 && t[15] == 0);
}

HX_TEST(BlockDecoder, Bc4BothModesAndSigned)
{
    // 8 value mode (200 > 100): index 1 is the second endpoint, index 2 one seventh of the way.
    const u8 eight[8] = {200, 100, 0x08 | (2 << 6), 0, 0, 0, 0, 0};
    TextureData out;
    String error;
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::BC4, 4, 4, eight), out, error));
    HX_CHECK(out.data[0] == 200 && out.data[1] == 100 && out.data[2] == 186);
    // 6 value mode: indices 6 and 7 are 0 and 255.
    const u8 six[8] = {50, 100, 6 | (7 << 3), 0, 0, 0, 0, 0};
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::BC4, 4, 4, six), out, error));
    HX_CHECK(out.data[0] == 0 && out.data[1] == 255 && out.data[2] == 50);
    const u8 negative[8] = {u8(i8(-128)), 100, 0, 0, 0, 0, 0, 0};
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::BC4Snorm, 4, 4, negative), out, error));
    HX_CHECK(i8(out.data[0]) == -127);
}

HX_TEST(BlockDecoder, Bc7Mode6AndBc6hMode11)
{
    // Mode 6: one subset, 7 bit endpoints with a p bit each, 4 bit indices; all indices 0 give the first endpoint.
    BitWriter w;
    w.Put(1u << 6, 7);
    const u32 e[4][2] = {{100, 20}, {50, 60}, {7, 120}, {127, 127}};
    for (const auto& c : e)
    {
        w.Put(c[0], 7);
        w.Put(c[1], 7);
    }
    w.Put(1, 1);
    w.Put(0, 1);
    TextureData out;
    String error;
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::BC7, 4, 4, w.block), out, error));
    // (value << 1 | p) as 8 bits.
    HX_CHECK(out.data[0] == 201 && out.data[1] == 101 && out.data[2] == 15 && out.data[3] == 255);

    // BC6H mode 11 (code 00011): 10 bit endpoints; index 0 everywhere gives the first endpoint.
    BitWriter h;
    h.Put(3, 5);
    for (u32 v : {512u, 0u, 1023u, 100u, 100u, 100u})
        h.Put(v, 10);
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::BC6H, 4, 4, h.block), out, error));
    u16 half[4];
    std::memcpy(half, out.data.Data(), 8);
    u32 unquantized = ((512u << 16) + 0x8000) >> 10;
    HX_CHECK_EQ(half[0], u16((unquantized * 31) >> 6));
    HX_CHECK_EQ(half[1], u16(0));
    HX_CHECK_EQ(half[2], u16((0xFFFFu * 31) >> 6));
    HX_CHECK_EQ(half[3], u16(0x3C00));
}

HX_TEST(BlockDecoder, EtcIndividualModeAndEac)
{
    // Individual mode, no flip: left half base (8, 4, 2) * 17, right half (1, 2, 3) * 17, tables 0 and 7.
    const u8 etc[8] = {0x81, 0x42, 0x23, 0x1C, 0x00, 0x00, 0x00, 0x00};
    TextureData out;
    String error;
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::ETC2RGB, 4, 4, etc), out, error));
    const u8* t = out.data.Data();
    // Texel (0, 0): base + 2 (table 0, index 0); texel (3, 0): base + 47 (table 7, index 0).
    HX_CHECK(t[0] == 138 && t[1] == 70 && t[2] == 36 && t[3] == 255);
    HX_CHECK(t[12] == 64 && t[13] == 81 && t[14] == 98);
    // EAC: base 128, multiplier 2, table 0, all indices 7 (+14 times the multiplier): 156.
    u8 eac[16] = {128, 0x20, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::memcpy(eac + 8, etc, 8);
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::ETC2RGBA, 4, 4, eac), out, error));
    HX_CHECK(out.data[3] == 156 && out.data[63] == 156);
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::EACR11, 4, 4, eac), out, error));
    u16 r;
    std::memcpy(&r, out.data.Data(), 2);
    u32 v11 = 128 * 8 + 4 + 14 * 2 * 8;
    HX_CHECK_EQ(r, u16((v11 << 5) | (v11 >> 6)));
}

HX_TEST(BlockDecoder, EdgeSizesAndMips)
{
    const u8 block[8] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    TextureData out;
    String error;
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::BC1, 13, 7, block, 4), out, error));
    HX_CHECK(out.format == PixelFormat::RGBA8 && out.mips == 4u && out.At(0, 0, 3).width == 1u);
    bool red = true;
    for (usize i = 0; i < out.data.Size(); i += 4)
        red = red && out.data[i] == 255 && out.data[i + 1] == 0;
    HX_CHECK(red);
    TextureData rgba;
    rgba.Allocate(PixelFormat::RGBA8, 4, 4);
    HX_CHECK(!DecompressTexture(rgba, out, error));
}

HX_TEST(BlockDecoder, AstcVoidExtentAndAnyBlock)
{
    // The void extent block: block mode 0x1FC, then one RGBA color in 16 bit values.
    u8 block[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x30, 0x00, 0x80, 0x00, 0xC0, 0x00, 0xFF};
    TextureData out;
    String error;
    HX_REQUIRE(DecompressTexture(Blocks(PixelFormat::ASTC6x5, 12, 10, block), out, error));
    bool constant = true;
    for (usize i = 0; i < out.data.Size(); i += 4)
        constant = constant && out.data[i] == 0x30 && out.data[i + 1] == 0x80 && out.data[i + 2] == 0xC0 && out.data[i + 3] == 0xFF;
    HX_CHECK(constant);

    // Every block, valid or not, decodes to something (invalid ones to magenta) without reading outside the block.
    u64 state = 4242;
    for (u32 f = u32(PixelFormat::ASTC4x4); f <= u32(PixelFormat::ASTC12x12); f++)
        for (int round = 0; round < 300; round++)
        {
            u8 random[16];
            for (u8& b : random)
            {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                b = u8(state >> 24);
            }
            HX_REQUIRE(DecompressTexture(Blocks(PixelFormat(f), 12, 12, random), out, error));
        }
}
