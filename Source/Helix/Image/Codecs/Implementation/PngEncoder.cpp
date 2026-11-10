#include <Helix/Image/Codecs/Implementation/BuiltInEncoders.h>
#include <Helix/Image/Codecs/Implementation/PngCommon.h>
#include <Helix/Image/Codecs/Png.h>
#include <Helix/Image/Compression/Deflate.h>

#include <Helix/Core/Hashing/Hash.h>
#include <Helix/Jobs/Jobs.h>

#include <cstring>

namespace helix::image
{
    namespace
    {
        using namespace png;

        void PutBe32(Array<u8>& out, u32 v)
        {
            for (int shift = 24; shift >= 0; shift -= 8)
                out.Push(u8(v >> shift));
        }

        void Chunk(Array<u8>& out, const char* type, const u8* body, usize length)
        {
            PutBe32(out, u32(length));
            usize start = out.Size();
            for (int i = 0; i < 4; i++)
                out.Push(u8(type[i]));
            usize at = out.Size();
            out.Resize(at + length);
            if (length)
                std::memcpy(out.Data() + at, body, length);
            PutBe32(out, Crc32(out.Data() + start, length + 4));
        }

        u64 Cost(const u8* row, usize n)
        {
            u64 cost = 0;
            for (usize i = 0; i < n; i++)
                cost += row[i] < 128 ? row[i] : 256 - row[i];
            return cost;
        }

        // Applies one filter type to a row (prior is a zero row for the first one) and returns the sum of absolute signed
        // residuals, the usual cost heuristic. Each type has its own loop without per byte edge checks.
        u64 FilterRow(u8 type, const u8* cur, const u8* prior, usize n, u32 bpp, u8* dst)
        {
            switch (type)
            {
            case 0: std::memcpy(dst, cur, n); break;
            case 1:
                std::memcpy(dst, cur, bpp);
                for (usize i = bpp; i < n; i++)
                    dst[i] = u8(cur[i] - cur[i - bpp]);
                break;
            case 2:
                for (usize i = 0; i < n; i++)
                    dst[i] = u8(cur[i] - prior[i]);
                break;
            case 3:
                for (usize i = 0; i < bpp; i++)
                    dst[i] = u8(cur[i] - (prior[i] >> 1));
                for (usize i = bpp; i < n; i++)
                    dst[i] = u8(cur[i] - ((u32(cur[i - bpp]) + prior[i]) >> 1));
                break;
            default:
                for (usize i = 0; i < bpp; i++)
                    dst[i] = u8(cur[i] - prior[i]);
                for (usize i = bpp; i < n; i++)
                    dst[i] = u8(cur[i] - Paeth(cur[i - bpp], prior[i], prior[i - bpp]));
                break;
            }
            return Cost(dst, n);
        }
    }

    namespace
    {
        struct FilterContext
        {
            const u8* pixels;
            u8* filtered;
            usize rowBytes;
            u32 bpp;
        };

        // Rows only read the source image, so any range of rows can be filtered on its own.
        void FilterRows(void* user, u32 begin, u32 end)
        {
            const FilterContext& c = *static_cast<const FilterContext*>(user);
            Array<u8> trial, zeroRow;
            trial.Resize(c.rowBytes);
            zeroRow.Resize(c.rowBytes);
            for (u32 y = begin; y < end; y++)
            {
                const u8* cur = c.pixels + y * c.rowBytes;
                const u8* prior = y ? cur - c.rowBytes : zeroRow.Data();
                u8* dst = c.filtered + y * (c.rowBytes + 1);
                u64 best = ~0ull;
                for (u8 type = 0; type < 5; type++)
                {
                    u64 cost = FilterRow(type, cur, prior, c.rowBytes, c.bpp, trial.Data());
                    if (cost < best)
                    {
                        best = cost;
                        dst[0] = type;
                        std::memcpy(dst + 1, trial.Data(), c.rowBytes);
                    }
                }
            }
        }
    }

    namespace
    {
        // Rows of `channels` samples of `depth` bits (16 bit samples big endian) to a PNG of the matching color type.
        void WritePng(const u8* pixels, u32 width, u32 height, u32 channels, u32 depth, int compressionLevel, Array<u8>& out)
        {
            static constexpr u8 kColorType[4] = {0, 4, 2, 6};
            u32 bpp = channels * depth / 8;
            usize rowBytes = usize(width) * bpp;
            Array<u8> filtered;
            filtered.AppendUninitialized((rowBytes + 1) * height);
            FilterContext context{pixels, filtered.Data(), rowBytes, bpp};
            constexpr u32 kRowsPerJob = 32;
            if (jobs::IsInitialized() && height > 2 * kRowsPerJob)
                jobs::Complete(jobs::ScheduleParallelFor(&FilterRows, &context, height, kRowsPerJob));
            else
                FilterRows(&context, 0, height);
            Array<u8> compressed;
            ZlibCompress(filtered, compressed, compressionLevel);

            for (u8 b : kSignature)
                out.Push(b);
            u8 ihdr[13] = {u8(width >> 24),  u8(width >> 16),  u8(width >> 8),  u8(width),       u8(height >> 24), u8(height >> 16), u8(height >> 8),
                           u8(height),       u8(depth),        kColorType[channels - 1], 0, 0, 0};
            Chunk(out, "IHDR", ihdr, 13);
            Chunk(out, "IDAT", compressed.Data(), compressed.Size());
            Chunk(out, "IEND", nullptr, 0);
        }

        // Gray, RGB or RGBA from the channels of the format (two channel formats as RGB with blue 0), 16 bit samples for
        // formats with more than 8 bits per channel.
        bool EncodePngTexture(const TextureData& texture, const EncodeOptions&, Array<u8>& out, String& error)
        {
            const PixelFormatInfo& info = FormatInfo(texture.format);
            bool wide = info.kind == PixelKind::Float || info.kind == PixelKind::Uint || (info.kind != PixelKind::Block && info.bytesPerBlock / Max<u32>(info.channels, 1) >= 2) ||
                        texture.format == PixelFormat::RGB10A2 || texture.format == PixelFormat::RG11B10Float || texture.format == PixelFormat::RGB9E5Float;
            u32 channels = info.channels == 1 ? 1 : info.channels == 4 ? 4 : 3;
            static constexpr PixelFormat kNarrow[4] = {PixelFormat::R8, PixelFormat::RG8, PixelFormat::RGB8, PixelFormat::RGBA8};
            static constexpr PixelFormat kWide[4] = {PixelFormat::R16, PixelFormat::RG16, PixelFormat::RGB16, PixelFormat::RGBA16};
            Array<u8> pixels;
            if (!FirstSurface(texture, (wide ? kWide : kNarrow)[channels - 1], pixels, error))
                return false;
            if (wide)
                for (usize i = 0; i + 1 < pixels.Size(); i += 2)
                {
                    u8 t = pixels[i];
                    pixels[i] = pixels[i + 1];
                    pixels[i + 1] = t;
                }
            WritePng(pixels.Data(), texture.width, texture.height, channels, wide ? 16 : 8, 6, out);
            return true;
        }
    }

    void EncodePng(const Image& image, Array<u8>& out, int compressionLevel)
    {
        WritePng(image.pixels.Data(), image.width, image.height, ChannelCount(image.format), 8, compressionLevel, out);
    }

    const ImageEncoder& PngImageEncoder()
    {
        static constexpr StringView kExtensions[] = {"png"};
        static const ImageEncoder kEncoder{"PNG", Span<const StringView>(kExtensions, 1), &EncodePngTexture};
        return kEncoder;
    }
}
