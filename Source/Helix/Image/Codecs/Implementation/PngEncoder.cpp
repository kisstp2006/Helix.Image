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

        // Applies one filter type to a row; returns the sum of absolute signed residuals, the usual cost heuristic.
        u64 FilterRow(u8 type, const u8* cur, const u8* prior, usize n, u32 bpp, u8* dst)
        {
            u64 cost = 0;
            for (usize i = 0; i < n; i++)
            {
                u8 left = i >= bpp ? cur[i - bpp] : 0, up = prior ? prior[i] : 0, corner = prior && i >= bpp ? prior[i - bpp] : 0;
                u8 predict = type == 0 ? 0 : type == 1 ? left : type == 2 ? up : type == 3 ? u8((u32(left) + up) >> 1) : Paeth(left, up, corner);
                u8 v = u8(cur[i] - predict);
                dst[i] = v;
                cost += v < 128 ? v : 256 - v;
            }
            return cost;
        }
    }

    void EncodePng(const Image& image, Array<u8>& out, int compressionLevel)
    {
        static constexpr u8 kColorType[4] = {0, 4, 2, 6};
        u32 bpp = ChannelCount(image.format);
        usize rowBytes = image.RowBytes();
        Array<u8> filtered;
        filtered.Resize((rowBytes + 1) * image.height);
        Array<u8> trial;
        trial.Resize(rowBytes);
        for (u32 y = 0; y < image.height; y++)
        {
            const u8* cur = image.pixels.Data() + y * rowBytes;
            const u8* prior = y ? cur - rowBytes : nullptr;
            u8* dst = filtered.Data() + y * (rowBytes + 1);
            u64 best = ~0ull;
            for (u8 type = 0; type < 5; type++)
            {
                u64 cost = FilterRow(type, cur, prior, rowBytes, bpp, trial.Data());
                if (cost < best)
                {
                    best = cost;
                    dst[0] = type;
                    std::memcpy(dst + 1, trial.Data(), rowBytes);
                }
            }
        }
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
