#pragma once

#include <Helix/Core/Strings/String.h>
#include <Helix/Image/Texture/TextureData.h>

namespace helix::image
{
    // The flags of EncodeToEXR.
    enum ExrFlags : u32
    {
        ExrNone = 0,
        ExrOutputAsFloat = 1,
        ExrCompressZip = 2,
        ExrCompressRle = 4,
        ExrCompressPiz = 8,
    };

    struct EncodeOptions
    {
        // JPG quality, 1 to 100.
        u32 quality = 75;
        u32 exrFlags = ExrNone;
    };

    // A file format that can be written. Encoders write the first subresource (layer 0, face 0, mip 0, depth slice 0) of
    // an uncompressed texture.
    struct ImageEncoder
    {
        StringView name;
        // Lower case, without the dot. The array must outlive the registration.
        Span<const StringView> extensions;
        bool (*encode)(const TextureData& texture, const EncodeOptions& options, Array<u8>& out, String& error) = nullptr;
    };

    // Adds an encoder, or replaces the one with the same name. The built in encoders are registered on first use.
    void RegisterImageEncoder(const ImageEncoder& encoder);
    void ImageEncoders(Array<ImageEncoder>& out);
    // By name ("PNG") or extension ("png" or ".png"), ignoring case.
    bool FindImageEncoder(StringView nameOrExtension, ImageEncoder& out);

    bool EncodeImage(const TextureData& texture, StringView nameOrExtension, Array<u8>& out, String& error, const EncodeOptions& options = {});
}
