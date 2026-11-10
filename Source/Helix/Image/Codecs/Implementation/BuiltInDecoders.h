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
    const ImageDecoder& ExrImageDecoder();
    const ImageDecoder& DdsImageDecoder();
    const ImageDecoder& Ktx1ImageDecoder();
    const ImageDecoder& Ktx2ImageDecoder();
    const ImageDecoder& PvrImageDecoder();

    inline const ImageDecoder* const* BuiltInDecoders(usize& count)
    {
        static const ImageDecoder* const kDecoders[] = {&PngImageDecoder(), &JpegImageDecoder(), &BmpImageDecoder(), &PsdImageDecoder(), &GifImageDecoder(),
                                                       &HdrImageDecoder(), &TiffImageDecoder(), &ExrImageDecoder(), &DdsImageDecoder(),
                                                       &Ktx1ImageDecoder(), &Ktx2ImageDecoder(), &PvrImageDecoder(), &TgaImageDecoder()};
        count = sizeof(kDecoders) / sizeof(kDecoders[0]);
        return kDecoders;
    }
}
