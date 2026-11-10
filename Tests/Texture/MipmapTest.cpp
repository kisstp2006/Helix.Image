#include <Helix/Image/Texture/Color.h>
#include <Helix/Image/Texture/Convert.h>
#include <Helix/Image/Texture/Mipmaps.h>
#include <Helix/Image/Texture/Resize.h>
#include <Helix/Jobs/Jobs.h>
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

    TextureData Texture(PixelFormat format, u32 w, u32 h, u32 layers = 1, u32 faces = 1)
    {
        TextureData t;
        t.Allocate(format, w, h, 1, layers, faces);
        return t;
    }

    bool Mips(const TextureData& source, const MipmapSettings& settings, TextureData& out)
    {
        String error;
        bool ok = GenerateMipmaps(source, settings, out, error);
        if (!ok)
            std::printf("    %s\n", error.CStr());
        return ok;
    }

    Array<float> Floats(const TextureData& t, u32 mip, u32 layer = 0, u32 face = 0)
    {
        const Subresource& s = t.At(layer, face, mip);
        Array<float> rgba;
        rgba.Resize(usize(s.width) * s.height * 4);
        ToFloat(t.format, t.Pixels(layer, face, mip), rgba.Data(), usize(s.width) * s.height);
        return rgba;
    }
}

HX_TEST(Mipmaps, ConstantImagesStayConstant)
{
    for (int kaiser = 0; kaiser < 2; kaiser++)
        for (int srgb = 0; srgb < 2; srgb++)
        {
            TextureData source = Texture(PixelFormat::RGBA8, 37, 23, 2, 1);
            for (usize i = 0; i < source.data.Size(); i += 4)
            {
                source.data[i] = 200;
                source.data[i + 1] = 13;
                source.data[i + 2] = 77;
                source.data[i + 3] = 140;
            }
            MipmapSettings settings;
            settings.filter = kaiser ? MipFilter::Kaiser : MipFilter::Box;
            settings.srgb = srgb != 0;
            TextureData out;
            HX_REQUIRE(Mips(source, settings, out));
            HX_CHECK(out.mips == 6u && out.At(1, 0, 5).width == 1u && out.At(1, 0, 5).height == 1u);
            bool same = true;
            for (usize i = 0; i < out.data.Size(); i += 4)
                same = same && out.data[i] == 200 && out.data[i + 1] == 13 && out.data[i + 2] == 77 && out.data[i + 3] == 140;
            HX_CHECK(same);
        }
}

HX_TEST(Mipmaps, SrgbIsFilteredInLinearLight)
{
    TextureData checker = Texture(PixelFormat::RGBA8, 64, 64);
    for (u32 y = 0; y < 64; y++)
        for (u32 x = 0; x < 64; x++)
        {
            u8 v = (x + y) & 1 ? 255 : 0;
            u8* p = checker.data.Data() + (usize(y) * 64 + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    MipmapSettings settings;
    settings.srgb = true;
    TextureData gamma, plain;
    HX_REQUIRE(Mips(checker, settings, gamma));
    settings.srgb = false;
    HX_REQUIRE(Mips(checker, settings, plain));
    HX_CHECK_EQ(gamma.Pixels(0, 0, 1)[0], u8(188));
    HX_CHECK_EQ(plain.Pixels(0, 0, 1)[0], u8(128));
    HX_CHECK_NEAR(SrgbToLinear(LinearToSrgb(0.25f)), 0.25f, 1e-6f);
    HX_CHECK_NEAR(SrgbByteToLinear(188), 0.5f, 0.003f);
}

HX_TEST(Mipmaps, FiltersKeepTheSumOfAnImpulse)
{
    for (int kaiser = 0; kaiser < 2; kaiser++)
    {
        TextureData impulse = Texture(PixelFormat::RGBA32Float, 32, 32);
        float* f = reinterpret_cast<float*>(impulse.data.Data());
        f[(16 * 32 + 15) * 4] = 64.0f;
        MipmapSettings settings;
        settings.filter = kaiser ? MipFilter::Kaiser : MipFilter::Box;
        TextureData out;
        HX_REQUIRE(Mips(impulse, settings, out));
        for (u32 mip = 1; mip < 4; mip++)
        {
            Array<float> level = Floats(out, mip);
            double sum = 0;
            for (usize i = 0; i < level.Size(); i += 4)
                sum += level[i];
            // The Kaiser kernel reaches 6 texels of the level above; once that is wider than the image, the edges cut it.
            if (kaiser && mip >= 3)
                continue;
            HX_CHECK_NEAR(sum * double(1u << (2 * mip)), 64.0, 1e-3);
        }
    }
}

HX_TEST(Mipmaps, CoverageIsKeptAtTheReference)
{
    // A disc with a soft edge: without the setting its alpha test area shrinks on the smaller levels.
    const u32 n = 128;
    TextureData disc = Texture(PixelFormat::RGBA8, n, n);
    for (u32 y = 0; y < n; y++)
        for (u32 x = 0; x < n; x++)
        {
            float dx = float(x) - 63.5f, dy = float(y) - 63.5f, d = std::sqrt(dx * dx + dy * dy);
            u8* p = disc.data.Data() + (usize(y) * n + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = u8(d < 40.0f ? 255.0f : d < 56.0f ? (56.0f - d) / 16.0f * 255.0f * ((x / 3 + y / 5) & 1 ? 1.0f : 0.4f) : 0.0f);
        }
    auto coverage = [](const Array<float>& rgba) {
        usize pass = 0;
        for (usize i = 3; i < rgba.Size(); i += 4)
            pass += rgba[i] > 0.5f;
        return double(pass) / double(rgba.Size() / 4);
    };
    MipmapSettings settings;
    settings.preserveCoverage = true;
    TextureData kept, lost;
    HX_REQUIRE(Mips(disc, settings, kept));
    settings.preserveCoverage = false;
    HX_REQUIRE(Mips(disc, settings, lost));
    double top = coverage(Floats(kept, 0));
    for (u32 mip = 1; mip < 6; mip++)
    {
        Array<float> level = Floats(kept, mip);
        double step = 1.0 / double(level.Size() / 4);
        double got = coverage(level), without = coverage(Floats(lost, mip));
        std::printf("    level %u: coverage %.4f, without the setting %.4f, top %.4f\n", mip, got, without, top);
        HX_CHECK(std::fabs(got - top) <= step + 1e-9);
    }
}

HX_TEST(Mipmaps, NormalsStayUnitLength)
{
    u64 state = 5;
    TextureData normals = Texture(PixelFormat::RGBA32Float, 40, 24);
    float* f = reinterpret_cast<float*>(normals.data.Data());
    for (usize i = 0; i < usize(40) * 24; i++)
    {
        float v[3] = {float(Random(state) % 2000) / 1000.0f - 1.0f, float(Random(state) % 2000) / 1000.0f - 1.0f, float(Random(state) % 1000) / 1000.0f + 0.1f};
        float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int c = 0; c < 3; c++)
            f[i * 4 + c] = v[c] / l * 0.5f + 0.5f;
        f[i * 4 + 3] = 1.0f;
    }
    MipmapSettings settings;
    settings.normalMap = true;
    settings.filter = MipFilter::Kaiser;
    TextureData out;
    HX_REQUIRE(Mips(normals, settings, out));
    bool unit = true;
    for (u32 mip = 1; mip < out.mips; mip++)
    {
        Array<float> level = Floats(out, mip);
        for (usize i = 0; i < level.Size(); i += 4)
        {
            float x = level[i] * 2 - 1, y = level[i + 1] * 2 - 1, z = level[i + 2] * 2 - 1;
            unit = unit && std::fabs(std::sqrt(x * x + y * y + z * z) - 1.0f) < 1e-5f;
        }
    }
    HX_CHECK(unit);
}

HX_TEST(Mipmaps, BorderAndFade)
{
    u64 state = 77;
    TextureData source = Texture(PixelFormat::RGBA8, 32, 16);
    for (u8& b : source.data)
        b = u8(Random(state));
    MipmapSettings settings;
    settings.borderMipmap = true;
    TextureData out;
    HX_REQUIRE(Mips(source, settings, out));
    bool edges = true;
    for (u32 mip = 1; mip < out.mips; mip++)
    {
        const Subresource& s = out.At(0, 0, mip);
        for (u32 x = 0; x < s.width; x++)
        {
            u32 tx = s.width > 1 ? x * 31 / (s.width - 1) : 0;
            edges = edges && std::memcmp(out.Pixels(0, 0, mip) + x * 4, source.data.Data() + tx * 4, 4) == 0;
        }
    }
    HX_CHECK(edges);

    settings.borderMipmap = false;
    settings.fadeout = true;
    settings.fadeStart = 1;
    settings.fadeEnd = 3;
    HX_REQUIRE(Mips(source, settings, out));
    HX_CHECK(std::memcmp(out.Pixels(0, 0, 0), source.data.Data(), source.data.Size()) == 0);
    bool gray = true;
    for (u32 mip = 3; mip < out.mips; mip++)
    {
        const Subresource& s = out.At(0, 0, mip);
        for (u64 i = 0; i < s.size; i += 4)
            gray = gray && out.Pixels(0, 0, mip)[i] == 128 && out.Pixels(0, 0, mip)[i + 2] == 128;
    }
    HX_CHECK(gray);
    TextureData plain;
    settings.fadeout = false;
    HX_REQUIRE(Mips(source, settings, plain));
    // Level 2 is halfway between its own color and gray.
    i32 own = plain.Pixels(0, 0, 2)[0], faded = out.Pixels(0, 0, 2)[0];
    HX_CHECK(std::abs(faded - (own + 128) / 2) <= 1);
}

HX_TEST(Resize, MaxSizeAndFilters)
{
    u32 w = 0, h = 0;
    FitToMaxSize(4096, 1024, 1024, w, h);
    HX_CHECK(w == 1024u && h == 256u);
    FitToMaxSize(100, 50, 64, w, h);
    HX_CHECK(w == 64u && h == 32u);
    FitToMaxSize(1000, 3, 256, w, h);
    HX_CHECK(w == 256u && h == 1u);
    FitToMaxSize(512, 512, 1024, w, h);
    HX_CHECK(w == 512u && h == 512u);

    // A horizontal ramp stays a ramp, both down and up, and a constant stays the constant.
    for (int filter = 0; filter < 2; filter++)
        for (u32 target : {u32(13), u32(90)})
        {
            Array<float> ramp, out;
            for (u32 y = 0; y < 7; y++)
                for (u32 x = 0; x < 40; x++)
                {
                    ramp.Push(float(x) / 39.0f);
                    ramp.Push(0.25f);
                    ramp.Push(0.0f);
                    ramp.Push(1.0f);
                }
            out.Resize(usize(target) * 5 * 4);
            Resample(ramp.Data(), 40, 7, out.Data(), target, 5, filter ? ResampleFilter::Bilinear : ResampleFilter::Mitchell);
            bool rising = true, constant = true;
            for (u32 y = 0; y < 5; y++)
                for (u32 x = 0; x < target; x++)
                {
                    const float* p = out.Data() + (usize(y) * target + x) * 4;
                    if (x > 0)
                        rising = rising && p[0] >= p[-4] - 1e-6f;
                    constant = constant && std::fabs(p[1] - 0.25f) < 1e-6f && std::fabs(p[3] - 1.0f) < 1e-6f;
                }
            HX_CHECK(rising);
            HX_CHECK(constant);
        }
    TextureData big = Texture(PixelFormat::RGBA8, 64, 32, 1, 6), small;
    String error;
    HX_REQUIRE(ResizeTexture(big, 16, 8, ResampleFilter::Mitchell, true, small, error));
    HX_CHECK(small.width == 16u && small.height == 8u && small.faces == 6u && small.mips == 1u);
}

HX_TEST(Mipmaps, JobsGiveTheSameBytes)
{
    u64 state = 31;
    TextureData source = Texture(PixelFormat::RGBA8, 512, 300);
    for (u8& b : source.data)
        b = u8(Random(state));
    MipmapSettings settings;
    settings.filter = MipFilter::Kaiser;
    settings.srgb = true;
    settings.preserveCoverage = true;
    bool wasRunning = jobs::IsInitialized();
    if (wasRunning)
        jobs::Shutdown();
    TextureData serial, parallel;
    HX_REQUIRE(Mips(source, settings, serial));
    HX_REQUIRE(jobs::Initialize({.workerCount = 4}));
    HX_REQUIRE(Mips(source, settings, parallel));
    jobs::Shutdown();
    if (wasRunning)
        jobs::Initialize();
    HX_CHECK(serial.data.Size() == parallel.data.Size() && std::memcmp(serial.data.Data(), parallel.data.Data(), serial.data.Size()) == 0);
}

HX_TEST(Mipmaps, BlockFormatsAndVolumesAreErrors)
{
    TextureData block = Texture(PixelFormat::BC1, 16, 16), out;
    String error;
    HX_CHECK(!GenerateMipmaps(block, {}, out, error) && !error.Empty());
    TextureData volume;
    volume.Allocate(PixelFormat::RGBA8, 4, 4, 4);
    HX_CHECK(!GenerateMipmaps(volume, {}, out, error));
}
