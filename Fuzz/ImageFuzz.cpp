#include <Generated/TestImages.h>
#include <Support/HdrTiffWriters.h>
#include <Support/ImageWriters.h>
#include <Support/TestVectors.h>

#include <Helix/Image/Codecs/ImageDecoder.h>
#include <Helix/Testing/Fuzz.h>

using namespace helix;
using namespace helix::image;
using namespace helix::image::testing;

namespace
{
    // Decodes a mutated copy of a seed file; a file that still decodes counts as an item.
    u64 DecodeMutated(helix::testing::FuzzRound& round, const u8* seed, usize size, StringView extension)
    {
        Array<u8> bytes;
        bytes.Append(seed, size);
        bytes.Resize(size + 64);
        bytes.Resize(round.Mutate(bytes.Data(), size, bytes.Size()));
        TextureData texture;
        String error;
        round.Step("decode", String(extension).CStr());
        return DecodeImage(Span<const u8>(bytes.Data(), bytes.Size()), texture, error, extension) ? 1 : 0;
    }

    u64 DecodeMutated(helix::testing::FuzzRound& round, const Array<u8>& seed, StringView extension)
    {
        return DecodeMutated(round, seed.Data(), seed.Size(), extension);
    }

    const TestImage& Windows(const char* name)
    {
        for (const TestImage& t : kTestImages)
            if (StringView(t.name) == StringView(name))
                return t;
        return kTestImages[0];
    }
}

HX_FUZZ(Png)
{
    const vectors::PngVector& v = vectors::kPngVectors[round.Below(sizeof(vectors::kPngVectors) / sizeof(vectors::kPngVectors[0]))];
    return DecodeMutated(round, v.data, v.size, "png");
}

HX_FUZZ(Jpeg)
{
    const TestImage& t = Windows("Jpeg");
    return DecodeMutated(round, t.file, usize(t.fileSize), "jpg");
}

HX_FUZZ(Bmp)
{
    const TestImage& t = Windows("Bmp");
    return DecodeMutated(round, t.file, usize(t.fileSize), "bmp");
}

HX_FUZZ(Gif)
{
    if (round.Below(2))
    {
        const TestImage& t = Windows("Gif");
        return DecodeMutated(round, t.file, usize(t.fileSize), "gif");
    }
    Array<Array<u8>> frames;
    frames.Push(Pattern(5, 3, false));
    frames.Push(Pattern(5, 3, false));
    return DecodeMutated(round, WriteGif(frames, 5, 3, 4), "gif");
}

HX_FUZZ(Tga)
{
    return DecodeMutated(round, WriteTga(Pattern(9, 5, true), 9, 5, round.Below(2) != 0, round.Below(2) != 0), "tga");
}

HX_FUZZ(Psd)
{
    return DecodeMutated(round, WritePsd(Pattern(7, 4, false), 7, 4, round.Below(2) != 0), "psd");
}

HX_FUZZ(Hdr)
{
    Array<float> rgb;
    for (u32 i = 0; i < 12 * 3 * 3; i++)
        rgb.Push(float(i % 7) * 0.4f);
    Array<u8> rgbe;
    return DecodeMutated(round, WriteHdr(rgb, 12, 3, round.Below(2) != 0, round.Below(2) != 0, rgbe), "hdr");
}

HX_FUZZ(Tiff)
{
    if (round.Below(4) == 0)
    {
        const TestImage& t = Windows("TiffLzw");
        return DecodeMutated(round, t.file, usize(t.fileSize), "tif");
    }
    static constexpr u32 kCompressions[] = {1, 8, 32773};
    TiffOptions o;
    o.bits = round.Below(3) == 0 ? 16 : 8;
    o.samples = 1 + round.Below(4);
    o.photometric = o.samples >= 3 ? 2 : 1;
    o.compression = kCompressions[round.Below(3)];
    o.predictor = round.Below(2) ? 2 : 1;
    o.bigEndian = round.Below(2) != 0;
    o.tiled = round.Below(3) == 0;
    o.planar = round.Below(3) == 0;
    Array<u32> values;
    for (u32 i = 0; i < 17 * 9 * o.samples; i++)
        values.Push((i * 29) & (o.bits == 16 ? 0xFFFFu : 0xFFu));
    return DecodeMutated(round, WriteTiff(values, 17, 9, o), "tif");
}
