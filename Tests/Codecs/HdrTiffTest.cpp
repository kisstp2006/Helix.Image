#include <Generated/TestImages.h>
#include <Support/HdrTiffWriters.h>

#include <Helix/Image/Codecs/ImageDecoder.h>
#include <Helix/Testing/Test.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::image;
using namespace helix::image::testing;

namespace
{
    bool Decode(const Array<u8>& file, TextureData& out)
    {
        String error;
        bool ok = DecodeImage(Span<const u8>(file.Data(), file.Size()), out, error);
        if (!ok)
            std::printf("    %s\n", error.CStr());
        return ok;
    }

    float Bits(u32 v)
    {
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }

    u32 Bits(float f)
    {
        u32 v;
        std::memcpy(&v, &f, 4);
        return v;
    }
}

HX_TEST(Hdr, ReadsFlatAndRunLengthScanlines)
{
    const u32 w = 21, h = 6;
    Array<float> rgb;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
        {
            // Runs of equal values for the run length form, and values from very dark to bright.
            rgb.Push(x < 8 ? 0.25f : float(x) * 0.7f);
            rgb.Push(std::ldexp(1.0f, int(y) * 3 - 8));
            rgb.Push(float(x * y) * 0.01f);
        }
    for (int rle = 0; rle < 2; rle++)
        for (int bottomUp = 0; bottomUp < 2; bottomUp++)
        {
            Array<u8> rgbe;
            Array<u8> file = WriteHdr(rgb, w, h, rle != 0, bottomUp != 0, rgbe);
            TextureData texture;
            HX_REQUIRE(Decode(file, texture));
            HX_REQUIRE(texture.format == PixelFormat::RGB32Float && texture.width == w && texture.height == h);
            HX_CHECK(texture.colorSpace == ColorSpace::Linear);
            const float* f = reinterpret_cast<const float*>(texture.data.Data());
            bool exact = true, close = true;
            for (usize i = 0; i < usize(w) * h; i++)
            {
                const u8* q = rgbe.Data() + i * 4;
                for (u32 c = 0; c < 3; c++)
                {
                    float expected = q[3] ? std::ldexp(float(q[c]) + 0.5f, int(q[3]) - 136) : 0.0f;
                    exact = exact && f[i * 3 + c] == expected;
                    float source = rgb[i * 3 + c], largest = Max(Max(rgb[i * 3], rgb[i * 3 + 1]), rgb[i * 3 + 2]);
                    close = close && std::fabs(f[i * 3 + c] - source) <= largest / 64.0f;
                }
            }
            HX_CHECK(exact);
            HX_CHECK(close);
        }
}

HX_TEST(Tiff, MatchesTheWindowsLzwFile)
{
    for (const TestImage& t : kTestImages)
    {
        if (std::strcmp(t.extension, "tif") != 0)
            continue;
        TextureData texture;
        String error;
        bool ok = DecodeImage(Span<const u8>(t.file, usize(t.fileSize)), texture, error);
        if (!ok)
            std::printf("    %s\n", error.CStr());
        HX_REQUIRE(ok);
        HX_CHECK(texture.format == PixelFormat::RGBA8 && std::memcmp(texture.data.Data(), t.rgba, usize(t.width) * t.height * 4) == 0);
    }
}

HX_TEST(Tiff, ReadsEveryLayoutCompressionAndSampleType)
{
    const u32 w = 37, h = 19;
    struct Case
    {
        const char* name;
        TiffOptions o;
    };
    Case cases[] = {
        {"rgb8 none", {}},
        {"rgb8 packbits big endian", {8, 3, 2, 32773, 1, true}},
        {"rgba8 deflate predictor", {8, 4, 2, 8, 2}},
        {"rgb8 tiled", {8, 3, 2, 8, 1, false, true}},
        {"rgba8 planar", {8, 4, 2, 32773, 1, false, false, true}},
        {"gray8", {8, 1, 1, 1}},
        {"white is zero gray8", {8, 1, 0, 8}},
        {"gray alpha 16 predictor", {16, 2, 1, 8, 2, true}},
        {"rgb16 tiled big endian", {16, 3, 2, 32773, 1, true, true}},
        {"rgba float", {32, 4, 2, 8, 1}},
        {"rgb float predictor big endian", {32, 3, 2, 8, 3, true}},
        {"gray1", {1, 1, 1, 32773}},
        {"gray4 strips of 1", {4, 1, 1, 1, 1, false, false, false, 1}},
    };
    for (const Case& k : cases)
    {
        const TiffOptions& o = k.o;
        Array<u32> values;
        u32 maximum = o.bits == 16 ? 0xFFFFu : (1u << (o.bits == 32 ? 8 : o.bits)) - 1;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                for (u32 s = 0; s < o.samples; s++)
                {
                    u32 v = (x * 37 + y * 11 + s * 101) % (maximum + 1);
                    values.Push(o.bits == 32 ? Bits(float(v) / 7.0f - 3.0f) : v);
                }
        TextureData texture;
        Array<u8> file = WriteTiff(values, w, h, o);
        bool ok = Decode(file, texture);
        if (!ok)
            std::printf("    case %s\n", k.name);
        HX_REQUIRE(ok);
        HX_CHECK(texture.width == w && texture.height == h);
        u32 color = o.photometric == 2 ? 3 : 1;
        bool same = true;
        for (usize p = 0; p < usize(w) * h; p++)
            for (u32 c = 0; c < 4; c++)
            {
                u32 source = c < 3 ? values[p * o.samples + (color == 3 ? c : 0)] : (o.samples > color ? values[p * o.samples + color] : ~0u);
                if (o.bits == 32)
                {
                    float got = reinterpret_cast<const float*>(texture.data.Data())[p * 4 + c];
                    same = same && got == (source == ~0u ? 1.0f : Bits(source));
                }
                else if (o.bits == 16)
                {
                    u16 got;
                    std::memcpy(&got, texture.data.Data() + (p * 4 + c) * 2, 2);
                    same = same && got == (source == ~0u ? 0xFFFFu : source);
                }
                else
                {
                    u32 v = source == ~0u ? maximum : (o.photometric == 0 && c < 3 ? maximum - source : source);
                    same = same && texture.data[p * 4 + c] == u8(v * 255u / maximum);
                }
            }
        if (!same)
            std::printf("    case %s decodes wrong samples\n", k.name);
        HX_CHECK(same);
    }
}

HX_TEST(Tiff, ReadsPalettes)
{
    const u32 w = 9, h = 4;
    Array<u16> map;
    for (u32 c = 0; c < 3; c++)
        for (u32 i = 0; i < 16; i++)
            map.Push(u16((i * 16 + c * 5) * 257));
    Array<u32> values;
    for (u32 i = 0; i < w * h; i++)
        values.Push(i % 16);
    TiffOptions o;
    o.bits = 4;
    o.samples = 1;
    o.photometric = 3;
    TextureData texture;
    HX_REQUIRE(Decode(WriteTiff(values, w, h, o, &map), texture));
    bool same = true;
    for (u32 i = 0; i < w * h; i++)
        for (u32 c = 0; c < 3; c++)
            same = same && texture.data[i * 4 + c] == u8(map[c * 16 + values[i]] >> 8);
    HX_CHECK(same);
}

// Compressions the decoder does not read are named, not reported as damaged data (found with the corpus files of
// Pillow's libtiff: CCITT group 4 and JPEG).
// Associated (premultiplied) alpha, as Blender writes RGBA TIFFs, comes back as straight alpha (the colors divided by
// alpha and rounded; fully transparent pixels keep their zero colors).
HX_TEST(Tiff, AssociatedAlphaBecomesStraight)
{
    for (u32 bits : {8u, 16u})
    {
        u32 maximum = bits == 8 ? 255 : 65535;
        Array<u32> values;
        // Straight colors and alphas, premultiplied for the file.
        const u32 straight[4][4] = {{maximum, maximum / 2, 0, maximum / 4}, {maximum / 3, maximum, maximum / 5, maximum}, {0, 0, 0, 0},
                                    {maximum, maximum, maximum, maximum / 2}};
        for (const auto& px : straight)
        {
            for (u32 c = 0; c < 3; c++)
                values.Push((px[c] * px[3] + maximum / 2) / maximum);
            values.Push(px[3]);
        }
        TiffOptions o;
        o.bits = bits;
        o.samples = 4;
        o.associatedAlpha = true;
        TextureData texture;
        HX_REQUIRE(Decode(WriteTiff(values, 2, 2, o), texture));
        bool close = true;
        for (u32 p = 0; p < 4; p++)
            for (u32 c = 0; c < 4; c++)
            {
                u32 got = bits == 8 ? texture.data[p * 4 + c] : u32(texture.data[(p * 4 + c) * 2]) | u32(texture.data[(p * 4 + c) * 2 + 1]) << 8;
                u32 want = straight[p][3] == 0 && c < 3 ? 0 : straight[p][c];
                // Premultiplying lost up to half a step of alpha's scale.
                u32 bound = c < 3 && straight[p][3] ? maximum / (2 * straight[p][3]) + 1 : 0;
                close = close && (got > want ? got - want : want - got) <= bound;
            }
        HX_CHECK(close);
    }
}

HX_TEST(Tiff, UnsupportedCompressionsAreNamed)
{
    Array<u32> values;
    for (u32 i = 0; i < 8 * 8 * 3; i++)
        values.Push(i & 255);
    TiffOptions o;
    o.compression = 1;
    const Array<u8> plain = WriteTiff(values, 8, 8, o);
    struct Case
    {
        u16 compression;
        const char* message;
    };
    for (Case c : {Case{4, "CCITT"}, Case{7, "JPEG"}, Case{99, "(99)"}})
    {
        Array<u8> file = plain;
        // The compression entry (tag 259) of the first directory: its SHORT value sits in the entry's last field.
        u32 ifd = u32(file[4]) | u32(file[5]) << 8 | u32(file[6]) << 16 | u32(file[7]) << 24;
        u32 entries = u32(file[ifd]) | u32(file[ifd + 1]) << 8;
        bool patched = false;
        for (u32 i = 0; i < entries; i++)
        {
            u8* e = file.Data() + ifd + 2 + i * 12;
            if ((u32(e[0]) | u32(e[1]) << 8) == 259)
                e[8] = u8(c.compression), e[9] = u8(c.compression >> 8), patched = true;
        }
        HX_REQUIRE(patched);
        TextureData texture;
        String error;
        HX_CHECK(!DecodeImage(Span<const u8>(file.Data(), file.Size()), texture, error));
        HX_CHECK(StringView(error).Contains(StringView(c.message)));
    }
}

HX_TEST(Tiff, DamagedFilesAreErrors)
{
    Array<u32> values;
    for (u32 i = 0; i < 8 * 8 * 3; i++)
        values.Push(i & 255);
    TiffOptions o;
    o.compression = 8;
    Array<u8> file = WriteTiff(values, 8, 8, o);
    for (usize cut : {usize(6), usize(20), file.Size() - 5})
    {
        Array<u8> part;
        part.Append(file.Data(), cut);
        TextureData texture;
        String error;
        HX_CHECK(!DecodeImage(Span<const u8>(part.Data(), part.Size()), texture, error));
        HX_CHECK(!error.Empty());
    }
}
