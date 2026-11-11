#include <Helix/Image/Codecs/Implementation/BuiltInDecoders.h>
#include <Helix/Image/Codecs/Implementation/ByteReader.h>

#include <Helix/Core/Strings/Format.h>

#include <cstring>

// DirectDraw Surface: the legacy header (FourCC codes and pixel masks) and the DX10 header; 2D, cube, array and volume
// textures with their mips. The file order (element, face, mip) is the order of TextureData.
namespace helix::image
{
    namespace
    {
        using F = PixelFormat;

        constexpr u32 Code(const char* c) { return u32(u8(c[0])) | (u32(u8(c[1])) << 8) | (u32(u8(c[2])) << 16) | (u32(u8(c[3])) << 24); }

        struct DxgiFormat
        {
            u32 code;
            PixelFormat format;
            bool srgb;
        };

        constexpr DxgiFormat kDxgi[] = {
            {2, F::RGBA32Float, false}, {6, F::RGB32Float, false}, {9, F::RGBA16, false},      {10, F::RGBA16Float, false},
            {11, F::RGBA16, false},     {16, F::RG32Float, false}, {24, F::RGB10A2, false},     {26, F::RG11B10Float, false},
            {27, F::RGBA8, false},      {28, F::RGBA8, false},     {29, F::RGBA8, true},        {31, F::RGBA8Snorm, false},
            {34, F::RG16Float, false},  {35, F::RG16, false},      {41, F::R32Float, false},    {42, F::R32Uint, false},
            {49, F::RG8, false},        {51, F::RG8Snorm, false},  {54, F::R16Float, false},    {56, F::R16, false},
            {61, F::R8, false},         {63, F::R8Snorm, false},   {67, F::RGB9E5Float, false}, {70, F::BC1, false},
            {71, F::BC1, false},        {72, F::BC1, true},        {73, F::BC2, false},         {74, F::BC2, false},
            {75, F::BC2, true},         {76, F::BC3, false},       {77, F::BC3, false},         {78, F::BC3, true},
            {79, F::BC4, false},        {80, F::BC4, false},       {81, F::BC4Snorm, false},    {82, F::BC5, false},
            {83, F::BC5, false},        {84, F::BC5Snorm, false},  {85, F::B5G6R5, false},      {86, F::B5G5R5A1, false},
            {87, F::BGRA8, false},      {88, F::BGRA8, false},     {90, F::BGRA8, false},       {91, F::BGRA8, true},
            {92, F::BGRA8, false},      {93, F::BGRA8, true},      {94, F::BC6H, false},        {95, F::BC6H, false},
            {96, F::BC6HSigned, false}, {97, F::BC7, false},       {98, F::BC7, false},         {99, F::BC7, true},
            {115, F::B4G4R4A4, false},
        };

        struct FourCC
        {
            u32 code;
            PixelFormat format;
        };

        constexpr FourCC kFourCC[] = {
            {Code("DXT1"), F::BC1}, {Code("DXT2"), F::BC2}, {Code("DXT3"), F::BC2}, {Code("DXT4"), F::BC3},
            {Code("DXT5"), F::BC3}, {Code("ATI1"), F::BC4}, {Code("BC4U"), F::BC4}, {Code("BC4S"), F::BC4Snorm},
            {Code("ATI2"), F::BC5}, {Code("BC5U"), F::BC5}, {Code("BC5S"), F::BC5Snorm}, {36, F::RGBA16},
            {111, F::R16Float},     {112, F::RG16Float},    {113, F::RGBA16Float}, {114, F::R32Float},
            {115, F::RG32Float},    {116, F::RGBA32Float},
        };

        struct Masks
        {
            u32 bits, r, g, b, a;
            PixelFormat format;
            // 24 bit BGR is stored as RGB8 with the channels swapped.
            bool swapRgb;
        };

        constexpr Masks kMasks[] = {
            {32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000, F::RGBA8, false},
            {32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000, F::BGRA8, false},
            {32, 0xFF0000, 0xFF00, 0xFF, 0, F::BGRA8, false},
            {32, 0x3FF, 0xFFC00, 0x3FF00000, 0xC0000000, F::RGB10A2, false},
            {32, 0xFFFF, 0xFFFF0000, 0, 0, F::RG16, false},
            {24, 0xFF0000, 0xFF00, 0xFF, 0, F::RGB8, true},
            {24, 0xFF, 0xFF00, 0xFF0000, 0, F::RGB8, false},
            {16, 0xF800, 0x7E0, 0x1F, 0, F::B5G6R5, false},
            {16, 0x7C00, 0x3E0, 0x1F, 0x8000, F::B5G5R5A1, false},
            {16, 0xF00, 0xF0, 0xF, 0xF000, F::B4G4R4A4, false},
            {16, 0xFF, 0, 0, 0xFF00, F::RG8, false},
            {16, 0xFFFF, 0, 0, 0, F::R16, false},
            {8, 0xFF, 0, 0, 0, F::R8, false},
            {8, 0, 0, 0, 0xFF, F::R8, false},
        };

        constexpr u32 kFourCCFlag = 0x4, kLuminance = 0x20000, kCubemap = 0x200, kAllFaces = 0xFC00, kVolume = 0x200000;

        // Legacy luminance layouts (L8, A8L8, L16), known by their flag and size as DirectXTex does (writers disagree on
        // the masks): stored as R8, RG8 or R16 and widened to gray RGBA, as they sample.
        PixelFormat Luminance(u32 bits, u32 pfFlags)
        {
            if (bits == 8 && !(pfFlags & 1))
                return PixelFormat::R8;
            if (bits == 16 && (pfFlags & 1))
                return PixelFormat::RG8;
            if (bits == 16)
                return PixelFormat::R16;
            return PixelFormat::Unknown;
        }

        void WidenLuminance(const TextureData& stored, TextureData& out)
        {
            for (usize s = 0; s < stored.subresources.Size(); s++)
            {
                const Subresource& from = stored.subresources[s];
                const u8* src = stored.data.Data() + from.offset;
                u8* dst = out.data.Data() + out.subresources[s].offset;
                usize n = usize(from.width) * from.height * from.depth;
                for (usize i = 0; i < n; i++)
                    if (stored.format == PixelFormat::R16)
                        for (u32 c = 0; c < 4; c++)
                            dst[i * 8 + c * 2] = c == 3 ? 0xFF : src[i * 2], dst[i * 8 + c * 2 + 1] = c == 3 ? 0xFF : src[i * 2 + 1];
                    else
                    {
                        u8 l = stored.format == PixelFormat::RG8 ? src[i * 2] : src[i];
                        u8 a = stored.format == PixelFormat::RG8 ? src[i * 2 + 1] : 0xFF;
                        dst[i * 4] = dst[i * 4 + 1] = dst[i * 4 + 2] = l, dst[i * 4 + 3] = a;
                    }
            }
        }

        bool MatchesDds(Span<const u8> data) { return data.Size() >= 128 && std::memcmp(data.Data(), "DDS ", 4) == 0; }

        bool DecodeDds(Span<const u8> data, TextureData& out, String& error)
        {
            ByteReader r(data);
            r.Skip(4);
            u32 size = r.U32(), flags = r.U32(), height = r.U32(), width = r.U32();
            r.Skip(4);
            u32 depth = r.U32(), mips = r.U32();
            r.Skip(44);
            u32 pfSize = r.U32(), pfFlags = r.U32(), fourCC = r.U32(), bits = r.U32();
            u32 rm = r.U32(), gm = r.U32(), bm = r.U32(), am = r.U32();
            r.Skip(4);
            u32 caps2 = r.U32();
            r.Skip(12);
            (void)flags;
            if (!r.ok || size != 124 || pfSize != 32)
                return error = "the header is damaged", false;
            PixelFormat format = PixelFormat::Unknown;
            bool srgb = false, swapRgb = false, cube = false;
            u32 layers = 1;
            if ((pfFlags & kFourCCFlag) && fourCC == Code("DX10"))
            {
                u32 dxgi = r.U32(), dimension = r.U32(), misc = r.U32(), arraySize = r.U32();
                r.Skip(4);
                if (!r.ok)
                    return error = "the DX10 header is cut", false;
                for (const DxgiFormat& d : kDxgi)
                    if (d.code == dxgi)
                    {
                        format = d.format;
                        srgb = d.srgb;
                    }
                if (format == PixelFormat::Unknown)
                    return error = Format("DXGI format {} is not supported", dxgi), false;
                cube = (misc & 4) != 0;
                layers = arraySize ? arraySize : 1;
                if (dimension != 4)
                    depth = 1;
            }
            else if (pfFlags & kFourCCFlag)
            {
                for (const FourCC& f : kFourCC)
                    if (f.code == fourCC)
                        format = f.format;
                if (format == PixelFormat::Unknown)
                    return error = Format("FourCC {} is not supported", fourCC), false;
            }
            else if (pfFlags & kLuminance)
            {
                format = Luminance(bits, pfFlags);
                if (format == PixelFormat::Unknown)
                    return error = Format("a {} bit luminance layout that is not supported", bits), false;
            }
            else
            {
                for (const Masks& m : kMasks)
                    if (m.bits == bits && m.r == rm && m.g == gm && m.b == bm && (m.a == am || ((pfFlags & 1) == 0 && m.a == 0)))
                    {
                        format = m.format;
                        swapRgb = m.swapRgb;
                        break;
                    }
                if (format == PixelFormat::Unknown)
                    return error = Format("a {} bit pixel layout that is not supported", bits), false;
            }
            if (caps2 & kCubemap)
            {
                if ((caps2 & kAllFaces) != kAllFaces)
                    return error = "a cube map without all six faces", false;
                cube = true;
            }
            if (!(caps2 & kVolume) && layers == 1 && !(fourCC == Code("DX10")))
                depth = 1;
            depth = depth ? depth : 1;
            mips = mips ? mips : 1;
            bool luminance = !(pfFlags & kFourCCFlag) && (pfFlags & kLuminance);
            TextureData stored;
            TextureData& target = luminance ? stored : out;
            if (!target.Allocate(format, width, height, depth, layers, cube ? 6 : 1, mips))
                return error = "the texture is empty or too large", false;
            Span<const u8> bytes = r.Bytes(target.data.Size());
            if (!r.ok)
                return error = "the file is shorter than its surfaces", false;
            std::memcpy(target.data.Data(), bytes.Data(), bytes.Size());
            if (luminance)
            {
                if (!out.Allocate(format == PixelFormat::R16 ? PixelFormat::RGBA16 : PixelFormat::RGBA8, width, height, depth, layers, cube ? 6 : 1, mips))
                    return error = "the texture is empty or too large", false;
                WidenLuminance(stored, out);
            }
            out.dimension = cube ? TextureDimension::Cube : depth > 1 ? TextureDimension::Texture3D : TextureDimension::Texture2D;
            out.colorSpace = srgb ? ColorSpace::Srgb : ColorSpace::Unspecified;
            if (swapRgb)
                for (usize i = 0; i + 2 < out.data.Size(); i += 3)
                {
                    u8 t = out.data[i];
                    out.data[i] = out.data[i + 2];
                    out.data[i + 2] = t;
                }
            return true;
        }
    }

    const ImageDecoder& DdsImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"dds"};
        static const ImageDecoder kDecoder{"DDS", Span<const StringView>(kExtensions, 1), &MatchesDds, &DecodeDds};
        return kDecoder;
    }
}
