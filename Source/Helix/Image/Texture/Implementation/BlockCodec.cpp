#include <Helix/Image/Texture/BlockCodec.h>
#include <Helix/Image/Texture/Convert.h>
#include <Helix/Image/Texture/Implementation/BuiltInBlockCodecs.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/Jobs/Jobs.h>
#include <Helix/Platform/Threading/Thread.h>

#include <cstring>

namespace helix::image
{
    namespace
    {
        struct Registry
        {
            platform::Mutex mutex;
            Array<BlockCodec> codecs;

            Registry()
            {
                usize count = 0;
                const BlockCodec* builtIn = BuiltInBlockCodecs(count);
                for (usize i = 0; i < count; i++)
                    codecs.Push(builtIn[i]);
            }
        };

        Registry& Codecs()
        {
            static Registry registry;
            return registry;
        }

        // One surface (one depth slice) worth of blocks, as rows of blocks: blocks to texels or texels to blocks.
        struct Surface
        {
            const BlockCodec* codec;
            const u8* from;
            u8* to;
            u32 width, height, columns;
            CompressionQuality quality;
        };

        void DecodeRow(const Surface& s, u32 row)
        {
            const PixelFormatInfo& block = FormatInfo(s.codec->format);
            u32 bw = block.blockWidth, bh = block.blockHeight, texelBytes = FormatInfo(s.codec->decodedFormat).bytesPerBlock;
            u8 texels[12 * 12 * 16];
            for (u32 column = 0; column < s.columns; column++)
            {
                s.codec->decode(s.from + (usize(row) * s.columns + column) * block.bytesPerBlock, texels);
                for (u32 y = 0; y < bh && row * bh + y < s.height; y++)
                {
                    u32 x0 = column * bw, n = x0 + bw <= s.width ? bw : s.width - x0;
                    std::memcpy(s.to + (usize(row * bh + y) * s.width + x0) * texelBytes, texels + usize(y) * bw * texelBytes, usize(n) * texelBytes);
                }
            }
        }

        void EncodeRow(const Surface& s, u32 row)
        {
            const PixelFormatInfo& block = FormatInfo(s.codec->format);
            u32 bw = block.blockWidth, bh = block.blockHeight, texelBytes = FormatInfo(s.codec->decodedFormat).bytesPerBlock;
            u8 texels[12 * 12 * 16];
            for (u32 column = 0; column < s.columns; column++)
            {
                for (u32 y = 0; y < bh; y++)
                    for (u32 x = 0; x < bw; x++)
                    {
                        u32 sx = column * bw + x, sy = row * bh + y;
                        sx = sx < s.width ? sx : s.width - 1;
                        sy = sy < s.height ? sy : s.height - 1;
                        std::memcpy(texels + (usize(y) * bw + x) * texelBytes, s.from + (usize(sy) * s.width + sx) * texelBytes, texelBytes);
                    }
                s.codec->encode(texels, s.to + (usize(row) * s.columns + column) * block.bytesPerBlock, s.quality);
            }
        }

        template <void (*Row)(const Surface&, u32)>
        void Rows(const Surface& s, u32 rows)
        {
            if (jobs::IsInitialized() && rows > 1)
            {
                const Surface* shared = &s;
                jobs::Complete(jobs::ParallelFor(rows, 1, [shared](u32 row) { Row(*shared, row); }));
            }
            else
                for (u32 row = 0; row < rows; row++)
                    Row(s, row);
        }
    }

    void RegisterBlockCodec(const BlockCodec& codec)
    {
        Registry& r = Codecs();
        platform::ScopedLock lock(r.mutex);
        for (BlockCodec& c : r.codecs)
            if (c.format == codec.format)
            {
                c = codec;
                return;
            }
        r.codecs.Push(codec);
    }

    bool FindBlockCodec(PixelFormat format, BlockCodec& out)
    {
        Registry& r = Codecs();
        platform::ScopedLock lock(r.mutex);
        for (const BlockCodec& c : r.codecs)
            if (c.format == format)
            {
                out = c;
                return true;
            }
        return false;
    }

    bool DecompressTexture(const TextureData& source, TextureData& out, String& error)
    {
        BlockCodec codec;
        if (!FindBlockCodec(source.format, codec) || !codec.decode)
        {
            error = Format("no decoder for {}", FormatInfo(source.format).name);
            return false;
        }
        if (!out.Allocate(codec.decodedFormat, source.width, source.height, source.depth, source.layers, source.faces, source.mips))
        {
            error = "the texture is empty or too large";
            return false;
        }
        out.colorSpace = source.colorSpace;
        out.dimension = source.dimension;
        const PixelFormatInfo& block = FormatInfo(source.format);
        for (u32 i = 0; i < source.subresources.Size(); i++)
        {
            const Subresource& from = source.subresources[i];
            const Subresource& to = out.subresources[i];
            u64 slice = SurfaceBytes(source.format, from.width, from.height), decodedSlice = to.size / to.depth;
            for (u32 z = 0; z < from.depth; z++)
            {
                Surface s{&codec, source.data.Data() + from.offset + z * slice, out.data.Data() + to.offset + z * decodedSlice, from.width, from.height, (from.width + block.blockWidth - 1) / block.blockWidth, CompressionQuality::Normal};
                Rows<&DecodeRow>(s, (from.height + block.blockHeight - 1) / block.blockHeight);
            }
        }
        return true;
    }

    bool CompressTexture(const TextureData& source, PixelFormat format, CompressionQuality quality, TextureData& out, String& error)
    {
        BlockCodec codec;
        if (!FindBlockCodec(format, codec) || !codec.encode)
        {
            error = Format("no encoder for {}", FormatInfo(format).name);
            return false;
        }
        if (!CanConvert(source.format))
        {
            error = "the source must be uncompressed";
            return false;
        }
        if (!out.Allocate(format, source.width, source.height, source.depth, source.layers, source.faces, source.mips))
        {
            error = "the texture is empty or too large";
            return false;
        }
        out.colorSpace = source.colorSpace;
        out.dimension = source.dimension;
        const PixelFormatInfo& block = FormatInfo(format);
        Array<u8> texels;
        for (u32 i = 0; i < source.subresources.Size(); i++)
        {
            const Subresource& from = source.subresources[i];
            const Subresource& to = out.subresources[i];
            usize count = usize(from.width) * from.height;
            texels.Resize(count * FormatInfo(codec.decodedFormat).bytesPerBlock);
            u64 slice = SurfaceBytes(source.format, from.width, from.height), blockSlice = to.size / to.depth;
            for (u32 z = 0; z < from.depth; z++)
            {
                ConvertPixels(source.format, source.data.Data() + from.offset + z * slice, codec.decodedFormat, texels.Data(), count);
                Surface s{&codec, texels.Data(), out.data.Data() + to.offset + z * blockSlice, from.width, from.height,
                          (from.width + block.blockWidth - 1) / block.blockWidth, quality};
                Rows<&EncodeRow>(s, (from.height + block.blockHeight - 1) / block.blockHeight);
            }
        }
        return true;
    }
}
