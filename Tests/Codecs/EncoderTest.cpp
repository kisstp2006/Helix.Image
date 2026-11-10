#include <Helix/Image/Codecs/ImageDecoder.h>
#include <Helix/Image/Codecs/ImageEncoder.h>
#include <Helix/Image/Texture/Convert.h>
#include <Helix/Testing/Test.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::image;

namespace
{
    u32 Random(u64& state)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return u32(state >> 16);
    }

    TextureData Texture(PixelFormat format, u32 w, u32 h)
    {
        TextureData t;
        t.Allocate(format, w, h);
        return t;
    }

    // A smooth RGBA float image with an alpha ramp, for the lossy encoder and the format conversions.
    Array<float> Smooth(u32 w, u32 h)
    {
        Array<float> rgba;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
            {
                rgba.Push(0.5f + 0.4f * std::sin(float(x) * 0.15f));
                rgba.Push(float(y) / float(h));
                rgba.Push(0.25f + 0.2f * std::cos(float(x + y) * 0.1f));
                rgba.Push(float(x) / float(w));
            }
        return rgba;
    }

    bool RoundTrip(const TextureData& source, StringView encoder, TextureData& decoded, const EncodeOptions& options = {}, usize* size = nullptr)
    {
        Array<u8> file;
        String error;
        if (!EncodeImage(source, encoder, file, error, options))
        {
            std::printf("    encode %s: %s\n", String(encoder).CStr(), error.CStr());
            return false;
        }
        if (!DecodeImage(Span<const u8>(file.Data(), file.Size()), decoded, error, encoder))
        {
            std::printf("    decode %s: %s\n", String(encoder).CStr(), error.CStr());
            return false;
        }
        if (size)
            *size = file.Size();
        return decoded.width == source.width && decoded.height == source.height;
    }
}

HX_TEST(Convert, EveryFormatKeepsItsOwnValues)
{
    u64 state = 99;
    for (u32 f = 1; f < u32(PixelFormat::Count); f++)
    {
        PixelFormat format = PixelFormat(f);
        if (!CanConvert(format))
            continue;
        // Quantized once, a value converts to the same bytes again.
        Array<float> rgba;
        for (u32 i = 0; i < 64 * 4; i++)
            rgba.Push(float(Random(state) % 2000) / 1000.0f - 0.5f);
        u32 stride = FormatInfo(format).bytesPerBlock;
        Array<u8> once, twice;
        once.Resize(64 * stride);
        twice.Resize(64 * stride);
        FromFloat(format, rgba.Data(), once.Data(), 64);
        Array<float> back;
        back.Resize(64 * 4);
        ToFloat(format, once.Data(), back.Data(), 64);
        FromFloat(format, back.Data(), twice.Data(), 64);
        bool same = std::memcmp(once.Data(), twice.Data(), once.Size()) == 0;
        if (!same)
            std::printf("    %s does not keep its values\n", String(FormatInfo(format).name).CStr());
        HX_CHECK(same);
    }
    u8 bgra[4] = {10, 20, 30, 40}, rgba8[4];
    HX_REQUIRE(ConvertPixels(PixelFormat::BGRA8, bgra, PixelFormat::RGBA8, rgba8, 1));
    HX_CHECK(rgba8[0] == 30 && rgba8[1] == 20 && rgba8[2] == 10 && rgba8[3] == 40);
    u8 block[16];
    HX_CHECK(!ConvertPixels(PixelFormat::BC1, block, PixelFormat::RGBA8, rgba8, 1));
}

HX_TEST(Convert, HalvesRoundToNearestEven)
{
    HX_CHECK_EQ(FloatToHalf(1.0f), u16(0x3C00));
    HX_CHECK_EQ(FloatToHalf(-2.0f), u16(0xC000));
    HX_CHECK_EQ(FloatToHalf(65504.0f), u16(0x7BFF));
    HX_CHECK_EQ(FloatToHalf(65520.0f), u16(0x7C00));
    HX_CHECK_EQ(FloatToHalf(std::ldexp(1.0f, -24)), u16(1));
    HX_CHECK_EQ(FloatToHalf(std::ldexp(1.0f, -26)), u16(0));
    // 1 + 2^-11 lies halfway between 1 and the next half: it rounds to even (1); 1 + 3 * 2^-11 rounds up.
    HX_CHECK_EQ(FloatToHalf(1.0f + std::ldexp(1.0f, -11)), u16(0x3C00));
    HX_CHECK_EQ(FloatToHalf(1.0f + 3.0f * std::ldexp(1.0f, -11)), u16(0x3C02));
    for (u32 h = 0; h < 0x7C00; h++)
        if (FloatToHalf(HalfToFloat(u16(h))) != h)
        {
            std::printf("    half %x does not round trip\n", h);
            HX_CHECK(false);
            break;
        }
}

HX_TEST(ImageEncoder, FindsEncodersByNameAndExtension)
{
    ImageEncoder found;
    HX_REQUIRE(FindImageEncoder(".PNG", found));
    HX_CHECK(found.name == StringView("PNG"));
    HX_REQUIRE(FindImageEncoder("jpeg", found));
    HX_CHECK(found.name == StringView("JPG"));
    HX_CHECK(FindImageEncoder("exr", found) && FindImageEncoder("Tga", found));
    TextureData t = Texture(PixelFormat::RGBA8, 4, 4);
    Array<u8> file;
    String error;
    HX_CHECK(!EncodeImage(t, "webp", file, error) && error == StringView("no encoder for webp"));
    TextureData block = Texture(PixelFormat::BC1, 8, 8);
    HX_CHECK(!EncodeImage(block, "png", file, error) && error == StringView("PNG: block compressed textures cannot be encoded"));
}

HX_TEST(ImageEncoder, PngAndTgaRoundTripExactly)
{
    u64 state = 7;
    TextureData rgba = Texture(PixelFormat::RGBA8, 37, 23);
    for (u8& b : rgba.data)
        b = u8(Random(state));
    for (const char* encoder : {"png", "tga"})
    {
        TextureData back;
        HX_REQUIRE(RoundTrip(rgba, encoder, back));
        HX_CHECK(back.format == PixelFormat::RGBA8 && std::memcmp(back.data.Data(), rgba.data.Data(), rgba.data.Size()) == 0);
    }
    // RGB and gray files read back with opaque alpha; 16 bit and float textures write 16 bit PNG files.
    TextureData rgb = Texture(PixelFormat::RGB8, 9, 5), gray = Texture(PixelFormat::R8, 9, 5);
    for (u8& b : rgb.data)
        b = u8(Random(state));
    for (u8& b : gray.data)
        b = u8(Random(state));
    TextureData back;
    HX_REQUIRE(RoundTrip(rgb, "tga", back));
    bool same = true;
    for (u32 i = 0; i < 45; i++)
        same = same && back.data[i * 4] == rgb.data[i * 3] && back.data[i * 4 + 2] == rgb.data[i * 3 + 2] && back.data[i * 4 + 3] == 255;
    HX_CHECK(same);
    HX_REQUIRE(RoundTrip(gray, "png", back));
    same = true;
    for (u32 i = 0; i < 45; i++)
        same = same && back.data[i * 4] == gray.data[i] && back.data[i * 4 + 1] == gray.data[i] && back.data[i * 4 + 3] == 255;
    HX_CHECK(same);
    TextureData wide = Texture(PixelFormat::RGBA16, 11, 7);
    for (usize i = 0; i < wide.data.Size(); i++)
        wide.data[i] = u8(Random(state));
    HX_REQUIRE(RoundTrip(wide, "png", back));
    HX_CHECK(back.format == PixelFormat::RGBA16 && std::memcmp(back.data.Data(), wide.data.Data(), wide.data.Size()) == 0);
}

HX_TEST(ImageEncoder, JpgKeepsTheImageAndDropsAlpha)
{
    const u32 w = 64, h = 48;
    Array<float> rgba = Smooth(w, h);
    TextureData source = Texture(PixelFormat::RGBA8, w, h);
    FromFloat(PixelFormat::RGBA8, rgba.Data(), source.data.Data(), usize(w) * h);
    usize sizes[2] = {};
    for (u32 q = 0; q < 2; q++)
    {
        EncodeOptions options;
        options.quality = q ? 95 : 10;
        Array<u8> file;
        String error;
        HX_REQUIRE(EncodeImage(source, "jpg", file, error, options));
        sizes[q] = file.Size();
        TextureData back;
        HX_REQUIRE(DecodeImage(Span<const u8>(file.Data(), file.Size()), back, error));
        double squared = 0;
        bool opaque = true;
        for (usize i = 0; i < usize(w) * h; i++)
        {
            for (u32 c = 0; c < 3; c++)
            {
                double d = double(back.data[i * 4 + c]) - double(source.data[i * 4 + c]);
                squared += d * d;
            }
            opaque = opaque && back.data[i * 4 + 3] == 255;
        }
        double psnr = 10.0 * std::log10(255.0 * 255.0 / (squared / (double(w) * h * 3) + 1e-9));
        std::printf("    quality %u: %zu bytes, PSNR %.1f dB\n", options.quality, file.Size(), psnr);
        HX_CHECK(psnr > (q ? 40.0 : 25.0));
        HX_CHECK(opaque);
    }
    HX_CHECK(sizes[0] < sizes[1]);
}

HX_TEST(ImageEncoder, ExrRoundTripsEveryCompression)
{
    struct Case
    {
        const char* name;
        u32 w, h, flags;
        bool wide;
    };
    // Odd sizes reach the odd row and column cases of the wavelet. More than 16384 different values (here neighbouring
    // half bit patterns, so they still compress) make PIZ use its 16 bit wavelet, smooth images the 14 bit one.
    const Case cases[] = {
        {"none half", 37, 23, ExrNone, false},
        {"rle half", 37, 23, ExrCompressRle, false},
        {"zip float", 37, 23, ExrCompressZip | ExrOutputAsFloat, false},
        {"piz half", 37, 23, ExrCompressPiz, false},
        {"piz float", 33, 70, ExrCompressPiz | ExrOutputAsFloat, false},
        {"piz half wide range", 256, 96, ExrCompressPiz, true},
        {"piz float wide range", 61, 45, ExrCompressPiz | ExrOutputAsFloat, true},
        {"piz one pixel", 1, 1, ExrCompressPiz, false},
    };
    u64 state = 12345;
    for (const Case& k : cases)
    {
        Array<float> rgba = Smooth(k.w, k.h);
        if (k.wide)
            for (usize i = 0; i < rgba.Size(); i++)
                rgba[i] = HalfToFloat(u16((i + Random(state) % 3) % 0x7B00));
        TextureData source = Texture(PixelFormat::RGBA32Float, k.w, k.h);
        std::memcpy(source.data.Data(), rgba.Data(), source.data.Size());
        EncodeOptions options;
        options.exrFlags = k.flags;
        TextureData back;
        usize size = 0;
        bool ok = RoundTrip(source, "exr", back, options, &size);
        if (!ok)
            std::printf("    %s\n", k.name);
        HX_REQUIRE(ok);
        bool asFloat = (k.flags & ExrOutputAsFloat) != 0;
        // Compressed chunks, not stored ones: the file is smaller than its samples.
        usize samples = usize(k.w) * k.h * 4 * (asFloat ? 4 : 2);
        std::printf("    %-22s %7zu bytes for %7zu bytes of samples\n", k.name, size, samples);
        if (k.flags != ExrNone && k.w > 1)
            HX_CHECK(size < samples);
        HX_CHECK(back.format == (asFloat ? PixelFormat::RGBA32Float : PixelFormat::RGBA16Float));
        bool same = true;
        for (usize i = 0; i < rgba.Size(); i++)
        {
            if (asFloat)
            {
                float got;
                std::memcpy(&got, back.data.Data() + i * 4, 4);
                same = same && got == rgba[i];
            }
            else
            {
                u16 got;
                std::memcpy(&got, back.data.Data() + i * 2, 2);
                same = same && got == FloatToHalf(rgba[i]);
            }
        }
        if (!same)
            std::printf("    %s decodes other values\n", k.name);
        HX_CHECK(same);
    }
}

HX_TEST(ImageEncoder, ExrWritesTheChannelsOfTheFormat)
{
    TextureData rgb = Texture(PixelFormat::RGB16Float, 5, 4);
    for (usize i = 0; i < 5 * 4 * 3; i++)
    {
        u16 h = FloatToHalf(float(i) * 0.5f);
        std::memcpy(rgb.data.Data() + i * 2, &h, 2);
    }
    TextureData back;
    HX_REQUIRE(RoundTrip(rgb, "exr", back));
    bool same = true;
    for (usize p = 0; p < 20; p++)
    {
        u16 v[4];
        std::memcpy(v, back.data.Data() + p * 8, 8);
        for (u32 c = 0; c < 3; c++)
            same = same && HalfToFloat(v[c]) == float(p * 3 + c) * 0.5f;
        same = same && HalfToFloat(v[3]) == 1.0f;
    }
    HX_CHECK(same);
}
