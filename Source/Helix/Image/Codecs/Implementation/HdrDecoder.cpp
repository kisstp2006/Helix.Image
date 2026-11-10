#include <Helix/Image/Codecs/Implementation/BuiltInDecoders.h>

#include <cmath>
#include <cstring>

// Radiance RGBE (.hdr): a text header, a resolution line and scanlines that are flat, in the old run length form or in
// the per channel run length form.
namespace helix::image
{
    namespace
    {
        bool StartsWith(Span<const u8> data, const char* text)
        {
            usize n = std::strlen(text);
            return data.Size() >= n && std::memcmp(data.Data(), text, n) == 0;
        }

        bool MatchesHdr(Span<const u8> data) { return StartsWith(data, "#?RADIANCE") || StartsWith(data, "#?RGBE"); }

        // Reads one header line without its end; false at the end of the file.
        bool Line(Span<const u8> data, usize& at, StringView& line)
        {
            if (at >= data.Size())
                return false;
            usize start = at;
            while (at < data.Size() && data[at] != '\n')
                at++;
            line = StringView(reinterpret_cast<const char*>(data.Data()) + start, at - start);
            if (at < data.Size())
                at++;
            return true;
        }

        bool Number(StringView text, usize& at, u32& value)
        {
            while (at < text.Size() && text[at] == ' ')
                at++;
            usize start = at;
            u64 v = 0;
            while (at < text.Size() && text[at] >= '0' && text[at] <= '9' && v < (1u << 30))
                v = v * 10 + u64(text[at++] - '0');
            value = u32(v);
            return at > start && v < (1u << 30);
        }

        // "-Y h +X w" is top down, "+Y h +X w" bottom up; a negative X mirrors the rows.
        bool Resolution(StringView line, u32& width, u32& height, bool& bottomUp, bool& mirrored)
        {
            usize at = 0;
            auto axis = [&](char name, bool& negative) {
                while (at < line.Size() && line[at] == ' ')
                    at++;
                if (at + 2 > line.Size() || (line[at] != '-' && line[at] != '+') || line[at + 1] != name)
                    return false;
                negative = line[at] == '-';
                at += 2;
                return true;
            };
            bool yNegative = false, xNegative = false;
            if (!axis('Y', yNegative) || !Number(line, at, height) || !axis('X', xNegative) || !Number(line, at, width))
                return false;
            bottomUp = !yNegative;
            mirrored = xNegative;
            return width && height;
        }

        float Channel(u8 value, int exponent) { return std::ldexp(float(value) + 0.5f, exponent); }

        bool ReadScanline(Span<const u8> data, usize& at, u32 width, u8* rgbe)
        {
            const u8* p = data.Data();
            usize size = data.Size();
            // The per channel form: 2, 2, then the width in two bytes, then each channel as runs and literals.
            if (width >= 8 && width < 0x8000 && at + 4 <= size && p[at] == 2 && p[at + 1] == 2 && !(p[at + 2] & 0x80))
            {
                if ((u32(p[at + 2]) << 8 | p[at + 3]) != width)
                    return false;
                at += 4;
                for (u32 c = 0; c < 4; c++)
                    for (u32 x = 0; x < width;)
                    {
                        if (at >= size)
                            return false;
                        u32 count = p[at++];
                        if (count > 128)
                        {
                            count -= 128;
                            if (at >= size || x + count > width)
                                return false;
                            u8 value = p[at++];
                            for (u32 i = 0; i < count; i++)
                                rgbe[(x++) * 4 + c] = value;
                        }
                        else
                        {
                            if (count == 0 || x + count > width || at + count > size)
                                return false;
                            for (u32 i = 0; i < count; i++)
                                rgbe[(x++) * 4 + c] = p[at++];
                        }
                    }
                return true;
            }
            // Flat pixels, where 1, 1, 1, n repeats the previous pixel n << shift times.
            u32 shift = 0;
            for (u32 x = 0; x < width;)
            {
                if (at + 4 > size)
                    return false;
                const u8* q = p + at;
                at += 4;
                if (q[0] == 1 && q[1] == 1 && q[2] == 1)
                {
                    if (x == 0 || shift > 24)
                        return false;
                    u64 count = u64(q[3]) << shift;
                    if (x + count > width)
                        return false;
                    for (u64 i = 0; i < count; i++, x++)
                        std::memcpy(rgbe + x * 4, rgbe + (x - 1) * 4, 4);
                    shift += 8;
                    continue;
                }
                std::memcpy(rgbe + x * 4, q, 4);
                x++;
                shift = 0;
            }
            return true;
        }

        bool DecodeHdr(Span<const u8> data, TextureData& out, String& error)
        {
            usize at = 0;
            StringView line;
            bool rgbe = true;
            Line(data, at, line);
            while (true)
            {
                if (!Line(data, at, line))
                {
                    error = "the header has no end";
                    return false;
                }
                if (line.Empty())
                    break;
                if (line == StringView("FORMAT=32-bit_rle_xyze"))
                    rgbe = false;
            }
            if (!rgbe)
            {
                error = "XYZE colors are not supported";
                return false;
            }
            u32 width = 0, height = 0;
            bool bottomUp = false, mirrored = false;
            if (!Line(data, at, line) || !Resolution(line, width, height, bottomUp, mirrored))
            {
                error = "no valid resolution line";
                return false;
            }
            if (!out.Allocate(PixelFormat::RGB32Float, width, height))
            {
                error = "the image is too large";
                return false;
            }
            out.colorSpace = ColorSpace::Linear;
            Array<u8> row;
            row.Resize(usize(width) * 4);
            float* pixels = reinterpret_cast<float*>(out.data.Data());
            for (u32 y = 0; y < height; y++)
            {
                if (!ReadScanline(data, at, width, row.Data()))
                {
                    error = "a scanline is damaged or cut";
                    return false;
                }
                float* dst = pixels + usize(bottomUp ? height - 1 - y : y) * width * 3;
                for (u32 x = 0; x < width; x++)
                {
                    const u8* q = row.Data() + usize(mirrored ? width - 1 - x : x) * 4;
                    float* d = dst + usize(x) * 3;
                    if (q[3] == 0)
                        d[0] = d[1] = d[2] = 0.0f;
                    else
                    {
                        int exponent = int(q[3]) - 136;
                        d[0] = Channel(q[0], exponent);
                        d[1] = Channel(q[1], exponent);
                        d[2] = Channel(q[2], exponent);
                    }
                }
            }
            return true;
        }
    }

    const ImageDecoder& HdrImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"hdr", "rgbe", "pic"};
        static const ImageDecoder kDecoder{"HDR", Span<const StringView>(kExtensions, 3), &MatchesHdr, &DecodeHdr};
        return kDecoder;
    }
}
