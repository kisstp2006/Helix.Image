#include <Generated/TestImages.h>
#include <Support/ContainerWriters.h>
#include <Support/ExrWriter.h>
#include <Support/HdrTiffWriters.h>
#include <Support/ImageWriters.h>
#include <Support/TestVectors.h>

#include <Helix/Image/Codecs/ImageDecoder.h>
#include <Helix/Testing/Fuzz.h>

#include <cstring>

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

HX_FUZZ(Exr)
{
    Array<ExrChannel> channels;
    u32 type = round.Below(3);
    channels.Push({"B", type});
    channels.Push({"G", type});
    channels.Push({"R", type});
    Array<u32> samples;
    for (u32 i = 0; i < 19 * 18 * 3; i++)
        samples.Push(type == 1 ? 0x3C00u + (i % 5) * 64 : (i % 9) * 0x01010101u);
    ExrOptions o;
    // PIZ has no writer yet: the other compressions, with the byte that names it changed by the mutations.
    o.compression = round.Below(4);
    o.tiled = round.Below(3) == 0;
    Array<u8> file = WriteExr(channels, samples, 19, 18, o);
    if (round.Below(3) == 0)
    {
        // Calls the data PIZ, so the PIZ reader gets damaged input of every kind.
        static constexpr char kName[] = "compression";
        for (usize i = 0; i + 32 < file.Size(); i++)
            if (std::memcmp(file.Data() + i, kName, sizeof(kName)) == 0 && std::memcmp(file.Data() + i + sizeof(kName), kName, sizeof(kName)) == 0)
            {
                file[i + 2 * sizeof(kName) + 4] = 4;
                break;
            }
    }
    return DecodeMutated(round, file, "exr");
}

namespace
{
    TextureData Shape(helix::testing::FuzzRound& round, PixelFormat format)
    {
        TextureData t;
        bool cube = round.Below(4) == 0;
        t.Allocate(format, 4 + round.Below(29), 4 + round.Below(29), 1, 1 + round.Below(3), cube ? 6 : 1, 1 + round.Below(5));
        FillPattern(t);
        return t;
    }
}

HX_FUZZ(Dds)
{
    static constexpr u32 kMasks[5] = {32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000};
    switch (round.Below(3))
    {
    case 0: return DecodeMutated(round, WriteDds(Shape(round, PixelFormat::BC1), u32('D') | u32('X') << 8 | u32('T') << 16 | u32('1') << 24, 0), "dds");
    case 1: return DecodeMutated(round, WriteDds(Shape(round, PixelFormat::BC7), 0, 98), "dds");
    default: return DecodeMutated(round, WriteDds(Shape(round, PixelFormat::RGBA8), 0, 0, kMasks), "dds");
    }
}

HX_FUZZ(Ktx)
{
    if (round.Below(2))
        return DecodeMutated(round, WriteKtx1(Shape(round, PixelFormat::RGB8), 0x8051, 1, round.Below(2) != 0), "ktx");
    return DecodeMutated(round, WriteKtx1(Shape(round, PixelFormat::ETC2RGBA), 0x9278, 1, false), "ktx");
}

HX_FUZZ(Ktx2)
{
    return DecodeMutated(round, WriteKtx2(Shape(round, round.Below(2) ? PixelFormat::BC3 : PixelFormat::RGBA16Float), round.Below(2) ? 137 : 97, round.Below(2) ? 3 : 0), "ktx2");
}

HX_FUZZ(Pvr)
{
    if (round.Below(2))
        return DecodeMutated(round, WritePvr(Shape(round, PixelFormat::ASTC4x4), 27, 0, 0, 0), "pvr");
    return DecodeMutated(round, WritePvr(Shape(round, PixelFormat::RGBA8), u32('r') | u32('g') << 8 | u32('b') << 16 | u32('a') << 24, 0x08080808, 0, 0), "pvr");
}
