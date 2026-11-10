#pragma once

#include <Helix/Image/Codecs/ImageDecoder.h>

namespace helix::image
{
    // The decoders of this library, registered on first use of the registry. Each codec adds its own here.
    const ImageDecoder& PngImageDecoder();
    const ImageDecoder& JpegImageDecoder();
    const ImageDecoder& BmpImageDecoder();
    const ImageDecoder& TgaImageDecoder();
    const ImageDecoder& PsdImageDecoder();
    const ImageDecoder& GifImageDecoder();
    const ImageDecoder& HdrImageDecoder();
    const ImageDecoder& TiffImageDecoder();

    inline const ImageDecoder* const* BuiltInDecoders(usize& count)
    {
        static const ImageDecoder* const kDecoders[] = {&PngImageDecoder(), &JpegImageDecoder(), &BmpImageDecoder(), &PsdImageDecoder(), &GifImageDecoder(),
                                                       &HdrImageDecoder(), &TiffImageDecoder(), &TgaImageDecoder()};
        count = sizeof(kDecoders) / sizeof(kDecoders[0]);
        return kDecoders;
    }
}
