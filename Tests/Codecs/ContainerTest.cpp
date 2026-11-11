#include <Support/ContainerWriters.h>

#include <Helix/Image/Codecs/ImageDecoder.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::image;
using namespace helix::image::testing;

namespace
{
    TextureData Make(PixelFormat format, u32 w, u32 h, u32 d, u32 layers, u32 faces, u32 mips)
    {
        TextureData t;
        t.Allocate(format, w, h, d, layers, faces, mips);
        FillPattern(t);
        return t;
    }

    // The file decodes to the same shape and bytes.
    bool Same(const char* name, const Array<u8>& file, const TextureData& expected, ColorSpace colorSpace = ColorSpace::Unspecified)
    {
        TextureData got;
        String error;
        if (!DecodeImage(Span<const u8>(file.Data(), file.Size()), got, error))
        {
            std::printf("    %s: %s\n", name, error.CStr());
            return false;
        }
        bool ok = got.format == expected.format && got.width == expected.width && got.height == expected.height && got.depth == expected.depth &&
                  got.layers == expected.layers && got.faces == expected.faces && got.mips == expected.mips && got.data.Size() == expected.data.Size() &&
                  std::memcmp(got.data.Data(), expected.data.Data(), got.data.Size()) == 0 && got.colorSpace == colorSpace &&
                  (expected.faces != 6 || got.dimension == TextureDimension::Cube);
        if (!ok)
            std::printf("    %s decodes to the wrong texture\n", name);
        return ok;
    }
}

HX_TEST(Dds, ReadsFourCCMasksAndDx10)
{
    TextureData bc1 = Make(PixelFormat::BC1, 37, 21, 1, 1, 1, 6);
    HX_CHECK(Same("DXT1 with mips", WriteDds(bc1, u32('D') | u32('X') << 8 | u32('T') << 16 | u32('1') << 24, 0), bc1));
    TextureData cube = Make(PixelFormat::BC3, 16, 16, 1, 1, 6, 5);
    HX_CHECK(Same("DXT5 cube", WriteDds(cube, u32('D') | u32('X') << 8 | u32('T') << 16 | u32('5') << 24, 0), cube));
    TextureData half = Make(PixelFormat::RGBA16Float, 5, 3, 1, 1, 1, 1);
    HX_CHECK(Same("D3DFMT A16B16G16R16F", WriteDds(half, 113, 0), half));
    TextureData rgba = Make(PixelFormat::RGBA8, 7, 5, 1, 1, 1, 3);
    const u32 rgbaMasks[5] = {32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000};
    HX_CHECK(Same("RGBA masks", WriteDds(rgba, 0, 0, rgbaMasks), rgba));
    TextureData bgra = Make(PixelFormat::BGRA8, 7, 5, 1, 1, 1, 1);
    const u32 bgraMasks[5] = {32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000};
    HX_CHECK(Same("BGRA masks", WriteDds(bgra, 0, 0, bgraMasks), bgra));
    TextureData r565 = Make(PixelFormat::B5G6R5, 6, 6, 1, 1, 1, 1);
    const u32 masks565[5] = {16, 0xF800, 0x7E0, 0x1F, 0};
    HX_CHECK(Same("565 masks", WriteDds(r565, 0, 0, masks565), r565));
    TextureData volume = Make(PixelFormat::RGBA8, 8, 4, 4, 1, 1, 3);
    HX_CHECK(Same("volume", WriteDds(volume, 0, 0, rgbaMasks), volume));
    TextureData bc7 = Make(PixelFormat::BC7, 33, 17, 1, 3, 1, 4);
    HX_CHECK(Same("DX10 BC7 sRGB array", WriteDds(bc7, 0, 99), bc7, ColorSpace::Srgb));
    TextureData cubes = Make(PixelFormat::BC6H, 8, 8, 1, 2, 6, 2);
    HX_CHECK(Same("DX10 BC6H cube array", WriteDds(cubes, 0, 95), cubes));

    // BGR in 24 bits becomes RGB8.
    TextureData rgb = Make(PixelFormat::RGB8, 5, 2, 1, 1, 1, 1);
    TextureData bgr = rgb;
    for (usize i = 0; i < bgr.data.Size(); i += 3)
    {
        bgr.data[i] = rgb.data[i + 2];
        bgr.data[i + 2] = rgb.data[i];
    }
    const u32 bgrMasks[5] = {24, 0xFF0000, 0xFF00, 0xFF, 0};
    HX_CHECK(Same("24 bit BGR", WriteDds(bgr, 0, 0, bgrMasks), rgb));

    TextureData texture;
    String error;
    Array<u8> unknown = WriteDds(bc1, 0, 130);
    HX_CHECK(!DecodeImage(Span<const u8>(unknown.Data(), unknown.Size()), texture, error) && error == StringView("DDS: DXGI format 130 is not supported"));
}

// L8, A8L8 and L16 by the luminance flag, whatever the masks say (Pillow writes 0xFF000000 for L8), widened to gray
// RGBA with every mip.
HX_TEST(Dds, ReadsLuminance)
{
    struct Case
    {
        PixelFormat stored;
        u32 masks[5];
        u32 alphaFlag;
        PixelFormat decoded;
    };
    const Case cases[] = {
        {PixelFormat::R8, {8, 0xFF000000, 0xFF000000, 0xFF000000, 0}, 0, PixelFormat::RGBA8},
        {PixelFormat::RG8, {16, 0xFF, 0, 0, 0xFF00}, 1, PixelFormat::RGBA8},
        {PixelFormat::R16, {16, 0xFFFF, 0, 0, 0}, 0, PixelFormat::RGBA16},
    };
    for (const Case& c : cases)
    {
        TextureData stored = Make(c.stored, 9, 6, 1, 1, 1, 3);
        Array<u8> file = WriteDds(stored, 0, 0, c.masks);
        // The pixel format flags: luminance, and alpha pixels for A8L8.
        u32 flags = 0x20000 | c.alphaFlag;
        for (u32 b = 0; b < 4; b++)
            file[80 + b] = u8(flags >> (8 * b));
        TextureData texture;
        String error;
        HX_REQUIRE(DecodeImage(Span<const u8>(file.Data(), file.Size()), texture, error));
        HX_CHECK(texture.format == c.decoded && texture.mips == 3);
        bool same = true;
        for (u32 mip = 0; mip < 3; mip++)
        {
            const Subresource& s = stored.At(0, 0, mip);
            const u8* src = stored.data.Data() + s.offset;
            const u8* dst = texture.Pixels(0, 0, mip);
            for (usize i = 0; i < usize(s.width) * s.height; i++)
                if (c.stored == PixelFormat::R16)
                    same = same && dst[i * 8] == src[i * 2] && dst[i * 8 + 3] == src[i * 2 + 1] && dst[i * 8 + 5] == src[i * 2 + 1] && dst[i * 8 + 6] == 0xFF;
                else
                {
                    u8 l = c.stored == PixelFormat::RG8 ? src[i * 2] : src[i], a = c.stored == PixelFormat::RG8 ? src[i * 2 + 1] : 0xFF;
                    same = same && dst[i * 4] == l && dst[i * 4 + 1] == l && dst[i * 4 + 2] == l && dst[i * 4 + 3] == a;
                }
        }
        HX_CHECK(same);
    }
}

HX_TEST(Ktx, ReadsKtx1WithRowPaddingCubesAndArrays)
{
    TextureData rgb = Make(PixelFormat::RGB8, 7, 5, 1, 1, 1, 3);
    HX_CHECK(Same("RGB8 padded rows", WriteKtx1(rgb, 0x8051, 1, false), rgb));
    TextureData srgb = Make(PixelFormat::RGBA8, 9, 3, 1, 1, 1, 4);
    HX_CHECK(Same("SRGB8_ALPHA8", WriteKtx1(srgb, 0x8C43, 1, false), srgb, ColorSpace::Srgb));
    TextureData cube = Make(PixelFormat::R8, 3, 3, 1, 1, 6, 2);
    HX_CHECK(Same("R8 cube with face padding", WriteKtx1(cube, 0x8229, 1, false), cube));
    TextureData etc = Make(PixelFormat::ETC2RGBA, 12, 8, 1, 2, 1, 4);
    HX_CHECK(Same("ETC2 array", WriteKtx1(etc, 0x9278, 1, true), etc));
    TextureData astc = Make(PixelFormat::ASTC6x5, 13, 11, 1, 1, 1, 1);
    HX_CHECK(Same("ASTC 6x5 sRGB", WriteKtx1(astc, 0x93D3, 1, false), astc, ColorSpace::Srgb));
    TextureData floats = Make(PixelFormat::RGBA32Float, 3, 2, 1, 1, 1, 2);
    HX_CHECK(Same("RGBA32F", WriteKtx1(floats, 0x8814, 4, false), floats));
}

HX_TEST(Ktx, ReadsKtx2LevelsAndZlib)
{
    TextureData bc5 = Make(PixelFormat::BC5, 20, 12, 1, 1, 1, 5);
    HX_CHECK(Same("BC5", WriteKtx2(bc5, 141, 0), bc5));
    TextureData zipped = Make(PixelFormat::RGBA16Float, 9, 7, 1, 3, 1, 4);
    HX_CHECK(Same("RGBA16F array zlib", WriteKtx2(zipped, 97, 3), zipped));
    TextureData cube = Make(PixelFormat::ASTC8x8, 32, 32, 1, 1, 6, 3);
    HX_CHECK(Same("ASTC 8x8 sRGB cube", WriteKtx2(cube, 172, 0), cube, ColorSpace::Srgb));
    TextureData volume = Make(PixelFormat::R8, 4, 4, 8, 1, 1, 2);
    HX_CHECK(Same("R8 volume", WriteKtx2(volume, 9, 0), volume));

    TextureData texture;
    String error;
    Array<u8> zstd = WriteKtx2(bc5, 141, 2);
    HX_CHECK(!DecodeImage(Span<const u8>(zstd.Data(), zstd.Size()), texture, error) && error == StringView("KTX2: Zstandard supercompression is not supported"));
    Array<u8> basis = WriteKtx2(bc5, 0, 0);
    HX_CHECK(!DecodeImage(Span<const u8>(basis.Data(), basis.Size()), texture, error) && error == StringView("KTX2: Basis Universal textures are not supported"));
}

HX_TEST(Pvr, ReadsPredefinedAndChannelFormats)
{
    TextureData etc = Make(PixelFormat::ETC2RGB, 16, 8, 1, 1, 1, 5);
    HX_CHECK(Same("ETC2 RGB", WritePvr(etc, 22, 0, 0, 0), etc));
    TextureData astc = Make(PixelFormat::ASTC10x10, 30, 20, 1, 2, 1, 2);
    HX_CHECK(Same("ASTC 10x10 sRGB array", WritePvr(astc, 38, 0, 0, 1), astc, ColorSpace::Srgb));
    TextureData rgba = Make(PixelFormat::RGBA8, 6, 3, 1, 1, 6, 1);
    u32 names = u32('r') | u32('g') << 8 | u32('b') << 16 | u32('a') << 24;
    HX_CHECK(Same("rgba8888 cube", WritePvr(rgba, names, 0x08080808, 0, 0), rgba));
    TextureData half = Make(PixelFormat::RG16Float, 4, 4, 1, 1, 1, 3);
    HX_CHECK(Same("rg16 float", WritePvr(half, u32('r') | u32('g') << 8, 0x00001010, 12, 0), half));

    TextureData texture;
    String error;
    Array<u8> pvrtc = WritePvr(etc, 2, 0, 0, 0);
    HX_CHECK(!DecodeImage(Span<const u8>(pvrtc.Data(), pvrtc.Size()), texture, error) && error == StringView("PVR: PVRTC textures are not supported"));
}

HX_TEST(Containers, CutFilesAreErrors)
{
    TextureData bc1 = Make(PixelFormat::BC1, 16, 16, 1, 1, 1, 5);
    Array<Array<u8>> files;
    files.Push(WriteDds(bc1, u32('D') | u32('X') << 8 | u32('T') << 16 | u32('1') << 24, 0));
    files.Push(WriteKtx1(bc1, 0x83F1, 1, false));
    files.Push(WriteKtx2(bc1, 133, 3));
    files.Push(WritePvr(bc1, 7, 0, 0, 0));
    for (const Array<u8>& file : files)
    {
        Array<u8> part;
        part.Append(file.Data(), file.Size() - 9);
        TextureData texture;
        String error;
        HX_CHECK(!DecodeImage(Span<const u8>(part.Data(), part.Size()), texture, error) && !error.Empty());
    }
}
