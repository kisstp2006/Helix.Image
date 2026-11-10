#include <Helix/Image/Texture/BlockCodec.h>
#include <Helix/Image/Texture/Convert.h>
#include <Helix/Jobs/Jobs.h>
#include <Helix/Testing/Test.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::image;

namespace
{
    // The test set: a smooth photo like area, hard edges with noise, and an alpha disc, side by side in RGBA float.
    TextureData TestSet(u32 w = 96, u32 h = 48)
    {
        TextureData t;
        t.Allocate(PixelFormat::RGBA32Float, w, h);
        float* f = reinterpret_cast<float*>(t.data.Data());
        u64 state = 1;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
            {
                float* p = f + (usize(y) * w + x) * 4;
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                float noise = float(state % 1000) / 1000.0f;
                if (x < w / 3)
                {
                    p[0] = 0.5f + 0.4f * std::sin(float(x) * 0.2f + float(y) * 0.05f);
                    p[1] = float(y) / float(h);
                    p[2] = 0.3f + 0.2f * std::cos(float(x + y) * 0.13f);
                    p[3] = 1.0f;
                }
                else if (x < 2 * w / 3)
                {
                    // Hard edges of a pattern in light and dark with a tint, and a little noise, as text and lines in photos.
                    bool edge = ((x / 5) + (y / 7)) & 1;
                    float light = (edge ? 0.85f : 0.15f) + 0.06f * (noise - 0.5f);
                    p[0] = light;
                    p[1] = light * 0.9f + 0.05f;
                    p[2] = light * 0.7f + ((x * 7 + y * 3) % 32 < 16 ? 0.1f : 0.0f);
                    p[3] = 1.0f;
                }
                else
                {
                    float dx = float(x) - float(w) * 5.0f / 6.0f, dy = float(y) - float(h) * 0.5f, d = std::sqrt(dx * dx + dy * dy);
                    p[0] = 0.7f;
                    p[1] = 0.4f + 0.3f * noise;
                    p[2] = 0.2f;
                    p[3] = d < 10.0f ? 1.0f : d < 20.0f ? (20.0f - d) / 10.0f : 0.0f;
                }
            }
        return t;
    }

    // PSNR of the decoded texture against the source in the decoded format, over the channels the format has.
    double Psnr(const TextureData& source, const TextureData& decoded, u32 channels, bool punchThrough = false)
    {
        usize count = usize(source.width) * source.height;
        Array<float> a, b;
        a.Resize(count * 4);
        b.Resize(count * 4);
        Array<u8> quantized;
        quantized.Resize(count * FormatInfo(decoded.format).bytesPerBlock);
        ConvertPixels(source.format, source.data.Data(), decoded.format, quantized.Data(), count);
        ToFloat(decoded.format, quantized.Data(), a.Data(), count);
        // Punch through alpha: a texel below half alpha is transparent black, every other one opaque.
        if (punchThrough)
            for (usize i = 0; i < count; i++)
            {
                bool clear = a[i * 4 + 3] < 0.5f;
                for (u32 c = 0; c < 3; c++)
                    a[i * 4 + c] = clear ? 0.0f : a[i * 4 + c];
                a[i * 4 + 3] = clear ? 0.0f : 1.0f;
            }
        ToFloat(decoded.format, decoded.data.Data(), b.Data(), count);
        // HDR formats are measured as their encoders fit them: on the half values in order, close to a log scale.
        if (decoded.format == PixelFormat::RGBA16Float)
            for (Array<float>* values : {&a, &b})
                for (float& v : *values)
                {
                    u16 h = FloatToHalf(v);
                    v = float(h & 0x8000 ? -i32(h & 0x7FFF) : i32(h)) / 31743.0f;
                }
        double squared = 0;
        for (usize i = 0; i < count; i++)
            for (u32 c = 0; c < channels; c++)
            {
                double d = double(a[i * 4 + c]) - double(b[i * 4 + c]);
                squared += d * d;
            }
        double mse = squared / double(count * channels);
        return mse > 0 ? 10.0 * std::log10(1.0 / mse) : 99.0;
    }

    struct Expectation
    {
        PixelFormat format;
        u32 channels;
        // The PSNR each tier must reach (dB).
        double fast, normal, best;
    };
}

HX_TEST(BlockEncoder, EveryEncoderReachesItsQuality)
{
    const Expectation expectations[] = {
        {PixelFormat::BC1, 3, 30, 30, 30},   {PixelFormat::BC2, 4, 28, 28, 28},   {PixelFormat::BC3, 4, 30, 30, 30},
        {PixelFormat::BC4, 1, 36, 38, 40},   {PixelFormat::BC4Snorm, 1, 36, 38, 40}, {PixelFormat::BC5, 2, 36, 38, 40},
        {PixelFormat::BC5Snorm, 2, 36, 38, 40}, {PixelFormat::BC7, 4, 32, 38, 40},  {PixelFormat::BC6H, 3, 48, 48, 50}, {PixelFormat::BC6HSigned, 3, 48, 48, 50},
        {PixelFormat::ETC2RGB, 3, 28, 32, 32}, {PixelFormat::ETC2RGBA1, 4, 28, 30, 30}, {PixelFormat::ETC2RGBA, 4, 29, 33, 33},
        {PixelFormat::EACR11, 1, 39, 40, 41}, {PixelFormat::EACR11Snorm, 1, 39, 40, 41}, {PixelFormat::EACRG11, 2, 39, 40, 41},
        {PixelFormat::EACRG11Snorm, 2, 39, 40, 41}, {PixelFormat::ASTC4x4, 4, 32, 34, 35}, {PixelFormat::ASTC6x6, 4, 23, 25, 26},
        {PixelFormat::ASTC8x8, 4, 20, 21, 22}, {PixelFormat::ASTC12x12, 4, 17, 19, 20},
    };
    TextureData source = TestSet();
    for (const Expectation& e : expectations)
    {
        BlockCodec codec;
        HX_REQUIRE(FindBlockCodec(e.format, codec) && codec.encode);
        double psnr[3];
        for (u32 q = 0; q < 3; q++)
        {
            TextureData compressed, decoded;
            String error;
            HX_REQUIRE(CompressTexture(source, e.format, CompressionQuality(q), compressed, error));
            HX_REQUIRE(DecompressTexture(compressed, decoded, error));
            psnr[q] = Psnr(source, decoded, e.channels, e.format == PixelFormat::ETC2RGBA1);
        }
        std::printf("    %-12s PSNR fast %5.1f, normal %5.1f, best %5.1f dB\n", String(FormatInfo(e.format).name).CStr(), psnr[0], psnr[1], psnr[2]);
        HX_CHECK(psnr[0] >= e.fast && psnr[1] >= e.normal && psnr[2] >= e.best);
        HX_CHECK(psnr[2] >= psnr[0] - 0.05);
    }
}

HX_TEST(BlockEncoder, JobsGiveTheSameBlocks)
{
    TextureData source = TestSet(160, 120);
    bool wasRunning = jobs::IsInitialized();
    if (wasRunning)
        jobs::Shutdown();
    TextureData serial, parallel;
    String error;
    HX_REQUIRE(CompressTexture(source, PixelFormat::BC3, CompressionQuality::Normal, serial, error));
    HX_REQUIRE(jobs::Initialize({.workerCount = 4}));
    HX_REQUIRE(CompressTexture(source, PixelFormat::BC3, CompressionQuality::Normal, parallel, error));
    jobs::Shutdown();
    if (wasRunning)
        jobs::Initialize();
    HX_CHECK(std::memcmp(serial.data.Data(), parallel.data.Data(), serial.data.Size()) == 0);
}
