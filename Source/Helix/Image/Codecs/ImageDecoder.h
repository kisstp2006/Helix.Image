#pragma once

#include <Helix/Core/Basics/Span.h>
#include <Helix/Core/Strings/String.h>
#include <Helix/Image/Texture/TextureData.h>

namespace helix::image
{
    // A file format that can be decoded. Decoders are found by their signature first and by the file extension for
    // formats that have none (TGA).
    struct ImageDecoder
    {
        StringView name;
        // Lower case, without the dot. The array must outlive the registration.
        Span<const StringView> extensions;
        // Null for formats without a signature.
        bool (*matches)(Span<const u8> data) = nullptr;
        // On failure `error` says what is wrong with the file.
        bool (*decode)(Span<const u8> data, TextureData& out, String& error) = nullptr;
    };

    // Adds a decoder, or replaces the one with the same name. The built in decoders are registered on first use.
    void RegisterImageDecoder(const ImageDecoder& decoder);
    // A copy of the registered decoders, in registration order.
    void ImageDecoders(Array<ImageDecoder>& out);
    bool FindImageDecoder(Span<const u8> data, StringView extension, ImageDecoder& out);

    // Decodes any registered format; `extension` (with or without the dot) helps only formats without a signature.
    bool DecodeImage(Span<const u8> data, TextureData& out, String& error, StringView extension = {});
}
