#pragma once

#include <Helix/Core/Containers/Array.h>

namespace helix::image
{
    enum class ImageFormat : u8
    {
        R8,
        RG8,
        RGB8,
        RGBA8,
    };

    constexpr u32 ChannelCount(ImageFormat format) { return u32(format) + 1; }

    // Tightly packed 8 bit pixels, rows top first.
    struct Image
    {
        u32 width = 0;
        u32 height = 0;
        ImageFormat format = ImageFormat::RGBA8;
        Array<u8> pixels;

        usize RowBytes() const { return usize(width) * ChannelCount(format); }

        void Allocate(u32 w, u32 h, ImageFormat f)
        {
            width = w;
            height = h;
            format = f;
            pixels.Resize(RowBytes() * h);
        }
    };
}
