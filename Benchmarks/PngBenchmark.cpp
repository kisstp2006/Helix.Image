#include <Helix/Image/Codecs/Png.h>
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
