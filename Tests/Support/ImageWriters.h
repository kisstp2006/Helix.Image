#pragma once

#include <Helix/Core/Containers/Array.h>

// Small writers of the formats the decoders read, so the tests know every pixel of their files.
namespace helix::image::testing
{
    inline void Put8(Array<u8>& out, u32 v) { out.Push(u8(v)); }
    inline void PutLe16(Array<u8>& out, u32 v)
    {
        out.Push(u8(v));
        out.Push(u8(v >> 8));
    }
    inline void PutLe32(Array<u8>& out, u32 v)
    {
        PutLe16(out, v & 0xFFFF);
        PutLe16(out, v >> 16);
    }
    inline void PutBe16(Array<u8>& out, u32 v)
    {
        out.Push(u8(v >> 8));
        out.Push(u8(v));
    }
    inline void PutBe32(Array<u8>& out, u32 v)
    {
        PutBe16(out, v >> 16);
        PutBe16(out, v & 0xFFFF);
    }
    inline void PutText(Array<u8>& out, const char* text)
    {
        for (; *text; text++)
            out.Push(u8(*text));
    }

    // The pattern of the tests: RGBA8, rows top first.
    inline Array<u8> Pattern(u32 width, u32 height, bool alpha)
    {
        Array<u8> rgba;
        for (u32 y = 0; y < height; y++)
            for (u32 x = 0; x < width; x++)
            {
                rgba.Push(u8(x * 23 + y));
                rgba.Push(u8(y * 41));
                rgba.Push(u8((x ^ y) * 13));
                rgba.Push(alpha ? u8(255 - x * 9) : u8(255));
            }
        return rgba;
    }

    // 32 bit TGA, bottom up or top down, raw or run length encoded (runs of up to 128 copies of a pixel or literals).
    inline Array<u8> WriteTga(const Array<u8>& rgba, u32 width, u32 height, bool rle, bool topDown)
    {
        Array<u8> out;
        Put8(out, 0);
        Put8(out, 0);
        Put8(out, rle ? 10 : 2);
        for (int i = 0; i < 5; i++)
            Put8(out, 0);
        PutLe16(out, 0);
        PutLe16(out, 0);
        PutLe16(out, width);
        PutLe16(out, height);
        Put8(out, 32);
        Put8(out, 8 | (topDown ? 0x20 : 0));
        for (u32 row = 0; row < height; row++)
        {
            u32 y = topDown ? row : height - 1 - row;
            const u8* line = rgba.Data() + usize(y) * width * 4;
            auto bgra = [&](u32 x) {
                const u8* p = line + x * 4;
                Put8(out, p[2]);
                Put8(out, p[1]);
                Put8(out, p[0]);
                Put8(out, p[3]);
            };
            if (!rle)
            {
                for (u32 x = 0; x < width; x++)
                    bgra(x);
                continue;
            }
            // Packets: a run of 3 equal pixels (forced by repeating the first one) then literals, to cover both kinds.
            for (u32 x = 0; x < width;)
            {
                u32 literal = width - x < 4 ? width - x : 4;
                Put8(out, literal - 1);
                for (u32 i = 0; i < literal; i++)
                    bgra(x + i);
                x += literal;
            }
        }
        return out;
    }

    // A flattened PSD: RGBA channels as raw planes of 8 or 16 bits per sample.
    inline Array<u8> WritePsd(const Array<u8>& rgba, u32 width, u32 height, bool sixteen)
    {
        Array<u8> out;
        PutText(out, "8BPS");
        PutBe16(out, 1);
        for (int i = 0; i < 6; i++)
            Put8(out, 0);
        PutBe16(out, 4);
        PutBe32(out, height);
        PutBe32(out, width);
        PutBe16(out, sixteen ? 16 : 8);
        PutBe16(out, 3);
        PutBe32(out, 0);
        PutBe32(out, 0);
        PutBe32(out, 0);
        PutBe16(out, 0);
        for (u32 c = 0; c < 4; c++)
            for (usize i = 0; i < usize(width) * height; i++)
            {
                u8 v = rgba[i * 4 + c];
                if (sixteen)
                    PutBe16(out, v * 257u + (c == 0 ? 1u : 0u));
                else
                    Put8(out, v);
            }
        return out;
    }

    // One frame of a GIF: its rectangle on the canvas (RGBA rows of w x h) and what happens to it after it is shown
    // (the disposal method: 0 or 1 keep it, 2 clears to the background, 3 restores what was there before).
    struct GifFrame
    {
        Array<u8> rgba = Array<u8>();
        u32 x = 0, y = 0, w = 0, h = 0;
        u32 disposal = 0;
    };

    // A GIF of frames of up to 4 colors each from its own local palette, with a delay in 1/100 s. LZW codes are
    // written with a clear code before every pixel, so the code size never grows.
    inline Array<u8> WriteGifFrames(const Array<GifFrame>& frames, u32 width, u32 height, u32 delay)
    {
        Array<u8> out;
        PutText(out, "GIF89a");
        PutLe16(out, width);
        PutLe16(out, height);
        Put8(out, 0);
        Put8(out, 0);
        Put8(out, 0);
        for (const GifFrame& frame : frames)
        {
            Array<u8> palette;
            Array<u8> indices;
            for (usize i = 0; i < usize(frame.w) * frame.h; i++)
            {
                const u8* p = frame.rgba.Data() + i * 4;
                usize found = palette.Size() / 3;
                for (usize c = 0; c < palette.Size() / 3; c++)
                    if (palette[c * 3] == p[0] && palette[c * 3 + 1] == p[1] && palette[c * 3 + 2] == p[2])
                        found = c;
                if (found == palette.Size() / 3)
                {
                    palette.Push(p[0]);
                    palette.Push(p[1]);
                    palette.Push(p[2]);
                }
                indices.Push(u8(found));
            }
            while (palette.Size() < 12)
                palette.Push(0);
            Put8(out, 0x21);
            Put8(out, 0xF9);
            Put8(out, 4);
            Put8(out, (frame.disposal & 7) << 2);
            PutLe16(out, delay);
            Put8(out, 0);
            Put8(out, 0);
            Put8(out, 0x2C);
            PutLe16(out, frame.x);
            PutLe16(out, frame.y);
            PutLe16(out, frame.w);
            PutLe16(out, frame.h);
            Put8(out, 0x80 | 1);
            for (usize i = 0; i < 12; i++)
                Put8(out, palette[i]);
            Put8(out, 2);
            Array<u8> codes;
            u32 bits = 0, count = 0;
            auto code = [&](u32 c) {
                bits |= c << count;
                count += 3;
                while (count >= 8)
                {
                    codes.Push(u8(bits));
                    bits >>= 8;
                    count -= 8;
                }
            };
            for (u8 index : indices)
            {
                code(4);
                code(index);
            }
            code(5);
            if (count)
                codes.Push(u8(bits));
            for (usize at = 0; at < codes.Size(); at += 255)
            {
                usize n = codes.Size() - at < 255 ? codes.Size() - at : 255;
                Put8(out, u32(n));
                for (usize i = 0; i < n; i++)
                    Put8(out, codes[at + i]);
            }
            Put8(out, 0);
        }
        Put8(out, 0x3B);
        return out;
    }

    // Whole canvas frames.
    inline Array<u8> WriteGif(const Array<Array<u8>>& frames, u32 width, u32 height, u32 delay)
    {
        Array<GifFrame> all;
        for (const Array<u8>& rgba : frames)
        {
            GifFrame& f = all.Emplace();
            f.rgba = rgba;
            f.w = width, f.h = height;
        }
        return WriteGifFrames(all, width, height, delay);
    }
}
