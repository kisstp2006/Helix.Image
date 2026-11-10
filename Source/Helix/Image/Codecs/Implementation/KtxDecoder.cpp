#include <Helix/Image/Codecs/Implementation/BuiltInDecoders.h>
#include <Helix/Image/Codecs/Implementation/ByteReader.h>
#include <Helix/Image/Compression/Deflate.h>

#include <Helix/Core/Strings/Format.h>

#include <cstring>

// Khronos textures: KTX 1 (OpenGL formats, rows padded to 4 bytes) and KTX2 (Vulkan formats, supercompression none
// or zlib). Both store mip, then layer, then face; TextureData wants layer, face, mip.
namespace helix::image
{
    namespace
    {
        using F = PixelFormat;

        struct Mapping
        {
            u32 code;
            PixelFormat format;
            bool srgb;
        };

        constexpr Mapping kGlFormats[] = {
            {0x8229, F::R8, false},          {0x822B, F::RG8, false},         {0x8051, F::RGB8, false},        {0x8C41, F::RGB8, true},
            {0x8058, F::RGBA8, false},       {0x8C43, F::RGBA8, true},        {0x8F94, F::R8Snorm, false},     {0x8F95, F::RG8Snorm, false},
            {0x8F97, F::RGBA8Snorm, false},  {0x822A, F::R16, false},         {0x822C, F::RG16, false},        {0x8054, F::RGB16, false},
            {0x805B, F::RGBA16, false},      {0x822D, F::R16Float, false},    {0x822F, F::RG16Float, false},   {0x881B, F::RGB16Float, false},
            {0x881A, F::RGBA16Float, false}, {0x822E, F::R32Float, false},    {0x8230, F::RG32Float, false},   {0x8815, F::RGB32Float, false},
            {0x8814, F::RGBA32Float, false}, {0x8236, F::R32Uint, false},     {0x8059, F::RGB10A2, false},     {0x8C3A, F::RG11B10Float, false},
            {0x8C3D, F::RGB9E5Float, false}, {0x83F0, F::BC1, false},         {0x83F1, F::BC1, false},         {0x8C4C, F::BC1, true},
            {0x8C4D, F::BC1, true},          {0x83F2, F::BC2, false},         {0x8C4E, F::BC2, true},          {0x83F3, F::BC3, false},
            {0x8C4F, F::BC3, true},          {0x8DBB, F::BC4, false},         {0x8DBC, F::BC4Snorm, false},    {0x8DBD, F::BC5, false},
            {0x8DBE, F::BC5Snorm, false},    {0x8E8F, F::BC6H, false},        {0x8E8E, F::BC6HSigned, false},  {0x8E8C, F::BC7, false},
            {0x8E8D, F::BC7, true},          {0x8D64, F::ETC2RGB, false},     {0x9274, F::ETC2RGB, false},     {0x9275, F::ETC2RGB, true},
            {0x9276, F::ETC2RGBA1, false},   {0x9277, F::ETC2RGBA1, true},    {0x9278, F::ETC2RGBA, false},    {0x9279, F::ETC2RGBA, true},
            {0x9270, F::EACR11, false},      {0x9271, F::EACR11Snorm, false}, {0x9272, F::EACRG11, false},     {0x9273, F::EACRG11Snorm, false},
        };

        constexpr Mapping kVkFormats[] = {
            {4, F::B5G6R5, false},        {8, F::B5G5R5A1, false},     {9, F::R8, false},           {10, F::R8Snorm, false},
            {15, F::R8, true},            {16, F::RG8, false},         {17, F::RG8Snorm, false},    {22, F::RG8, true},
            {23, F::RGB8, false},         {29, F::RGB8, true},         {37, F::RGBA8, false},       {38, F::RGBA8Snorm, false},
            {43, F::RGBA8, true},         {44, F::BGRA8, false},       {50, F::BGRA8, true},        {64, F::RGB10A2, false},
            {70, F::R16, false},          {76, F::R16Float, false},    {77, F::RG16, false},        {83, F::RG16Float, false},
            {84, F::RGB16, false},        {90, F::RGB16Float, false},  {91, F::RGBA16, false},      {97, F::RGBA16Float, false},
            {98, F::R32Uint, false},      {100, F::R32Float, false},   {103, F::RG32Float, false},  {106, F::RGB32Float, false},
            {109, F::RGBA32Float, false}, {122, F::RG11B10Float, false}, {123, F::RGB9E5Float, false}, {131, F::BC1, false},
            {132, F::BC1, true},          {133, F::BC1, false},        {134, F::BC1, true},         {135, F::BC2, false},
            {136, F::BC2, true},          {137, F::BC3, false},        {138, F::BC3, true},         {139, F::BC4, false},
            {140, F::BC4Snorm, false},    {141, F::BC5, false},        {142, F::BC5Snorm, false},   {143, F::BC6H, false},
            {144, F::BC6HSigned, false},  {145, F::BC7, false},        {146, F::BC7, true},         {147, F::ETC2RGB, false},
            {148, F::ETC2RGB, true},      {149, F::ETC2RGBA1, false},  {150, F::ETC2RGBA1, true},   {151, F::ETC2RGBA, false},
            {152, F::ETC2RGBA, true},     {153, F::EACR11, false},     {154, F::EACR11Snorm, false}, {155, F::EACRG11, false},
            {156, F::EACRG11Snorm, false},
        };

        bool Find(const Mapping* table, usize count, u32 code, PixelFormat& format, bool& srgb)
        {
            for (usize i = 0; i < count; i++)
                if (table[i].code == code)
                {
                    format = table[i].format;
                    srgb = table[i].srgb;
                    return true;
                }
            // ASTC: 14 block sizes in order, the OpenGL codes 0x93B0 (sRGB 0x93D0) on, the Vulkan codes 157 on in pairs.
            if (table == kGlFormats && ((code >= 0x93B0 && code <= 0x93BD) || (code >= 0x93D0 && code <= 0x93DD)))
            {
                format = PixelFormat(u32(PixelFormat::ASTC4x4) + (code & 0xF));
                srgb = code >= 0x93D0;
                return true;
            }
            if (table == kVkFormats && code >= 157 && code <= 184)
            {
                format = PixelFormat(u32(PixelFormat::ASTC4x4) + (code - 157) / 2);
                srgb = (code - 157) % 2 == 1;
                return true;
            }
            return false;
        }

        constexpr u8 kKtx1[12] = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n'};
        constexpr u8 kKtx2[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

        bool MatchesKtx1(Span<const u8> data) { return data.Size() >= 64 && std::memcmp(data.Data(), kKtx1, 12) == 0; }
        bool MatchesKtx2(Span<const u8> data) { return data.Size() >= 80 && std::memcmp(data.Data(), kKtx2, 12) == 0; }

        void Finish(TextureData& out, bool srgb, bool cube)
        {
            out.colorSpace = srgb ? ColorSpace::Srgb : ColorSpace::Unspecified;
            out.dimension = cube ? TextureDimension::Cube : out.depth > 1 ? TextureDimension::Texture3D : TextureDimension::Texture2D;
        }

        bool DecodeKtx1(Span<const u8> data, TextureData& out, String& error)
        {
            ByteReader r(data);
            r.Skip(12);
            u32 endian = r.U32();
            if (endian == 0x01020304)
                r.bigEndian = true;
            else if (endian != 0x04030201)
                return error = "an unknown byte order", false;
            u32 glType = r.U32(), typeSize = r.U32();
            r.Skip(4);
            u32 internalFormat = r.U32();
            r.Skip(4);
            u32 width = r.U32(), height = r.U32(), depth = r.U32(), layers = r.U32(), faces = r.U32(), mips = r.U32(), keyValues = r.U32();
            (void)glType;
            if (!r.ok || !r.Skip(keyValues))
                return error = "the header is cut", false;
            PixelFormat format = PixelFormat::Unknown;
            bool srgb = false;
            if (!Find(kGlFormats, sizeof(kGlFormats) / sizeof(kGlFormats[0]), internalFormat, format, srgb))
                return error = Format("OpenGL format {} is not supported", internalFormat), false;
            bool arrayed = layers > 0;
            layers = layers ? layers : 1;
            height = height ? height : 1;
            depth = depth ? depth : 1;
            mips = mips ? mips : 1;
            if ((faces != 1 && faces != 6) || !out.Allocate(format, width, height, depth, layers, faces, mips))
                return error = "the texture is empty or too large", false;
            Finish(out, srgb, faces == 6);
            const PixelFormatInfo& info = FormatInfo(format);
            for (u32 mip = 0; mip < mips; mip++)
            {
                u32 imageSize = r.U32();
                usize start = r.at;
                (void)imageSize;
                for (u32 layer = 0; layer < layers; layer++)
                    for (u32 face = 0; face < faces; face++)
                    {
                        const Subresource& s = out.At(layer, face, mip);
                        u8* dst = out.data.Data() + s.offset;
                        if (info.kind == PixelKind::Block)
                        {
                            Span<const u8> bytes = r.Bytes(usize(s.size));
                            if (!r.ok)
                                return error = "the file is shorter than its images", false;
                            std::memcpy(dst, bytes.Data(), bytes.Size());
                        }
                        else
                        {
                            // Uncompressed rows are padded to 4 bytes.
                            usize row = usize(s.width) * info.bytesPerBlock, pitch = (row + 3) & ~usize(3);
                            for (u32 y = 0; y < s.height * s.depth; y++)
                            {
                                Span<const u8> bytes = r.Bytes(pitch);
                                if (!r.ok)
                                    return error = "the file is shorter than its images", false;
                                std::memcpy(dst + usize(y) * row, bytes.Data(), row);
                            }
                        }
                        if (faces == 6 && !arrayed)
                            r.Skip((4 - (r.at - start) % 4) % 4);
                    }
                r.Skip((4 - (r.at - start) % 4) % 4);
                r.ok = true;
                if (r.at > r.size)
                    return error = "the file is shorter than its images", false;
            }
            if (r.bigEndian && typeSize > 1)
            {
                // Samples of 2 or 4 bytes are in the byte order of the file.
                for (usize i = 0; i + typeSize <= out.data.Size(); i += typeSize)
                    for (u32 b = 0; b < typeSize / 2; b++)
                    {
                        u8 t = out.data[i + b];
                        out.data[i + b] = out.data[i + typeSize - 1 - b];
                        out.data[i + typeSize - 1 - b] = t;
                    }
            }
            return true;
        }

        bool DecodeKtx2(Span<const u8> data, TextureData& out, String& error)
        {
            ByteReader r(data);
            r.Skip(12);
            u32 vkFormat = r.U32();
            r.Skip(4);
            u32 width = r.U32(), height = r.U32(), depth = r.U32(), layers = r.U32(), faces = r.U32(), mips = r.U32(), scheme = r.U32();
            r.Skip(16);
            r.Skip(16);
            if (!r.ok)
                return error = "the header is cut", false;
            if (scheme == 1 || vkFormat == 0)
                return error = "Basis Universal textures are not supported", false;
            if (scheme == 2)
                return error = "Zstandard supercompression is not supported", false;
            if (scheme != 0 && scheme != 3)
                return error = Format("supercompression scheme {} is not supported", scheme), false;
            PixelFormat format = PixelFormat::Unknown;
            bool srgb = false;
            if (!Find(kVkFormats, sizeof(kVkFormats) / sizeof(kVkFormats[0]), vkFormat, format, srgb))
                return error = Format("Vulkan format {} is not supported", vkFormat), false;
            layers = layers ? layers : 1;
            height = height ? height : 1;
            depth = depth ? depth : 1;
            mips = mips ? mips : 1;
            if ((faces != 1 && faces != 6) || !out.Allocate(format, width, height, depth, layers, faces, mips))
                return error = "the texture is empty or too large", false;
            Finish(out, srgb, faces == 6);
            Array<u8> inflated;
            for (u32 mip = 0; mip < mips; mip++)
            {
                u64 offset = r.U64(), length = r.U64(), uncompressed = r.U64();
                if (!r.ok || offset > data.Size() || length > data.Size() - offset)
                    return error = "a level is outside the file", false;
                Span<const u8> level(data.Data() + offset, usize(length));
                if (scheme == 3)
                {
                    inflated.Clear();
                    if (!ZlibDecompress(level, inflated) || inflated.Size() != uncompressed)
                        return error = "a level does not inflate to its size", false;
                    level = Span<const u8>(inflated.Data(), inflated.Size());
                }
                usize at = 0;
                for (u32 layer = 0; layer < layers; layer++)
                    for (u32 face = 0; face < faces; face++)
                    {
                        const Subresource& s = out.At(layer, face, mip);
                        if (at + s.size > level.Size())
                            return error = "a level is shorter than its images", false;
                        std::memcpy(out.data.Data() + s.offset, level.Data() + at, usize(s.size));
                        at += usize(s.size);
                    }
            }
            return true;
        }
    }

    const ImageDecoder& Ktx1ImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"ktx"};
        static const ImageDecoder kDecoder{"KTX", Span<const StringView>(kExtensions, 1), &MatchesKtx1, &DecodeKtx1};
        return kDecoder;
    }

    const ImageDecoder& Ktx2ImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"ktx2"};
        static const ImageDecoder kDecoder{"KTX2", Span<const StringView>(kExtensions, 1), &MatchesKtx2, &DecodeKtx2};
        return kDecoder;
    }
}
