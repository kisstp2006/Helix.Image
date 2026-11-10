#pragma once

#include <Support/ImageWriters.h>

#include <Helix/Image/Compression/Deflate.h>

#include <cstring>

namespace helix::image::testing
{
    struct ExrChannel
    {
        const char* name;
        // 0 UINT, 1 HALF, 2 FLOAT.
        u32 type;
    };

    struct ExrOptions
    {
        // 0 NONE, 1 RLE, 2 ZIPS, 3 ZIP.
        u32 compression = 0;
        bool tiled = false;
        u32 tile = 16;
        i32 xMin = 0, yMin = 0;
    };

    // The forward predictor of RLE and ZIP data: the two halves of the bytes split, then deltas.
    inline Array<u8> ExrPredict(const Array<u8>& raw)
    {
        usize n = raw.Size(), half = (n + 1) / 2;
        Array<u8> t;
        t.Resize(n);
        for (usize i = 0; i < n; i++)
            t[i & 1 ? half + i / 2 : i / 2] = raw[i];
        Array<u8> out;
        out.Resize(n);
        for (usize i = 0; i < n; i++)
            out[i] = i == 0 ? t[0] : u8(t[i] - t[i - 1] + 128);
        return out;
    }

    inline Array<u8> ExrRle(const Array<u8>& in)
    {
        Array<u8> out;
        for (usize i = 0; i < in.Size();)
        {
            usize run = 1;
            while (i + run < in.Size() && run < 128 && in[i + run] == in[i])
                run++;
            if (run >= 3)
            {
                out.Push(u8(run - 1));
                out.Push(in[i]);
                i += run;
                continue;
            }
            usize literal = 0;
            while (i + literal < in.Size() && literal < 127 && !(i + literal + 2 < in.Size() && in[i + literal] == in[i + literal + 1] && in[i + literal] == in[i + literal + 2]))
                literal++;
            out.Push(u8(-i32(literal)));
            out.Append(in.Data() + i, literal);
            i += literal;
        }
        return out;
    }

    // An OpenEXR file of channels sorted by name; `samples` holds one value per pixel and channel (half or float bits, or
    // the integer), in the channel order given.
    inline Array<u8> WriteExr(const Array<ExrChannel>& channels, const Array<u32>& samples, u32 width, u32 height, const ExrOptions& o)
    {
        Array<u8> header;
        PutLe32(header, 20000630);
        PutLe32(header, 2 | (o.tiled ? 0x200 : 0));
        auto attribute = [&](const char* name, const char* type, const Array<u8>& value) {
            PutText(header, name);
            Put8(header, 0);
            PutText(header, type);
            Put8(header, 0);
            PutLe32(header, u32(value.Size()));
            header.Append(value.Data(), value.Size());
        };
        Array<u8> v;
        for (const ExrChannel& c : channels)
        {
            PutText(v, c.name);
            Put8(v, 0);
            PutLe32(v, c.type);
            PutLe32(v, 0);
            PutLe32(v, 1);
            PutLe32(v, 1);
        }
        Put8(v, 0);
        attribute("channels", "chlist", v);
        v.Clear();
        Put8(v, o.compression);
        attribute("compression", "compression", v);
        v.Clear();
        PutLe32(v, u32(o.xMin));
        PutLe32(v, u32(o.yMin));
        PutLe32(v, u32(o.xMin + i32(width) - 1));
        PutLe32(v, u32(o.yMin + i32(height) - 1));
        attribute("dataWindow", "box2i", v);
        attribute("displayWindow", "box2i", v);
        v.Clear();
        Put8(v, 0);
        attribute("lineOrder", "lineOrder", v);
        if (o.tiled)
        {
            v.Clear();
            PutLe32(v, o.tile);
            PutLe32(v, o.tile);
            Put8(v, 0);
            attribute("tiles", "tiledesc", v);
        }
        Put8(header, 0);

        u32 lines = o.tiled ? o.tile : o.compression == 3 ? 16 : 1;
        u32 chunkWidth = o.tiled ? o.tile : width;
        u32 across = (width + chunkWidth - 1) / chunkWidth, down = (height + lines - 1) / lines;
        Array<Array<u8>> chunks;
        for (u32 ty = 0; ty < down; ty++)
            for (u32 tx = 0; tx < across; tx++)
            {
                u32 x0 = tx * chunkWidth, y0 = ty * lines;
                u32 w = width - x0 < chunkWidth ? width - x0 : chunkWidth, h = height - y0 < lines ? height - y0 : lines;
                Array<u8> raw;
                for (u32 y = 0; y < h; y++)
                    for (usize c = 0; c < channels.Size(); c++)
                        for (u32 x = 0; x < w; x++)
                        {
                            u32 s = samples[(usize(y0 + y) * width + x0 + x) * channels.Size() + c];
                            if (channels[c].type == 1)
                                PutLe16(raw, s);
                            else
                                PutLe32(raw, s);
                        }
                Array<u8> packed;
                if (o.compression == 1)
                    packed = ExrRle(ExrPredict(raw));
                else if (o.compression >= 2)
                {
                    Array<u8> predicted = ExrPredict(raw);
                    ZlibCompress(Span<const u8>(predicted.Data(), predicted.Size()), packed);
                }
                if (o.compression == 0 || packed.Size() >= raw.Size())
                    packed = Move(raw);
                Array<u8>& chunk = chunks.Emplace();
                if (o.tiled)
                {
                    PutLe32(chunk, tx);
                    PutLe32(chunk, ty);
                    PutLe32(chunk, 0);
                    PutLe32(chunk, 0);
                }
                else
                    PutLe32(chunk, u32(o.yMin + i32(y0)));
                PutLe32(chunk, u32(packed.Size()));
                chunk.Append(packed.Data(), packed.Size());
            }
        Array<u8> out = Move(header);
        u64 at = out.Size() + chunks.Size() * 8;
        for (const Array<u8>& c : chunks)
        {
            PutLe32(out, u32(at));
            PutLe32(out, u32(at >> 32));
            at += c.Size();
        }
        for (const Array<u8>& c : chunks)
            out.Append(c.Data(), c.Size());
        return out;
    }
}
