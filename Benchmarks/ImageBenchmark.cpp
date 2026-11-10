#include <Helix/Image/Codecs/Png.h>
#include <Helix/Image/Compression/Deflate.h>
#include <Helix/Testing/Bench.h>

using namespace helix;
using namespace helix::image;

namespace
{
    Image Gradient()
    {
        Image image;
        image.Allocate(512, 512, ImageFormat::RGBA8);
        for (u32 y = 0; y < 512; y++)
            for (u32 x = 0; x < 512; x++)
            {
                u8* p = image.pixels.Data() + (usize(y) * 512 + x) * 4;
                p[0] = u8(x / 2);
                p[1] = u8(y / 2);
                p[2] = u8((x ^ y) & 0x3F);
                p[3] = 255;
            }
        return image;
    }
}

HX_BENCH(PngEncode512)
{
    Image image = Gradient();
    state.SetItemsPerIteration(512 * 512);
    while (state.KeepRunning())
    {
        Array<u8> file;
        EncodePng(image, file);
        testing::DoNotOptimize(file.Data());
    }
}

HX_BENCH(PngDecode512)
{
    Array<u8> file;
    EncodePng(Gradient(), file);
    state.SetItemsPerIteration(512 * 512);
    while (state.KeepRunning())
    {
        Image out;
        DecodePng(file, out);
        testing::DoNotOptimize(out.pixels.Data());
    }
}

namespace
{
    // Mixed content: repeated phrases with noise, close to filtered image rows and text assets.
    Array<u8> MixedData()
    {
        Array<u8> data;
        data.Resize(1 << 20);
        u32 state = 7;
        for (usize i = 0; i < data.Size(); i++)
        {
            state = state * 1103515245u + 12345u;
            data[i] = (state >> 28) < 3 ? u8(state >> 16) : u8("helix engine deflate benchmark data "[(i + (state >> 30)) % 36]);
        }
        return data;
    }
}

HX_BENCH(Deflate1MB)
{
    Array<u8> data = MixedData();
    state.SetItemsPerIteration(data.Size());
    while (state.KeepRunning())
    {
        Array<u8> packed;
        ZlibCompress(data, packed);
        testing::DoNotOptimize(packed.Data());
    }
}

HX_BENCH(Inflate1MB)
{
    Array<u8> data = MixedData(), packed;
    ZlibCompress(data, packed);
    state.SetItemsPerIteration(data.Size());
    while (state.KeepRunning())
    {
        Array<u8> out;
        ZlibDecompress(packed, out);
        testing::DoNotOptimize(out.Data());
    }
}
