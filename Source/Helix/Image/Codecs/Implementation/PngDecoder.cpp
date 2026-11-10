#include <Helix/Image/Codecs/Implementation/PngCommon.h>
#include <Helix/Image/Codecs/Png.h>
#include <Helix/Image/Compression/Deflate.h>

#include <Helix/Core/Hashing/Hash.h>

#include <cstring>

namespace helix::image
{
    namespace
    {
        using namespace png;

        struct Header
        {
            u32 width = 0, height = 0;
            u8 depth = 0, colorType = 0, interlace = 0;
            u32 channels = 0;
        };

        u32 Channels(u8 colorType)
        {
            switch (colorType)
            {
            case 0: return 1;
            case 2: return 3;
            case 3: return 1;
            case 4: return 2;
            case 6: return 4;
            default: return 0;
            }
        }

        bool ValidDepth(u8 colorType, u8 depth)
        {
            switch (colorType)
            {
            case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
            case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
            case 2:
            case 4:
            case 6: return depth == 8 || depth == 16;
            default: return false;
            }
        }

        // Undoes the per row filters of one (sub)image in place; rows are prefixed by their filter type byte.
        // `zeroRow` stands in for the row above the first one, so the loops need no per byte edge checks.
        bool Unfilter(u8* data, u32 rows, usize rowBytes, u32 bpp, const u8* zeroRow)
        {
            const u8* prior = zeroRow;
            usize head = Min<usize>(bpp, rowBytes);
            for (u32 y = 0; y < rows; y++)
            {
                u8* row = data + y * (rowBytes + 1);
                u8 filter = row[0];
                u8* cur = row + 1;
                switch (filter)
                {
                case 0: break;
                case 1:
                    for (usize i = bpp; i < rowBytes; i++)
                        cur[i] = u8(cur[i] + cur[i - bpp]);
                    break;
                case 2:
                    for (usize i = 0; i < rowBytes; i++)
                        cur[i] = u8(cur[i] + prior[i]);
                    break;
                case 3:
                    for (usize i = 0; i < head; i++)
                        cur[i] = u8(cur[i] + (prior[i] >> 1));
                    for (usize i = bpp; i < rowBytes; i++)
                        cur[i] = u8(cur[i] + ((u32(cur[i - bpp]) + prior[i]) >> 1));
                    break;
                case 4:
                    for (usize i = 0; i < head; i++)
                        cur[i] = u8(cur[i] + prior[i]);
                    for (usize i = bpp; i < rowBytes; i++)
                        cur[i] = u8(cur[i] + Paeth(cur[i - bpp], prior[i], prior[i - bpp]));
                    break;
                default: return false;
                }
                prior = cur;
            }
            return true;
        }

        struct Converter
        {
            const Header& h;
            const u8* palette;
            u32 paletteSize;
            const u8* trns;
            u32 trnsSize;

            u32 Sample(const u8* row, u32 index) const
            {
                switch (h.depth)
                {
                case 1: return (row[index >> 3] >> (7 - (index & 7))) & 1;
                case 2: return (row[index >> 2] >> (6 - 2 * (index & 3))) & 3;
                case 4: return (row[index >> 1] >> (4 - 4 * (index & 1))) & 15;
                case 8: return row[index];
                default: return (u32(row[2 * index]) << 8) | row[2 * index + 1];
                }
            }

            u8 To8(u32 v) const
            {
                switch (h.depth)
                {
                case 1: return v ? 255 : 0;
                case 2: return u8(v * 85);
                case 4: return u8(v * 17);
                case 8: return u8(v);
                default: return u8((v * 255 + 32895) >> 16);
                }
            }

            u32 TrnsSample(u32 i) const { return trnsSize >= 2 * (i + 1) ? (u32(trns[2 * i]) << 8) | trns[2 * i + 1] : 0x10000; }

            // Writes `count` pixels of an unfiltered row to RGBA at dst, every `step` pixels.
            bool Convert(const u8* row, u32 count, u8* dst, usize step) const
            {
                // The common 8 bit layouts skip the generic sample reader.
                if (h.depth == 8 && h.colorType == 6 && step == 4)
                {
                    std::memcpy(dst, row, usize(count) * 4);
                    return true;
                }
                if (h.depth == 8 && h.colorType == 2 && trnsSize < 6)
                {
                    for (u32 x = 0; x < count; x++, dst += step, row += 3)
                    {
                        dst[0] = row[0];
                        dst[1] = row[1];
                        dst[2] = row[2];
                        dst[3] = 255;
                    }
                    return true;
                }
                for (u32 x = 0; x < count; x++, dst += step)
                {
                    u32 base = x * h.channels;
                    switch (h.colorType)
                    {
                    case 0:
                    {
                        u32 g = Sample(row, base);
                        dst[0] = dst[1] = dst[2] = To8(g);
                        dst[3] = trnsSize >= 2 && g == TrnsSample(0) ? 0 : 255;
                        break;
                    }
                    case 2:
                    {
                        u32 r = Sample(row, base), g = Sample(row, base + 1), b = Sample(row, base + 2);
                        dst[0] = To8(r);
                        dst[1] = To8(g);
                        dst[2] = To8(b);
                        dst[3] = trnsSize >= 6 && r == TrnsSample(0) && g == TrnsSample(1) && b == TrnsSample(2) ? 0 : 255;
                        break;
                    }
                    case 3:
                    {
                        u32 i = Sample(row, base);
                        if (i >= paletteSize)
                            return false;
                        dst[0] = palette[3 * i];
                        dst[1] = palette[3 * i + 1];
                        dst[2] = palette[3 * i + 2];
                        dst[3] = i < trnsSize ? trns[i] : 255;
                        break;
                    }
                    case 4:
                        dst[0] = dst[1] = dst[2] = To8(Sample(row, base));
                        dst[3] = To8(Sample(row, base + 1));
                        break;
                    default:
                        for (u32 c = 0; c < 4; c++)
                            dst[c] = To8(Sample(row, base + c));
                        break;
                    }
                }
                return true;
            }
        };
    }

    bool DecodePng(Span<const u8> data, Image& out)
    {
        if (data.Size() < 8 || std::memcmp(data.Data(), kSignature, 8) != 0)
            return false;
        Header h;
        Array<u8> compressed;
        u8 palette[768];
        u32 paletteSize = 0;
        u8 trns[256];
        u32 trnsSize = 0;
        bool sawHeader = false, sawEnd = false;
        for (usize at = 8; at + 12 <= data.Size() && !sawEnd;)
        {
            u32 length = ReadBe32(data.Data() + at);
            const u8* type = data.Data() + at + 4;
            if (length > data.Size() - at - 12)
                return false;
            const u8* body = type + 4;
            if (Crc32(type, length + 4) != ReadBe32(body + length))
                return false;
            if (std::memcmp(type, "IHDR", 4) == 0)
            {
                if (length != 13)
                    return false;
                h.width = ReadBe32(body);
                h.height = ReadBe32(body + 4);
                h.depth = body[8];
                h.colorType = body[9];
                h.interlace = body[12];
                h.channels = Channels(h.colorType);
                if (!h.width || !h.height || h.width > (1u << 24) || h.height > (1u << 24) || !ValidDepth(h.colorType, h.depth) || body[10] || body[11] ||
                    h.interlace > 1)
                    return false;
                sawHeader = true;
            }
            else if (std::memcmp(type, "PLTE", 4) == 0)
            {
                if (length % 3 || length > 768)
                    return false;
                std::memcpy(palette, body, length);
                paletteSize = length / 3;
            }
            else if (std::memcmp(type, "tRNS", 4) == 0)
            {
                if (length > 256)
                    return false;
                std::memcpy(trns, body, length);
                trnsSize = length;
            }
            else if (std::memcmp(type, "IDAT", 4) == 0)
            {
                usize n = compressed.Size();
                compressed.Resize(n + length);
                std::memcpy(compressed.Data() + n, body, length);
            }
            else if (std::memcmp(type, "IEND", 4) == 0)
                sawEnd = true;
            else if (!(type[0] & 0x20))
                return false;
            at += 12 + usize(length);
        }
        if (!sawHeader || (h.colorType == 3 && !paletteSize))
            return false;

        u32 bitsPerPixel = h.channels * h.depth;
        u32 bpp = Max(1u, bitsPerPixel / 8);
        auto rowBytes = [&](u32 w) { return (usize(w) * bitsPerPixel + 7) / 8; };
        usize expected = 0;
        if (h.interlace)
        {
            for (const auto& p : kAdam7)
            {
                u32 w = (h.width + p[2] - 1 - p[0]) / p[2], hh = (h.height + p[3] - 1 - p[1]) / p[3];
                if (w && hh)
                    expected += (rowBytes(w) + 1) * hh;
            }
        }
        else
            expected = (rowBytes(h.width) + 1) * h.height;

        Array<u8> raw;
        raw.Reserve(expected);
        if (!ZlibDecompress(compressed, raw) || raw.Size() < expected)
            return false;

        out.Allocate(h.width, h.height, ImageFormat::RGBA8);
        Array<u8> zeroRow;
        zeroRow.Resize(rowBytes(h.width));
        Converter conv{h, palette, paletteSize, trns, trnsSize};
        if (!h.interlace)
        {
            usize rb = rowBytes(h.width);
            if (!Unfilter(raw.Data(), h.height, rb, bpp, zeroRow.Data()))
                return false;
            for (u32 y = 0; y < h.height; y++)
                if (!conv.Convert(raw.Data() + y * (rb + 1) + 1, h.width, out.pixels.Data() + usize(y) * h.width * 4, 4))
                    return false;
            return true;
        }
        u8* pass = raw.Data();
        for (const auto& p : kAdam7)
        {
            u32 w = (h.width + p[2] - 1 - p[0]) / p[2], hh = (h.height + p[3] - 1 - p[1]) / p[3];
            if (!w || !hh)
                continue;
            usize rb = rowBytes(w);
            if (!Unfilter(pass, hh, rb, bpp, zeroRow.Data()))
                return false;
            for (u32 y = 0; y < hh; y++)
            {
                u8* dst = out.pixels.Data() + (usize(p[1] + y * p[3]) * h.width + p[0]) * 4;
                if (!conv.Convert(pass + y * (rb + 1) + 1, w, dst, usize(p[2]) * 4))
                    return false;
            }
            pass += (rb + 1) * hh;
        }
        return true;
    }
}
