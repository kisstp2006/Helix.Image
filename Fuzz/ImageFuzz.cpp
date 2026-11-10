#include <Generated/TestImages.h>
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
