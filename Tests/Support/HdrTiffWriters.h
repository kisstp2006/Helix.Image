#pragma once

#include <Support/ImageWriters.h>

#include <Helix/Image/Compression/Deflate.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace helix::image::testing
{
    // Radiance RGBE of one color, as the reference float2rgbe does.
    inline void ToRgbe(const float* rgb, u8* out)
    {
        float v = rgb[0] > rgb[1] ? rgb[0] : rgb[1];
        v = v > rgb[2] ? v : rgb[2];
        if (v < 1e-32f)
        {
            out[0] = out[1] = out[2] = out[3] = 0;
            return;
        }
        int e = 0;
        float scale = std::frexp(v, &e) * 256.0f / v;
        for (int c = 0; c < 3; c++)
            out[c] = u8(rgb[c] * scale);
        out[3] = u8(e + 128);
    }

    // An .hdr file of RGB floats: flat pixels or per channel runs, top down or bottom up.
    inline Array<u8> WriteHdr(const Array<float>& rgb, u32 width, u32 height, bool rle, bool bottomUp, Array<u8>& rgbe)
    {
        Array<u8> out;
        PutText(out, "#?RADIANCE\n# written by the tests\nFORMAT=32-bit_rle_rgbe\nEXPOSURE=1.0\n\n");
        char resolution[64];
        std::snprintf(resolution, sizeof(resolution), "%cY %u +X %u\n", bottomUp ? '+' : '-', height, width);
        PutText(out, resolution);
        rgbe.Resize(usize(width) * height * 4);
        for (usize i = 0; i < usize(width) * height; i++)
            ToRgbe(rgb.Data() + i * 3, rgbe.Data() + i * 4);
        for (u32 row = 0; row < height; row++)
        {
            u32 y = bottomUp ? height - 1 - row : row;
            const u8* line = rgbe.Data() + usize(y) * width * 4;
            if (!rle)
            {
                for (u32 i = 0; i < width * 4; i++)
                    Put8(out, line[i]);
                continue;
            }
            Put8(out, 2);
            Put8(out, 2);
            Put8(out, width >> 8);
            Put8(out, width & 0xFF);
            for (u32 c = 0; c < 4; c++)
                for (u32 x = 0; x < width;)
                {
                    u32 run = 1;
                    while (x + run < width && run < 127 && line[(x + run) * 4 + c] == line[x * 4 + c])
                        run++;
                    if (run >= 3)
                    {
                        Put8(out, 128 + run);
                        Put8(out, line[x * 4 + c]);
                        x += run;
                        continue;
                    }
                    u32 literal = 0;
                    while (x + literal < width && literal < 128)
                    {
                        if (x + literal + 2 < width && line[(x + literal) * 4 + c] == line[(x + literal + 1) * 4 + c] &&
                            line[(x + literal) * 4 + c] == line[(x + literal + 2) * 4 + c])
                            break;
                        literal++;
                    }
                    if (literal == 0)
                        literal = 1;
                    Put8(out, literal);
                    for (u32 i = 0; i < literal; i++)
                        Put8(out, line[(x + i) * 4 + c]);
                    x += literal;
                }
        }
        return out;
    }

    struct TiffOptions
    {
        u32 bits = 8;
        u32 samples = 3;
        u32 photometric = 2;
        u32 compression = 1;
        u32 predictor = 1;
        bool bigEndian = false;
        bool tiled = false;
        bool planar = false;
        u32 rowsPerStrip = 3;
        // ExtraSamples 1 (premultiplied) instead of 2.
        bool associatedAlpha = false;
    };

    // A TIFF of `samples` per pixel from raw sample values (one u32 each, floats as their bits); palette files take a
    // color map of 3 << bits values.
    inline Array<u8> WriteTiff(const Array<u32>& values, u32 width, u32 height, const TiffOptions& o, const Array<u16>* colorMapPointer = nullptr)
    {
        bool big = o.bigEndian;
        auto put16 = [&](Array<u8>& out, u32 v) { big ? PutBe16(out, v) : PutLe16(out, v); };
        auto put32 = [&](Array<u8>& out, u32 v) { big ? PutBe32(out, v) : PutLe32(out, v); };
        u32 planes = o.planar ? o.samples : 1, perPixel = o.planar ? 1 : o.samples;
        u32 segW = o.tiled ? 16 : width, segH = o.tiled ? 16 : o.rowsPerStrip;
        u32 across = (width + segW - 1) / segW, down = (height + segH - 1) / segH;
        usize rowBytes = (usize(segW) * perPixel * o.bits + 7) / 8;

        Array<Array<u8>> segments;
        for (u32 p = 0; p < planes; p++)
            for (u32 sy = 0; sy < down; sy++)
                for (u32 sx = 0; sx < across; sx++)
                {
                    u32 rows = o.tiled ? segH : (sy * segH + segH > height ? height - sy * segH : segH);
                    Array<u8> raw;
                    raw.Resize(rowBytes * rows);
                    for (u32 y = 0; y < rows; y++)
                    {
                        u8* row = raw.Data() + y * rowBytes;
                        Array<u32> line;
                        for (u32 x = 0; x < segW; x++)
                            for (u32 c = 0; c < perPixel; c++)
                            {
                                u32 px = sx * segW + x, py = sy * segH + y;
                                u32 channel = o.planar ? p : c;
                                line.Push(px < width && py < height ? values[(usize(py) * width + px) * o.samples + channel] : 0u);
                            }
                        if (o.predictor == 2)
                            for (usize i = line.Size(); i-- > perPixel;)
                                line[i] = (line[i] - line[i - perPixel]) & (o.bits == 16 ? 0xFFFFu : 0xFFu);
                        for (usize i = 0; i < line.Size(); i++)
                        {
                            u32 v = line[i];
                            switch (o.bits)
                            {
                            case 1: row[i >> 3] |= u8((v & 1) << (7 - (i & 7))); break;
                            case 4: row[i >> 1] |= u8((v & 15) << (4 - 4 * (i & 1))); break;
                            case 8: row[i] = u8(v); break;
                            case 16:
                                row[2 * i] = u8(big ? v >> 8 : v);
                                row[2 * i + 1] = u8(big ? v : v >> 8);
                                break;
                            default:
                                for (u32 b = 0; b < 4; b++)
                                    row[4 * i + b] = u8(v >> (8 * (big ? 3 - b : b)));
                                break;
                            }
                        }
                        if (o.predictor == 3)
                        {
                            // The floating point predictor: byte planes, most significant first, then differences.
                            usize n = line.Size();
                            for (usize i = 0; i < n; i++)
                                for (u32 b = 0; b < 4; b++)
                                    row[b * n + i] = u8(line[i] >> (8 * (3 - b)));
                            for (usize i = n * 4; i-- > perPixel;)
                                row[i] = u8(row[i] - row[i - perPixel]);
                        }
                    }
                    Array<u8>& stored = segments.Emplace();
                    if (o.compression == 8)
                        ZlibCompress(Span<const u8>(raw.Data(), raw.Size()), stored);
                    else if (o.compression == 32773)
                    {
                        // Literal packets of up to 64 bytes, and a run packet wherever 4 equal bytes follow.
                        for (usize i = 0; i < raw.Size();)
                        {
                            usize run = 1;
                            while (i + run < raw.Size() && run < 128 && raw[i + run] == raw[i])
                                run++;
                            if (run >= 4)
                            {
                                stored.Push(u8(1 - int(run)));
                                stored.Push(raw[i]);
                                i += run;
                                continue;
                            }
                            usize n = raw.Size() - i < 64 ? raw.Size() - i : 64;
                            stored.Push(u8(n - 1));
                            for (usize k = 0; k < n; k++)
                                stored.Push(raw[i + k]);
                            i += n;
                        }
                    }
                    else
                        stored = Move(raw);
                }

        Array<u8> out;
        PutText(out, big ? "MM" : "II");
        put16(out, 42);
        put32(out, 8);
        struct Entry
        {
            u32 tag, type;
            Array<u32> values;
        };
        Array<Entry> entries;
        auto add = [&](u32 tag, u32 type, Array<u32> v) {
            Entry& e = entries.Emplace();
            e.tag = tag;
            e.type = type;
            e.values = Move(v);
        };
        auto one = [](u32 v) {
            Array<u32> a;
            a.Push(v);
            return a;
        };
        Array<u32> bits, offsets, counts, formats;
        for (u32 s = 0; s < o.samples; s++)
        {
            bits.Push(o.bits);
            formats.Push(o.bits == 32 ? 3u : 1u);
        }
        for (usize i = 0; i < segments.Size(); i++)
        {
            offsets.Push(0);
            counts.Push(u32(segments[i].Size()));
        }
        add(256, 4, one(width));
        add(257, 4, one(height));
        add(258, 3, bits);
        add(259, 3, one(o.compression));
        add(262, 3, one(o.photometric));
        add(o.tiled ? 324 : 273, 4, offsets);
        add(277, 3, one(o.samples));
        if (!o.tiled)
            add(278, 4, one(o.rowsPerStrip));
        add(o.tiled ? 325 : 279, 4, counts);
        add(284, 3, one(o.planar ? 2 : 1));
        if (o.predictor != 1)
            add(317, 3, one(o.predictor));
        if (colorMapPointer)
        {
            Array<u32> map;
            for (u16 v : *colorMapPointer)
                map.Push(v);
            add(320, 3, map);
        }
        if (o.tiled)
        {
            add(322, 3, one(16));
            add(323, 3, one(16));
        }
        u32 color = o.photometric == 2 ? 3 : 1;
        if (o.samples > color)
            add(338, 3, one(o.associatedAlpha ? 1 : 2));
        add(339, 3, formats);
        for (usize i = 1; i < entries.Size(); i++)
            for (usize j = i; j > 0 && entries[j - 1].tag > entries[j].tag; j--)
            {
                Entry t = Move(entries[j - 1]);
                entries[j - 1] = Move(entries[j]);
                entries[j] = Move(t);
            }

        // Layout: the directory, then the arrays that do not fit an entry, then the strips or tiles.
        usize directory = 8, extra = directory + 2 + entries.Size() * 12 + 4;
        usize at = extra;
        Array<usize> places;
        for (const Entry& e : entries)
        {
            usize size = e.values.Size() * (e.type == 3 ? 2 : 4);
            places.Push(size > 4 ? at : 0);
            if (size > 4)
                at += (size + 1) & ~usize(1);
        }
        usize data = at;
        for (usize i = 0; i < segments.Size(); i++)
        {
            offsets[i] = u32(data);
            data += segments[i].Size();
        }
        for (Entry& e : entries)
            if (e.tag == 273 || e.tag == 324)
                e.values = offsets;
        put16(out, u32(entries.Size()));
        for (usize i = 0; i < entries.Size(); i++)
        {
            const Entry& e = entries[i];
            put16(out, e.tag);
            put16(out, e.type);
            put32(out, u32(e.values.Size()));
            if (places[i])
                put32(out, u32(places[i]));
            else
            {
                usize start = out.Size();
                for (u32 v : e.values)
                    e.type == 3 ? put16(out, v) : put32(out, v);
                while (out.Size() < start + 4)
                    out.Push(0);
            }
        }
        put32(out, 0);
        for (usize i = 0; i < entries.Size(); i++)
            if (places[i])
            {
                for (u32 v : entries[i].values)
                    entries[i].type == 3 ? put16(out, v) : put32(out, v);
                if (out.Size() & 1)
                    out.Push(0);
            }
        for (const Array<u8>& s : segments)
            out.Append(s.Data(), s.Size());
        return out;
    }
}
