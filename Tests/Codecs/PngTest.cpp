#include <Support/TestVectors.h>

#include <Helix/Core/Hashing/Hash.h>
#include <Helix/Image/Codecs/Png.h>
#include <Helix/Jobs/Jobs.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::image;

HX_TEST(Png, DecodesEveryColorTypeDepthAndInterlace)
{
    for (const vectors::PngVector& v : vectors::kPngVectors)
    {
        Image image;
        bool ok = DecodePng(Span<const u8>(v.data, v.size), image);
        u32 crc = ok ? Crc32(image.pixels.Data(), image.pixels.Size()) : 0;
        if (!ok || crc != v.rgbaCrc)
            std::printf("    %s failed\n", v.name);
        HX_CHECK(ok);
        HX_CHECK_EQ(image.width, v.width);
        HX_CHECK_EQ(image.height, v.height);
        HX_CHECK_EQ(crc, v.rgbaCrc);
    }
}

HX_TEST(Png, EncodeDecodeRoundTrip)
{
    const ImageFormat formats[] = {ImageFormat::R8, ImageFormat::RG8, ImageFormat::RGB8, ImageFormat::RGBA8};
    for (ImageFormat format : formats)
    {
        Image src;
        src.Allocate(37, 23, format);
        for (usize i = 0; i < src.pixels.Size(); i++)
            src.pixels[i] = u8((i * 31) ^ (i / src.RowBytes() * 7));
        Array<u8> file;
        EncodePng(src, file);
        Image back;
        HX_REQUIRE(DecodePng(file, back));
        HX_REQUIRE(back.width == 37 && back.height == 23);
        u32 channels = ChannelCount(format);
        u32 mismatches = 0;
        for (u32 p = 0; p < 37 * 23; p++)
        {
            const u8* s = src.pixels.Data() + p * channels;
            const u8* d = back.pixels.Data() + p * 4;
            u8 expect[4] = {s[0], channels >= 3 ? s[1] : s[0], channels >= 3 ? s[2] : s[0],
                            channels == 4 ? s[3] : channels == 2 ? s[1] : u8(255)};
            mismatches += std::memcmp(expect, d, 4) ? 1 : 0;
        }
        HX_CHECK_EQ(mismatches, 0u);
    }
}

HX_TEST(Png, RejectsDamagedFiles)
{
    Image src;
    src.Allocate(8, 8, ImageFormat::RGBA8);
    Array<u8> file;
    EncodePng(src, file);
    Image out;
    HX_CHECK(DecodePng(file, out));
    Array<u8> bad = file;
    bad[20] ^= 1;
    HX_CHECK(!DecodePng(bad, out));
    HX_CHECK(!DecodePng(Span<const u8>(file.Data(), 30), out));
    HX_CHECK(!DecodePng(Span<const u8>(file.Data(), 4), out));
}

HX_TEST(Png, LargeImageEncodesTheSameOnWorkers)
{
    Image src;
    src.Allocate(700, 500, ImageFormat::RGBA8);
    for (usize i = 0; i < src.pixels.Size(); i++)
        src.pixels[i] = u8((i * 13) ^ (i / 2800 * 5) ^ (i >> 9));
    Array<u8> serial, parallel;
    EncodePng(src, serial);
    jobs::Initialize();
    EncodePng(src, parallel);
    jobs::Shutdown();
    HX_CHECK_EQ(serial.Size(), parallel.Size());
    HX_CHECK(std::memcmp(serial.Data(), parallel.Data(), serial.Size()) == 0);
    Image back;
    HX_REQUIRE(DecodePng(parallel, back));
    HX_CHECK(std::memcmp(back.pixels.Data(), src.pixels.Data(), src.pixels.Size()) == 0);
}
