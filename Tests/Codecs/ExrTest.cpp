#include <Support/ExrWriter.h>

#include <Helix/Image/Codecs/ImageDecoder.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::image;
using namespace helix::image::testing;

namespace
{
    u16 FloatToHalf(float f)
    {
        u32 bits;
        std::memcpy(&bits, &f, 4);
        u32 sign = (bits >> 16) & 0x8000;
        i32 exponent = i32((bits >> 23) & 255) - 112;
        u32 mantissa = (bits >> 13) & 1023;
        if (exponent <= 0)
            return u16(sign);
        if (exponent >= 31)
            return u16(sign | 0x7C00);
        return u16(sign | (u32(exponent) << 10) | mantissa);
    }

    float HalfToFloat(u16 h)
    {
        u32 e = (h >> 10) & 31, bits = (u32(h & 0x8000) << 16) | (e ? ((e + 112) << 23) | (u32(h & 1023) << 13) : 0u);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    u32 FloatBits(float f)
    {
        u32 v;
        std::memcpy(&v, &f, 4);
        return v;
    }
}

HX_TEST(Exr, ReadsEveryCompressionLayoutAndChannelType)
{
    const u32 w = 41, h = 37;
    struct Case
    {
        const char* name;
        ExrOptions o;
        bool halves;
    };
    const Case cases[] = {
        {"none", {0}, true},     {"rle", {1}, true},  {"zips", {2}, false},       {"zip", {3}, true},
        {"zip float", {3}, false}, {"tiled rle", {1, true}, false}, {"tiled zip", {3, true, 16, -5, 7}, true},
    };
    for (const Case& k : cases)
    {
        // Channels sorted by name, as the format wants; the uint channel is extra and ignored.
        Array<ExrChannel> channels;
        u32 type = k.halves ? 1 : 2;
        channels.Push({"A", type});
        channels.Push({"B", type});
        channels.Push({"G", type});
        channels.Push({"R", type});
        channels.Push({"Z", 0});
        Array<u32> samples;
        Array<float> expected;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
            {
                // Smooth areas give runs, so every compression has work to do.
                float r = float(x / 4) * 0.25f, g = float(y) * 0.125f - 2.0f, b = x == y ? 1000.0f : 0.5f, a = float(x + y) / 80.0f;
                float rgba[4] = {r, g, b, a};
                float order[4] = {a, b, g, r};
                for (float f : order)
                    samples.Push(k.halves ? FloatToHalf(f) : FloatBits(f));
                samples.Push(x * 1000 + y);
                for (float f : rgba)
                    expected.Push(k.halves ? HalfToFloat(FloatToHalf(f)) : f);
            }
        Array<u8> file = WriteExr(channels, samples, w, h, k.o);
        TextureData texture;
        String error;
        bool ok = DecodeImage(Span<const u8>(file.Data(), file.Size()), texture, error);
        if (!ok)
            std::printf("    %s: %s\n", k.name, error.CStr());
        HX_REQUIRE(ok);
        HX_CHECK(texture.width == w && texture.height == h && texture.colorSpace == ColorSpace::Linear);
        HX_CHECK(texture.format == (k.halves ? PixelFormat::RGBA16Float : PixelFormat::RGBA32Float));
        bool same = true;
        for (usize i = 0; i < expected.Size(); i++)
        {
            float got;
            if (k.halves)
            {
                u16 v;
                std::memcpy(&v, texture.data.Data() + i * 2, 2);
                got = HalfToFloat(v);
            }
            else
                std::memcpy(&got, texture.data.Data() + i * 4, 4);
            same = same && got == expected[i];
        }
        if (!same)
            std::printf("    %s decodes wrong samples\n", k.name);
        HX_CHECK(same);
    }
}

HX_TEST(Exr, ReadsLuminanceAndLayerNames)
{
    Array<ExrChannel> channels;
    channels.Push({"Y", 2});
    Array<u32> samples;
    for (u32 i = 0; i < 6 * 2; i++)
        samples.Push(FloatBits(float(i)));
    Array<u8> file = WriteExr(channels, samples, 6, 2, {});
    TextureData texture;
    String error;
    HX_REQUIRE(DecodeImage(Span<const u8>(file.Data(), file.Size()), texture, error));
    const float* f = reinterpret_cast<const float*>(texture.data.Data());
    HX_CHECK(f[4 * 5] == 5.0f && f[4 * 5 + 1] == 5.0f && f[4 * 5 + 2] == 5.0f && f[4 * 5 + 3] == 1.0f);

    channels.Clear();
    channels.Push({"diffuse.B", 1});
    channels.Push({"diffuse.G", 1});
    channels.Push({"diffuse.R", 1});
    samples.Clear();
    for (u32 i = 0; i < 4; i++)
    {
        samples.Push(FloatToHalf(0.25f));
        samples.Push(FloatToHalf(0.5f));
        samples.Push(FloatToHalf(2.0f));
    }
    file = WriteExr(channels, samples, 2, 2, {});
    HX_REQUIRE(DecodeImage(Span<const u8>(file.Data(), file.Size()), texture, error));
    u16 rgba[4];
    std::memcpy(rgba, texture.data.Data(), 8);
    HX_CHECK(HalfToFloat(rgba[0]) == 2.0f && HalfToFloat(rgba[1]) == 0.5f && HalfToFloat(rgba[2]) == 0.25f && HalfToFloat(rgba[3]) == 1.0f);
}

HX_TEST(Exr, DamagedFilesAreErrors)
{
    Array<ExrChannel> channels;
    channels.Push({"R", 1});
    Array<u32> samples;
    for (u32 i = 0; i < 20 * 20; i++)
        samples.Push(FloatToHalf(float(i % 7)));
    ExrOptions o;
    o.compression = 3;
    Array<u8> file = WriteExr(channels, samples, 20, 20, o);
    for (usize cut : {usize(10), usize(60), file.Size() - 3})
    {
        Array<u8> part;
        part.Append(file.Data(), cut);
        TextureData texture;
        String error;
        HX_CHECK(!DecodeImage(Span<const u8>(part.Data(), part.Size()), texture, error) && !error.Empty());
    }
}
