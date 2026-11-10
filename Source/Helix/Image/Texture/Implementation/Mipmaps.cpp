#include <Helix/Image/Texture/Color.h>
#include <Helix/Image/Texture/Convert.h>
#include <Helix/Image/Texture/Mipmaps.h>
#include <Helix/Image/Texture/Resize.h>

#include <Helix/Jobs/Jobs.h>

#include <cmath>
#include <cstring>

namespace helix::image
{
    namespace
    {
        bool EightBit(PixelFormat f) { return f == PixelFormat::R8 || f == PixelFormat::RG8 || f == PixelFormat::RGB8 || f == PixelFormat::RGBA8; }

        struct Conversion
        {
            PixelFormat format;
            bool srgb;
            const u8* bytes;
            u8* out;
            float* rgba;
            usize count;
        };

        constexpr usize kChunk = 16384;

        // Stored texels to working RGBA floats, in linear light for sRGB data; 8 bit formats read a table.
        void DecodeChunk(const Conversion& c, usize chunk)
        {
            usize at = chunk * kChunk, n = c.count - at < kChunk ? c.count - at : kChunk;
            float* dst = c.rgba + at * 4;
            if (c.srgb && EightBit(c.format))
            {
                u32 channels = FormatInfo(c.format).channels;
                const u8* src = c.bytes + at * channels;
                for (usize i = 0; i < n; i++, src += channels, dst += 4)
                {
                    dst[0] = SrgbByteToLinear(src[0]);
                    dst[1] = channels > 1 ? SrgbByteToLinear(src[1]) : 0.0f;
                    dst[2] = channels > 2 ? SrgbByteToLinear(src[2]) : 0.0f;
                    dst[3] = channels > 3 ? float(src[3]) / 255.0f : 1.0f;
                }
                return;
            }
            ToFloat(c.format, c.bytes + at * FormatInfo(c.format).bytesPerBlock, dst, n);
            if (c.srgb)
                SrgbToLinear(dst, n);
        }

        void EncodeChunk(const Conversion& c, usize chunk)
        {
            usize at = chunk * kChunk, n = c.count - at < kChunk ? c.count - at : kChunk;
            float* src = c.rgba + at * 4;
            if (c.srgb && EightBit(c.format))
            {
                u32 channels = FormatInfo(c.format).channels;
                u8* dst = c.out + at * channels;
                for (usize i = 0; i < n; i++, src += 4, dst += channels)
                {
                    for (u32 k = 0; k < channels && k < 3; k++)
                        dst[k] = LinearToSrgbByte(src[k]);
                    if (channels > 3)
                        dst[3] = u8((src[3] > 0.0f ? (src[3] < 1.0f ? src[3] : 1.0f) : 0.0f) * 255.0f + 0.5f);
                }
                return;
            }
            if (c.srgb)
                LinearToSrgb(src, n);
            FromFloat(c.format, src, c.out + at * FormatInfo(c.format).bytesPerBlock, n);
        }

        template <void (*Chunk)(const Conversion&, usize)>
        void Convert(const Conversion& c)
        {
            u32 chunks = u32((c.count + kChunk - 1) / kChunk);
            if (jobs::IsInitialized() && chunks > 1)
            {
                const Conversion* shared = &c;
                jobs::Complete(jobs::ParallelFor(chunks, 1, [shared](u32 i) { Chunk(*shared, i); }));
            }
            else
                for (u32 i = 0; i < chunks; i++)
                    Chunk(c, i);
        }

        // The share of texels whose alpha, scaled, passes the reference.
        double Coverage(const Array<float>& rgba, float reference, float scale)
        {
            usize passing = 0, count = rgba.Size() / 4;
            for (usize i = 0; i < count; i++)
                passing += rgba[i * 4 + 3] * scale > reference;
            return count ? double(passing) / double(count) : 0.0;
        }

        // Scales alpha so the level has the coverage of the top level: the scale is searched by halving its range.
        void KeepCoverage(Array<float>& rgba, float reference, double target)
        {
            float low = 0.0f, high = 4.0f;
            for (int i = 0; i < 20; i++)
            {
                float middle = (low + high) * 0.5f;
                if (Coverage(rgba, reference, middle) < target)
                    low = middle;
                else
                    high = middle;
            }
            // Of the two ends of the range, the one closer to the target.
            float scale = std::fabs(Coverage(rgba, reference, low) - target) < std::fabs(Coverage(rgba, reference, high) - target) ? low : high;
            for (usize i = 3; i < rgba.Size(); i += 4)
            {
                float a = rgba[i] * scale;
                rgba[i] = a > 1.0f ? 1.0f : a;
            }
        }

        void Renormalize(Array<float>& rgba)
        {
            for (usize i = 0; i < rgba.Size(); i += 4)
            {
                float x = rgba[i] * 2.0f - 1.0f, y = rgba[i + 1] * 2.0f - 1.0f, z = rgba[i + 2] * 2.0f - 1.0f;
                float length = std::sqrt(x * x + y * y + z * z);
                if (length < 1e-12f)
                {
                    x = y = 0.0f;
                    z = length = 1.0f;
                }
                rgba[i] = (x / length) * 0.5f + 0.5f;
                rgba[i + 1] = (y / length) * 0.5f + 0.5f;
                rgba[i + 2] = (z / length) * 0.5f + 0.5f;
            }
        }

        // The edge texels of a level take the top level's edge texel at the same relative place.
        void Border(Array<float>& level, u32 w, u32 h, const Array<float>& top, u32 topW, u32 topH)
        {
            auto copy = [&](u32 x, u32 y) {
                u32 tx = w > 1 ? u32(u64(x) * (topW - 1) / (w - 1)) : 0, ty = h > 1 ? u32(u64(y) * (topH - 1) / (h - 1)) : 0;
                std::memcpy(level.Data() + (usize(y) * w + x) * 4, top.Data() + (usize(ty) * topW + tx) * 4, 16);
            };
            for (u32 x = 0; x < w; x++)
            {
                copy(x, 0);
                copy(x, h - 1);
            }
            for (u32 y = 0; y < h; y++)
            {
                copy(0, y);
                copy(w - 1, y);
            }
        }

        void Fade(Array<float>& level, u32 mip, const MipmapSettings& s)
        {
            if (!s.fadeout || mip < s.fadeStart)
                return;
            float t = s.fadeEnd > s.fadeStart ? float(mip - s.fadeStart) / float(s.fadeEnd - s.fadeStart) : 1.0f;
            t = t > 1.0f ? 1.0f : t;
            // Gray as stored: 0.5, in linear light for sRGB data; normal maps fade to a flat normal.
            float gray = s.srgb ? SrgbToLinear(0.5f) : 0.5f;
            float target[3] = {s.normalMap ? 0.5f : gray, s.normalMap ? 0.5f : gray, s.normalMap ? 1.0f : gray};
            for (usize i = 0; i < level.Size(); i += 4)
                for (int c = 0; c < 3; c++)
                    level[i + c] += (target[c] - level[i + c]) * t;
        }
    }

    u32 MipCount(u32 width, u32 height)
    {
        u32 larger = width > height ? width : height, count = 1;
        while (larger > 1)
        {
            larger >>= 1;
            count++;
        }
        return count;
    }

    bool GenerateMipmaps(const TextureData& source, const MipmapSettings& settings, TextureData& out, String& error)
    {
        if (!CanConvert(source.format) || source.depth != 1)
        {
            error = "mips are made for uncompressed 2D, cube and array textures";
            return false;
        }
        u32 mips = MipCount(source.width, source.height);
        if (!out.Allocate(source.format, source.width, source.height, 1, source.layers, source.faces, mips))
        {
            error = "the texture is empty or too large";
            return false;
        }
        out.colorSpace = source.colorSpace;
        out.dimension = source.dimension;
        ResampleFilter filter = settings.filter == MipFilter::Kaiser ? ResampleFilter::Kaiser : ResampleFilter::Box;
        bool srgb = settings.srgb && !settings.normalMap;
        MipmapSettings s = settings;
        s.srgb = srgb;
        Array<float> top, above, level;
        for (u32 layer = 0; layer < source.layers; layer++)
            for (u32 face = 0; face < source.faces; face++)
            {
                usize count = usize(source.width) * source.height;
                top.Resize(count * 4);
                Convert<&DecodeChunk>({source.format, srgb, source.Pixels(layer, face, 0), nullptr, top.Data(), count});
                double coverage = settings.preserveCoverage ? Coverage(top, settings.alphaReference, 1.0f) : 0.0;
                u32 w = source.width, h = source.height;
                for (u32 mip = 0; mip < mips; mip++)
                {
                    if (mip > 0)
                    {
                        u32 nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
                        level.Resize(usize(nw) * nh * 4);
                        Resample(mip == 1 ? top.Data() : above.Data(), w, h, level.Data(), nw, nh, filter);
                        w = nw;
                        h = nh;
                        if (settings.normalMap)
                            Renormalize(level);
                        if (settings.preserveCoverage)
                            KeepCoverage(level, settings.alphaReference, coverage);
                        // The next level filters this one before the border and the fade change it.
                        above = level;
                        if (settings.borderMipmap)
                            Border(level, w, h, top, source.width, source.height);
                    }
                    else if (s.fadeout && s.fadeStart == 0)
                        level = top;
                    else
                    {
                        // The top level is the source, byte for byte.
                        std::memcpy(out.Pixels(layer, face, 0), source.Pixels(layer, face, 0), usize(out.At(layer, face, 0).size));
                        continue;
                    }
                    Fade(level, mip, s);
                    Convert<&EncodeChunk>({out.format, srgb, nullptr, out.Pixels(layer, face, mip), level.Data(), usize(w) * h});
                }
            }
        return true;
    }
}
