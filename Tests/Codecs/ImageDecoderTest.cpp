#include <Generated/TestImages.h>
#include <Support/ImageWriters.h>
#include <Support/TestVectors.h>

#include <Helix/Image/Codecs/ImageDecoder.h>
#include <Helix/Image/Codecs/Png.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::image;
using namespace helix::image::testing;

namespace
{
    // Largest difference between two RGBA8 images of the same size.
    u32 Difference(const u8* a, const u8* b, usize bytes)
    {
        u32 worst = 0;
        for (usize i = 0; i < bytes; i++)
        {
            u32 d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
            worst = d > worst ? d : worst;
        }
        return worst;
    }

    bool Decode(const Array<u8>& file, TextureData& out, StringView extension = {})
    {
        String error;
        bool ok = DecodeImage(Span<const u8>(file.Data(), file.Size()), out, error, extension);
        if (!ok)
            std::printf("    %s\n", error.CStr());
        return ok;
    }
}

HX_TEST(ImageDecoder, FindsDecodersBySignatureAndExtension)
{
    Array<ImageDecoder> decoders;
    ImageDecoders(decoders);
    HX_CHECK(decoders.Size() >= 6u);
    ImageDecoder found;
    HX_REQUIRE(FindImageDecoder(Span<const u8>(vectors::kRgba8, sizeof(vectors::kRgba8)), {}, found));
    HX_CHECK(found.name == StringView("PNG"));
    // A PNG named .tga is still a PNG; a file without a signature needs its extension.
    HX_REQUIRE(FindImageDecoder(Span<const u8>(vectors::kRgba8, sizeof(vectors::kRgba8)), ".tga", found));
    HX_CHECK(found.name == StringView("PNG"));
    const u8 unknown[] = {1, 2, 3, 4, 5, 6, 7, 8};
    HX_REQUIRE(FindImageDecoder(Span<const u8>(unknown, 8), ".TGA", found));
    HX_CHECK(found.name == StringView("TGA"));
    HX_CHECK(!FindImageDecoder(Span<const u8>(unknown, 8), {}, found));
    TextureData texture;
    String error;
    HX_CHECK(!DecodeImage(Span<const u8>(unknown, 8), texture, error));
    HX_CHECK(!error.Empty());
}

HX_TEST(ImageDecoder, TakesNewDecodersAndReplacesByName)
{
    static constexpr StringView kExtensions[] = {"probe"};
    ImageDecoder probe;
    probe.name = "Probe";
    probe.extensions = Span<const StringView>(kExtensions, 1);
    probe.matches = [](Span<const u8> data) { return data.Size() >= 5 && std::memcmp(data.Data(), "PROBE", 5) == 0; };
    probe.decode = [](Span<const u8>, TextureData& out, String&) { return out.Allocate(PixelFormat::R8, 1, 1); };
    RegisterImageDecoder(probe);
    TextureData texture;
    String error;
    const u8 file[] = {'P', 'R', 'O', 'B', 'E'};
    HX_CHECK(DecodeImage(Span<const u8>(file, 5), texture, error) && texture.format == PixelFormat::R8);
    probe.decode = [](Span<const u8>, TextureData&, String& e) {
        e = "always fails";
        return false;
    };
    RegisterImageDecoder(probe);
    HX_CHECK(!DecodeImage(Span<const u8>(file, 5), texture, error));
    HX_CHECK(error == StringView("Probe: always fails"));
}

HX_TEST(ImageDecoder, PngKeepsSixteenBits)
{
    TextureData wide;
    String error;
    HX_REQUIRE(DecodeImage(Span<const u8>(vectors::kRgba16, sizeof(vectors::kRgba16)), wide, error));
    HX_CHECK(wide.format == PixelFormat::RGBA16);
    Image narrow;
    HX_REQUIRE(DecodePng(Span<const u8>(vectors::kRgba16, sizeof(vectors::kRgba16)), narrow));
    const u16* samples = reinterpret_cast<const u16*>(wide.data.Data());
    u32 worst = 0;
    for (usize i = 0; i < narrow.pixels.Size(); i++)
    {
        u32 a = narrow.pixels[i] * 257u, b = samples[i];
        worst = Max(worst, a > b ? a - b : b - a);
    }
    HX_CHECK(worst <= 128u);
    TextureData eight;
    HX_REQUIRE(DecodeImage(Span<const u8>(vectors::kRgba8, sizeof(vectors::kRgba8)), eight, error));
    HX_CHECK(eight.format == PixelFormat::RGBA8 && eight.subresources.Size() == 1u);
}

HX_TEST(ImageDecoder, MatchesTheWindowsCodecs)
{
    for (const TestImage& t : kTestImages)
    {
        if (std::strcmp(t.extension, "tif") == 0)
            continue;
        TextureData texture;
        String error;
        bool ok = DecodeImage(Span<const u8>(t.file, usize(t.fileSize)), texture, error, t.extension);
        if (!ok)
            std::printf("    %s: %s\n", t.name, error.CStr());
        HX_REQUIRE(ok);
        HX_CHECK(texture.format == PixelFormat::RGBA8 && texture.width == t.width && texture.height == t.height);
        u32 worst = Difference(texture.data.Data(), t.rgba, usize(t.width) * t.height * 4);
        std::printf("    %-8s largest difference %u (allowed %u)\n", t.name, worst, t.tolerance);
        HX_CHECK(worst <= t.tolerance);
    }
}

HX_TEST(ImageDecoder, ReadsTgaRawAndRunLength)
{
    Array<u8> pattern = Pattern(19, 7, true);
    for (int rle = 0; rle < 2; rle++)
        for (int topDown = 0; topDown < 2; topDown++)
        {
            Array<u8> file = WriteTga(pattern, 19, 7, rle != 0, topDown != 0);
            TextureData texture;
            HX_REQUIRE(Decode(file, texture, "tga"));
            HX_CHECK(texture.width == 19u && texture.height == 7u);
            HX_CHECK(Difference(texture.data.Data(), pattern.Data(), pattern.Size()) == 0u);
        }
}

HX_TEST(ImageDecoder, ReadsPsdAtEightAndSixteenBits)
{
    // Opaque: the composite of a PSD is matted against white where alpha is below 255, and the reader removes the matte.
    Array<u8> pattern = Pattern(11, 5, false);
    TextureData eight, sixteen;
    HX_REQUIRE(Decode(WritePsd(pattern, 11, 5, false), eight));
    HX_CHECK(eight.format == PixelFormat::RGBA8 && Difference(eight.data.Data(), pattern.Data(), pattern.Size()) == 0u);
    HX_REQUIRE(Decode(WritePsd(pattern, 11, 5, true), sixteen));
    HX_REQUIRE(sixteen.format == PixelFormat::RGBA16);
    const u16* samples = reinterpret_cast<const u16*>(sixteen.data.Data());
    bool same = true;
    for (usize i = 0; i < pattern.Size(); i++)
        same = same && samples[i] == u16(pattern[i] * 257u + (i % 4 == 0 ? 1u : 0u));
    HX_CHECK(same);
}

HX_TEST(ImageDecoder, ReadsGifFramesAsLayers)
{
    Array<Array<u8>> frames;
    for (u32 f = 0; f < 3; f++)
    {
        Array<u8>& rgba = frames.Emplace();
        for (u32 i = 0; i < 6 * 4; i++)
        {
            u32 c = (i + f) % 3;
            rgba.Push(u8(c == 0 ? 255 : 0));
            rgba.Push(u8(c == 1 ? 255 : 0));
            rgba.Push(u8(c == 2 ? 200 : 10));
            rgba.Push(255);
        }
    }
    TextureData texture;
    HX_REQUIRE(Decode(WriteGif(frames, 6, 4, 7), texture));
    HX_REQUIRE(texture.layers == 3u && texture.frameDelays.Size() == 3u);
    for (u32 f = 0; f < 3; f++)
    {
        HX_CHECK(texture.frameDelays[f] == 70u);
        HX_CHECK(Difference(texture.Pixels(f), frames[f].Data(), frames[f].Size()) == 0u);
    }
}

// "Restore to previous" (disposal 3): the frame after it is drawn over the frame from before it. stb_image took that
// frame from before its frame buffer (fixed in our copy; found by AddressSanitizer on the corpus' animated GIF).
HX_TEST(ImageDecoder, GifRestoreToPrevious)
{
    const u8 colors[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    Array<GifFrame> frames;
    const u32 rects[3][5] = {{0, 0, 6, 4, 1}, {0, 0, 6, 4, 3}, {2, 1, 2, 2, 0}};
    for (u32 f = 0; f < 3; f++)
    {
        GifFrame& g = frames.Emplace();
        g.x = rects[f][0], g.y = rects[f][1], g.w = rects[f][2], g.h = rects[f][3], g.disposal = rects[f][4];
        for (u32 i = 0; i < g.w * g.h; i++)
            g.rgba.Push(colors[f][0]), g.rgba.Push(colors[f][1]), g.rgba.Push(colors[f][2]), g.rgba.Push(255);
    }
    TextureData texture;
    HX_REQUIRE(Decode(WriteGifFrames(frames, 6, 4, 5), texture));
    HX_REQUIRE(texture.layers == 3u);
    bool same = true;
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 6; x++)
        {
            bool inRect = x >= 2 && x < 4 && y >= 1 && y < 3;
            const u8* expected[3] = {colors[0], colors[1], inRect ? colors[2] : colors[0]};
            for (u32 f = 0; f < 3; f++)
            {
                const u8* p = texture.Pixels(f) + (y * 6 + x) * 4;
                same = same && p[0] == expected[f][0] && p[1] == expected[f][1] && p[2] == expected[f][2] && p[3] == 255;
            }
        }
    HX_CHECK(same);
}

// A TGA or BMP header that claims more pixels than the rest of the file can hold, raw or run length coded, is an
// error before any pixel is made (found by the fuzz runner: a 96 byte TGA asked for 1.8 GB).
HX_TEST(ImageDecoder, ClaimsBeyondTheDataAreErrors)
{
    Array<Array<u8>> one;
    Array<u8>& px = one.Emplace();
    for (u32 i = 0; i < 4 * 3; i++)
        px.Push(u8(i * 20)), px.Push(u8(i * 7)), px.Push(u8(255 - i)), px.Push(255);
    for (bool rle : {false, true})
    {
        Array<u8> tga = WriteTga(px, 4, 3, rle, false);
        TextureData texture;
        String error;
        HX_REQUIRE(DecodeImage(Span<const u8>(tga.Data(), tga.Size()), texture, error, "tga"));
        // Width 50000, height 9000.
        tga[12] = 0x50, tga[13] = 0xC3, tga[14] = 0x28, tga[15] = 0x23;
        HX_CHECK(!DecodeImage(Span<const u8>(tga.Data(), tga.Size()), texture, error, "tga"));
        HX_CHECK(StringView(error).Contains(StringView("where the file holds at most")));
    }
    Array<u8> bmp;
    bmp.Append(kBmpFile, sizeof(kBmpFile));
    TextureData texture;
    String error;
    HX_REQUIRE(DecodeImage(Span<const u8>(bmp.Data(), bmp.Size()), texture, error));
    // Height 20000.
    bmp[22] = 0x20, bmp[23] = 0x4E, bmp[24] = 0, bmp[25] = 0;
    HX_CHECK(!DecodeImage(Span<const u8>(bmp.Data(), bmp.Size()), texture, error));
    HX_CHECK(StringView(error).Contains(StringView("where the file holds at most")));
    // PSD raw and run length (compression at byte 38): 50000 x 9000 from a few bytes.
    for (u8 compression : {u8(0), u8(1)})
    {
        Array<u8> psd = WritePsd(px, 4, 3, false);
        psd[39] = compression;
        if (compression == 0)
            HX_REQUIRE(DecodeImage(Span<const u8>(psd.Data(), psd.Size()), texture, error));
        psd[14] = 0, psd[15] = 0, psd[16] = 0x23, psd[17] = 0x28;
        psd[18] = 0, psd[19] = 0, psd[20] = 0xC3, psd[21] = 0x50;
        HX_CHECK(!DecodeImage(Span<const u8>(psd.Data(), psd.Size()), texture, error));
        HX_CHECK(StringView(error).Contains(StringView("where the file holds at most")));
    }
    // A section length past the end leaves no room for pixels (stb would read zeros for all of them).
    Array<u8> psd = WritePsd(px, 4, 3, false);
    psd[26] = 0x7F, psd[27] = 0xFF, psd[28] = 0xFF, psd[29] = 0xFF;
    HX_CHECK(!DecodeImage(Span<const u8>(psd.Data(), psd.Size()), texture, error));
    HX_CHECK(StringView(error).Contains(StringView("where the file holds at most")));
}

// A cut file is an error with a reason, or (JPEG and BMP fill what is missing) an image of the size the header gives.
HX_TEST(ImageDecoder, CutFilesAreErrorsOrWhole)
{
    for (const TestImage& t : kTestImages)
    {
        Array<u8> file;
        file.Append(t.file, usize(t.fileSize) / 3);
        TextureData texture;
        String error;
        bool ok = DecodeImage(Span<const u8>(file.Data(), file.Size()), texture, error, t.extension);
        HX_CHECK(ok ? texture.width == t.width && texture.height == t.height && texture.data.Size() == usize(t.width) * t.height * 4 : !error.Empty());
    }
}
