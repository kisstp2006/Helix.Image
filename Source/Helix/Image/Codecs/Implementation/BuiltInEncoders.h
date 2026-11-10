#pragma once

#include <Helix/Image/Codecs/ImageEncoder.h>
#include <Helix/Image/Texture/Convert.h>

namespace helix::image
{
    // The encoders of this library, registered on first use of the registry. Each codec adds its own here.
    const ImageEncoder& PngImageEncoder();
    const ImageEncoder& JpgImageEncoder();
    const ImageEncoder& TgaImageEncoder();
    const ImageEncoder& ExrImageEncoder();

    inline const ImageEncoder* const* BuiltInEncoders(usize& count)
    {
        static const ImageEncoder* const kEncoders[] = {&PngImageEncoder(), &JpgImageEncoder(), &TgaImageEncoder(), &ExrImageEncoder()};
        count = sizeof(kEncoders) / sizeof(kEncoders[0]);
        return kEncoders;
    }

    // The first 2D surface of a texture in another uncompressed format, rows top first and tightly packed.
    inline bool FirstSurface(const TextureData& texture, PixelFormat format, Array<u8>& pixels, String& error)
    {
        if (texture.subresources.Empty() || !texture.width || !texture.height)
        {
            error = "the texture is empty";
            return false;
        }
        if (!CanConvert(texture.format))
        {
            error = "block compressed textures cannot be encoded";
            return false;
        }
        usize count = usize(texture.width) * texture.height;
        pixels.Resize(count * FormatInfo(format).bytesPerBlock);
        return ConvertPixels(texture.format, texture.Pixels(), format, pixels.Data(), count);
    }
}
