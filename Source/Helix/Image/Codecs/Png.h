#pragma once

#include <Helix/Core/Basics/Span.h>
#include <Helix/Image/Image/Image.h>

namespace helix::image
{
    // Decodes every standard PNG (all color types and bit depths, palettes with transparency, Adam7 interlacing) to RGBA8.
    // 16 bit samples are rounded to 8 bits; chunk CRCs and the zlib checksum are verified.
    bool DecodePng(Span<const u8> data, Image& out);

    // Writes gray, gray alpha, RGB or RGBA at 8 bits per sample from the image format, picking a filter per row.
    void EncodePng(const Image& image, Array<u8>& out, int compressionLevel = 6);
}
