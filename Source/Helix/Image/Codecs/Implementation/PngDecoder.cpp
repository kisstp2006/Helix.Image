#include <Helix/Image/Codecs/Implementation/BuiltInDecoders.h>
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

        // Turns unfiltered rows into RGBA samples of T: u8, or u16 for 16 bit files.
        template <typename T>
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

            static constexpr u32 kMax = sizeof(T) == 1 ? 255u : 65535u;

            T Out(u32 v) const
            {
                switch (h.depth)
                {
                case 1: return T(v ? kMax : 0);
                case 2: return T(v * (kMax / 3));
                case 4: return T(v * (kMax / 15));
                case 8: return T(sizeof(T) == 1 ? v : v * 257);
                default: return T(sizeof(T) == 1 ? (v * 255 + 32895) >> 16 : v);
                }
            }

            T From8(u8 v) const { return T(sizeof(T) == 1 ? v : v * 257u); }

            u32 TrnsSample(u32 i) const { return trnsSize >= 2 * (i + 1) ? (u32(trns[2 * i]) << 8) | trns[2 * i + 1] : 0x10000; }

            // Writes `count` pixels of an unfiltered row to RGBA at dst, every `step` pixels.
            bool Convert(const u8* row, u32 count, T* dst, usize step) const
            {
                // The common 8 bit layouts skip the generic sample reader.
                if constexpr (sizeof(T) == 1)
                {
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
                }
                for (u32 x = 0; x < count; x++, dst += step)
                {
                    u32 base = x * h.channels;
                    switch (h.colorType)
                    {
                    case 0:
                    {
                        u32 g = Sample(row, base);
                        dst[0] = dst[1] = dst[2] = Out(g);
                        dst[3] = trnsSize >= 2 && g == TrnsSample(0) ? T(0) : T(kMax);
                        break;
                    }
                    case 2:
                    {
                        u32 r = Sample(row, base), g = Sample(row, base + 1), b = Sample(row, base + 2);
                        dst[0] = Out(r);
                        dst[1] = Out(g);
                        dst[2] = Out(b);
                        dst[3] = trnsSize >= 6 && r == TrnsSample(0) && g == TrnsSample(1) && b == TrnsSample(2) ? T(0) : T(kMax);
                        break;
                    }
                    case 3:
                    {
                        u32 i = Sample(row, base);
                        if (i >= paletteSize)
                            return false;
                        dst[0] = From8(palette[3 * i]);
                        dst[1] = From8(palette[3 * i + 1]);
                        dst[2] = From8(palette[3 * i + 2]);
                        dst[3] = i < trnsSize ? From8(trns[i]) : T(kMax);
                        break;
                    }
                    case 4:
                        dst[0] = dst[1] = dst[2] = Out(Sample(row, base));
                        dst[3] = Out(Sample(row, base + 1));
                        break;
                    default:
                        for (u32 c = 0; c < 4; c++)
                            dst[c] = Out(Sample(row, base + c));
                        break;
                    }
                }
                return true;
            }
        };

        // A PNG read up to its unfiltered rows; the pixels come from Expand.
        struct Parsed
        {
            Header h;
            u8 palette[768];
            u32 paletteSize = 0;
            u8 trns[256];
            u32 trnsSize = 0;
            Array<u8> raw;
        };

        bool Parse(Span<const u8> data, Parsed& p, String& error)
        {
            auto fail = [&](const char* what) {
                error = what;
                return false;
            };
            if (data.Size() < 8 || std::memcmp(data.Data(), kSignature, 8) != 0)
                return fail("no PNG signature");
            Header& h = p.h;
            Array<u8> compressed;
            bool sawHeader = false, sawEnd = false;
            for (usize at = 8; at + 12 <= data.Size() && !sawEnd;)
            {
                u32 length = ReadBe32(data.Data() + at);
                const u8* type = data.Data() + at + 4;
                if (length > data.Size() - at - 12)
                    return fail("a chunk runs past the end of the file");
                const u8* body = type + 4;
                if (Crc32(type, length + 4) != ReadBe32(body + length))
                    return fail("a chunk checksum does not match");
                if (std::memcmp(type, "IHDR", 4) == 0)
                {
                    if (length != 13)
                        return fail("the header chunk has the wrong size");
                    h.width = ReadBe32(body);
                    h.height = ReadBe32(body + 4);
                    h.depth = body[8];
                    h.colorType = body[9];
                    h.interlace = body[12];
                    h.channels = Channels(h.colorType);
                    if (!h.width || !h.height || h.width > (1u << 24) || h.height > (1u << 24) || !ValidDepth(h.colorType, h.depth) || body[10] || body[11] ||
                        h.interlace > 1)
                        return fail("the header describes no valid image");
                    sawHeader = true;
                }
                else if (std::memcmp(type, "PLTE", 4) == 0)
                {
                    if (length % 3 || length > 768)
                        return fail("the palette has the wrong size");
                    std::memcpy(p.palette, body, length);
                    p.paletteSize = length / 3;
                }
                else if (std::memcmp(type, "tRNS", 4) == 0)
                {
                    if (length > 256)
                        return fail("the transparency chunk is too long");
                    std::memcpy(p.trns, body, length);
                    p.trnsSize = length;
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
                    return fail("an unknown critical chunk");
                at += 12 + usize(length);
            }
            if (!sawHeader)
                return fail("no header chunk");
            if (h.colorType == 3 && !p.paletteSize)
                return fail("a palette image without a palette");
            if (u64(h.width) * h.height * 8 > kMaxTextureBytes)
                return fail("the image is too large");

            u32 bitsPerPixel = h.channels * h.depth;
            auto rowBytes = [&](u32 w) { return (usize(w) * bitsPerPixel + 7) / 8; };
            usize expected = 0;
            if (h.interlace)
            {
                for (const auto& a : kAdam7)
                {
                    u32 w = (h.width + a[2] - 1 - a[0]) / a[2], hh = (h.height + a[3] - 1 - a[1]) / a[3];
                    if (w && hh)
                        expected += (rowBytes(w) + 1) * hh;
                }
            }
            else
                expected = (rowBytes(h.width) + 1) * h.height;
            p.raw.Reserve(expected);
            if (!ZlibDecompress(compressed, p.raw))
                return fail("the image data does not inflate");
            if (p.raw.Size() < expected)
                return fail("the image data is too short");
            return true;
        }

        template <typename T>
        bool Expand(Parsed& p, T* pixels, String& error)
        {
            const Header& h = p.h;
            u32 bitsPerPixel = h.channels * h.depth;
            u32 bpp = Max(1u, bitsPerPixel / 8);
            auto rowBytes = [&](u32 w) { return (usize(w) * bitsPerPixel + 7) / 8; };
            Array<u8> zeroRow;
            zeroRow.Resize(rowBytes(h.width));
            Converter<T> conv{h, p.palette, p.paletteSize, p.trns, p.trnsSize};
            auto fail = [&](const char* what) {
                error = what;
                return false;
            };
            if (!h.interlace)
            {
                usize rb = rowBytes(h.width);
                if (!Unfilter(p.raw.Data(), h.height, rb, bpp, zeroRow.Data()))
                    return fail("an unknown row filter");
                for (u32 y = 0; y < h.height; y++)
                    if (!conv.Convert(p.raw.Data() + y * (rb + 1) + 1, h.width, pixels + usize(y) * h.width * 4, 4))
                        return fail("a palette index past the palette");
                return true;
            }
            u8* pass = p.raw.Data();
            for (const auto& a : kAdam7)
            {
                u32 w = (h.width + a[2] - 1 - a[0]) / a[2], hh = (h.height + a[3] - 1 - a[1]) / a[3];
                if (!w || !hh)
                    continue;
                usize rb = rowBytes(w);
                if (!Unfilter(pass, hh, rb, bpp, zeroRow.Data()))
                    return fail("an unknown row filter");
                for (u32 y = 0; y < hh; y++)
                {
                    T* dst = pixels + (usize(a[1] + y * a[3]) * h.width + a[0]) * 4;
                    if (!conv.Convert(pass + y * (rb + 1) + 1, w, dst, usize(a[2]) * 4))
                        return fail("a palette index past the palette");
                }
                pass += (rb + 1) * hh;
            }
            return true;
        }

        bool MatchesPng(Span<const u8> data) { return data.Size() >= 8 && std::memcmp(data.Data(), kSignature, 8) == 0; }

        // 16 bit files keep their 16 bits (RGBA16), the others give RGBA8.
        bool DecodePngTexture(Span<const u8> data, TextureData& out, String& error)
        {
            Parsed p;
            if (!Parse(data, p, error))
                return false;
            bool wide = p.h.depth == 16;
            if (!out.Allocate(wide ? PixelFormat::RGBA16 : PixelFormat::RGBA8, p.h.width, p.h.height))
                return false;
            if (!wide)
                return Expand(p, out.data.Data(), error);
            Array<u16> samples;
            samples.Resize(usize(p.h.width) * p.h.height * 4);
            if (!Expand(p, samples.Data(), error))
                return false;
            std::memcpy(out.data.Data(), samples.Data(), samples.Size() * 2);
            return true;
        }
    }

    bool DecodePng(Span<const u8> data, Image& out)
    {
        Parsed p;
        String error;
        if (!Parse(data, p, error))
            return false;
        out.Allocate(p.h.width, p.h.height, ImageFormat::RGBA8);
        return Expand(p, out.pixels.Data(), error);
    }

    const ImageDecoder& PngImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"png"};
        static const ImageDecoder kDecoder{"PNG", Span<const StringView>(kExtensions, 1), &MatchesPng, &DecodePngTexture};
        return kDecoder;
    }
}
