#include <Helix/Image/Codecs/Implementation/BuiltInDecoders.h>
#include <Helix/Image/Codecs/Implementation/ByteReader.h>

#include <Helix/Core/Strings/Format.h>

#include <cstring>

// PowerVR textures, version 3: block formats by their predefined code, uncompressed formats by channel names and
// sizes. The file stores mip, then surface, then face.
namespace helix::image
{
    namespace
    {
        using F = PixelFormat;

        constexpr u32 kVersion = 0x03525650;

        struct Predefined
        {
            u32 code;
            PixelFormat format;
        };

        constexpr Predefined kPredefined[] = {
            {6, F::ETC2RGB},     {7, F::BC1},           {8, F::BC2},       {9, F::BC2},     {10, F::BC3},      {11, F::BC3},
            {12, F::BC4},        {13, F::BC5},          {14, F::BC6H},     {15, F::BC7},    {22, F::ETC2RGB},  {23, F::ETC2RGBA},
            {24, F::ETC2RGBA1},  {25, F::EACR11},       {26, F::EACRG11},
        };

        struct Uncompressed
        {
            const char* channels;
            u8 bits;
            // 0 unsigned normalized, 12 / 13 float.
            u8 kind;
            PixelFormat format;
        };

        constexpr Uncompressed kUncompressed[] = {
            {"r", 8, 0, F::R8},           {"rg", 8, 0, F::RG8},          {"rgb", 8, 0, F::RGB8},        {"rgba", 8, 0, F::RGBA8},
            {"bgra", 8, 0, F::BGRA8},     {"r", 16, 4, F::R16},          {"rg", 16, 4, F::RG16},        {"rgba", 16, 4, F::RGBA16},
            {"r", 16, 12, F::R16Float},   {"rg", 16, 12, F::RG16Float},  {"rgba", 16, 12, F::RGBA16Float}, {"r", 32, 12, F::R32Float},
            {"rg", 32, 12, F::RG32Float}, {"rgb", 32, 12, F::RGB32Float}, {"rgba", 32, 12, F::RGBA32Float},
        };

        bool MatchesPvr(Span<const u8> data) { return data.Size() >= 52 && data[0] == 'P' && data[1] == 'V' && data[2] == 'R' && data[3] == 3; }

        bool DecodePvr(Span<const u8> data, TextureData& out, String& error)
        {
            ByteReader r(data);
            u32 version = r.U32();
            r.Skip(4);
            u32 low = r.U32(), high = r.U32(), colorSpace = r.U32(), channelType = r.U32();
            u32 height = r.U32(), width = r.U32(), depth = r.U32(), surfaces = r.U32(), faces = r.U32(), mips = r.U32(), meta = r.U32();
            if (!r.ok || version != kVersion || !r.Skip(meta))
                return error = "the header is damaged", false;
            PixelFormat format = PixelFormat::Unknown;
            if (high == 0)
            {
                for (const Predefined& p : kPredefined)
                    if (p.code == low)
                        format = p.format;
                if (low >= 27 && low <= 40)
                    format = PixelFormat(u32(PixelFormat::ASTC4x4) + (low - 27));
                if (low <= 5)
                    return error = "PVRTC textures are not supported", false;
            }
            else
            {
                char names[5] = {char(low), char(low >> 8), char(low >> 16), char(low >> 24), 0};
                u32 bits[4] = {high & 0xFF, (high >> 8) & 0xFF, (high >> 16) & 0xFF, high >> 24};
                for (const Uncompressed& u : kUncompressed)
                {
                    usize n = std::strlen(u.channels);
                    bool same = std::strncmp(names, u.channels, 4) == 0 || (std::strncmp(names, u.channels, n) == 0 && (n == 4 || names[n] == 0));
                    bool sizes = true;
                    for (usize c = 0; c < 4; c++)
                        sizes = sizes && bits[c] == (c < n ? u.bits : 0u);
                    bool kind = u.kind == 0 ? channelType == 0 || channelType == 2 : u.kind == 4 ? channelType == 4 || channelType == 6 : channelType == 12 || channelType == 13;
                    if (same && sizes && kind)
                        format = u.format;
                }
            }
            if (format == PixelFormat::Unknown)
                return error = Format("pixel format {} {} is not supported", high, low), false;
            surfaces = surfaces ? surfaces : 1;
            faces = faces ? faces : 1;
            depth = depth ? depth : 1;
            mips = mips ? mips : 1;
            if ((faces != 1 && faces != 6) || !out.Allocate(format, width, height, depth, surfaces, faces, mips))
                return error = "the texture is empty or too large", false;
            out.colorSpace = colorSpace == 1 ? ColorSpace::Srgb : ColorSpace::Unspecified;
            out.dimension = faces == 6 ? TextureDimension::Cube : depth > 1 ? TextureDimension::Texture3D : TextureDimension::Texture2D;
            for (u32 mip = 0; mip < mips; mip++)
                for (u32 layer = 0; layer < surfaces; layer++)
                    for (u32 face = 0; face < faces; face++)
                    {
                        const Subresource& s = out.At(layer, face, mip);
                        Span<const u8> bytes = r.Bytes(usize(s.size));
                        if (!r.ok)
                            return error = "the file is shorter than its surfaces", false;
                        std::memcpy(out.data.Data() + s.offset, bytes.Data(), bytes.Size());
                    }
            return true;
        }
    }

    const ImageDecoder& PvrImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"pvr"};
        static const ImageDecoder kDecoder{"PVR", Span<const StringView>(kExtensions, 1), &MatchesPvr, &DecodePvr};
        return kDecoder;
    }
}
