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

    void EncodePng(const Image& image, Array<u8>& out, int compressionLevel)
    {
        static constexpr u8 kColorType[4] = {0, 4, 2, 6};
        u32 bpp = ChannelCount(image.format);
        usize rowBytes = image.RowBytes();
        Array<u8> filtered;
        filtered.AppendUninitialized((rowBytes + 1) * image.height);
        FilterContext context{image.pixels.Data(), filtered.Data(), rowBytes, bpp};
        constexpr u32 kRowsPerJob = 32;
        if (jobs::IsInitialized() && image.height > 2 * kRowsPerJob)
            jobs::Complete(jobs::ScheduleParallelFor(&FilterRows, &context, image.height, kRowsPerJob));
        else
            FilterRows(&context, 0, image.height);
        Array<u8> compressed;
        ZlibCompress(filtered, compressed, compressionLevel);

        for (u8 b : kSignature)
            out.Push(b);
        u8 ihdr[13] = {u8(image.width >> 24),  u8(image.width >> 16),  u8(image.width >> 8),  u8(image.width),
                       u8(image.height >> 24), u8(image.height >> 16), u8(image.height >> 8), u8(image.height),
                       8,                      kColorType[u32(image.format)], 0, 0, 0};
        Chunk(out, "IHDR", ihdr, 13);
        Chunk(out, "IDAT", compressed.Data(), compressed.Size());
        Chunk(out, "IEND", nullptr, 0);
    }
}
