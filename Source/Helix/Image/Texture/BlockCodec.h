#pragma once

#include <Helix/Core/Strings/String.h>
#include <Helix/Image/Texture/TextureData.h>

namespace helix::image
{
    enum class CompressionQuality : u8
    {
        Fast,
        Normal,
        Best,
    };

    // A GPU block format: how one block turns into texels and back. Texels are `decodedFormat`, blockWidth x blockHeight
    // of them, rows top first.
    struct BlockCodec
    {
        PixelFormat format = PixelFormat::Unknown;
        PixelFormat decodedFormat = PixelFormat::Unknown;
        void (*decode)(const u8* block, u8* texels) = nullptr;
        // Null while the format has no encoder.
        void (*encode)(const u8* texels, u8* block, CompressionQuality quality) = nullptr;
    };

    // Adds a codec, or replaces the one of the same format. The built in codecs are registered on first use.
    void RegisterBlockCodec(const BlockCodec& codec);
    bool FindBlockCodec(PixelFormat format, BlockCodec& out);

    // Every layer, face and mip of a block compressed texture as its decoded format.
    bool DecompressTexture(const TextureData& source, TextureData& out, String& error);
    // Every layer, face and mip of an uncompressed texture in a block format; the texels are converted to the codec's
    // decoded format first, edge blocks repeat the last row and column. Block rows run as jobs with the same bytes.
    bool CompressTexture(const TextureData& source, PixelFormat format, CompressionQuality quality, TextureData& out, String& error);
}
