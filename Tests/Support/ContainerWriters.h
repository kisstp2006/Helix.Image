#pragma once

#include <Support/ImageWriters.h>

#include <Helix/Image/Compression/Deflate.h>
#include <Helix/Image/Texture/TextureData.h>

#include <cstring>

// Writers of DDS, KTX, KTX2 and PVR files from a TextureData, in the subresource order each format uses.
namespace helix::image::testing
{
    // Fills every byte of every subresource with a pattern that differs per layer, face, mip and position.
    inline void FillPattern(TextureData& t)
    {
        for (u32 layer = 0; layer < t.layers; layer++)
            for (u32 face = 0; face < t.faces; face++)
                for (u32 mip = 0; mip < t.mips; mip++)
                {
                    const Subresource& s = t.At(layer, face, mip);
                    for (u64 i = 0; i < s.size; i++)
                        t.data[usize(s.offset + i)] = u8(i * 7 + layer * 31 + face * 17 + mip * 101);
                }
    }

    inline void PutSurface(Array<u8>& out, const TextureData& t, u32 layer, u32 face, u32 mip)
    {
        const Subresource& s = t.At(layer, face, mip);
        out.Append(t.data.Data() + s.offset, usize(s.size));
    }

    // Legacy header with a FourCC or pixel masks (fourCC 0), or the DX10 header (dxgi not 0).
    inline Array<u8> WriteDds(const TextureData& t, u32 fourCC, u32 dxgi, const u32 masks[5] = nullptr)
    {
        Array<u8> out;
        PutText(out, "DDS ");
        PutLe32(out, 124);
        PutLe32(out, 0x1 | 0x2 | 0x4 | 0x1000 | (t.mips > 1 ? 0x20000 : 0) | (t.depth > 1 ? 0x800000 : 0));
        PutLe32(out, t.height);
        PutLe32(out, t.width);
        PutLe32(out, 0);
        PutLe32(out, t.depth);
        PutLe32(out, t.mips);
        for (int i = 0; i < 11; i++)
            PutLe32(out, 0);
        PutLe32(out, 32);
        bool dx10 = dxgi != 0;
        PutLe32(out, masks ? 0x40 | (masks[4] ? 1 : 0) : 0x4);
        PutLe32(out, dx10 ? u32('D') | u32('X') << 8 | u32('1') << 16 | u32('0') << 24 : fourCC);
        for (int i = 0; i < 5; i++)
            PutLe32(out, masks ? masks[i] : 0);
        PutLe32(out, 0x1000 | (t.mips > 1 ? 0x400008 : 0));
        PutLe32(out, (t.faces == 6 && !dx10 ? 0xFE00 : 0) | (t.depth > 1 ? 0x200000 : 0));
        PutLe32(out, 0);
        PutLe32(out, 0);
        PutLe32(out, 0);
        if (dx10)
        {
            PutLe32(out, dxgi);
            PutLe32(out, t.depth > 1 ? 4 : 3);
            PutLe32(out, t.faces == 6 ? 4 : 0);
            PutLe32(out, t.layers);
            PutLe32(out, 0);
        }
        for (u32 layer = 0; layer < t.layers; layer++)
            for (u32 face = 0; face < t.faces; face++)
                for (u32 mip = 0; mip < t.mips; mip++)
                    PutSurface(out, t, layer, face, mip);
        return out;
    }

    // KTX 1, little endian; uncompressed rows are padded to 4 bytes. `arrayed` writes the layer count even for one.
    inline Array<u8> WriteKtx1(const TextureData& t, u32 internalFormat, u32 typeSize, bool arrayed)
    {
        static constexpr u8 kId[12] = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n'};
        Array<u8> out;
        out.Append(kId, 12);
        PutLe32(out, 0x04030201);
        PutLe32(out, 0);
        PutLe32(out, typeSize);
        PutLe32(out, 0);
        PutLe32(out, internalFormat);
        PutLe32(out, 0);
        PutLe32(out, t.width);
        PutLe32(out, t.height);
        PutLe32(out, t.depth > 1 ? t.depth : 0);
        PutLe32(out, arrayed ? t.layers : 0);
        PutLe32(out, t.faces);
        PutLe32(out, t.mips);
        PutLe32(out, 8);
        PutText(out, "abc");
        Put8(out, 0);
        PutLe32(out, 0);
        const PixelFormatInfo& info = FormatInfo(t.format);
        for (u32 mip = 0; mip < t.mips; mip++)
        {
            usize sizeAt = out.Size();
            PutLe32(out, 0);
            usize start = out.Size();
            for (u32 layer = 0; layer < t.layers; layer++)
                for (u32 face = 0; face < t.faces; face++)
                {
                    const Subresource& s = t.At(layer, face, mip);
                    if (info.kind == PixelKind::Block)
                        PutSurface(out, t, layer, face, mip);
                    else
                    {
                        usize row = usize(s.width) * info.bytesPerBlock;
                        for (u32 y = 0; y < s.height * s.depth; y++)
                        {
                            out.Append(t.data.Data() + s.offset + y * row, row);
                            while ((out.Size() - start) % 4)
                                out.Push(0xEE);
                        }
                    }
                    if (t.faces == 6 && !arrayed)
                        while ((out.Size() - start) % 4)
                            out.Push(0xEE);
                }
            u32 size = u32(out.Size() - start);
            std::memcpy(out.Data() + sizeAt, &size, 4);
            while ((out.Size() - start) % 4)
                out.Push(0xEE);
        }
        return out;
    }

    // KTX2 with supercompression none (0) or zlib (3); levels are stored smallest first, as the format suggests.
    inline Array<u8> WriteKtx2(const TextureData& t, u32 vkFormat, u32 scheme)
    {
        static constexpr u8 kId[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
        Array<Array<u8>> levels;
        Array<u64> sizes;
        for (u32 mip = 0; mip < t.mips; mip++)
        {
            Array<u8> raw;
            for (u32 layer = 0; layer < t.layers; layer++)
                for (u32 face = 0; face < t.faces; face++)
                    PutSurface(raw, t, layer, face, mip);
            sizes.Push(raw.Size());
            Array<u8>& level = levels.Emplace();
            if (scheme == 3)
                ZlibCompress(Span<const u8>(raw.Data(), raw.Size()), level);
            else
                level = Move(raw);
        }
        Array<u8> out;
        out.Append(kId, 12);
        PutLe32(out, vkFormat);
        PutLe32(out, 1);
        PutLe32(out, t.width);
        PutLe32(out, t.height);
        PutLe32(out, t.depth > 1 ? t.depth : 0);
        PutLe32(out, t.layers > 1 ? t.layers : 0);
        PutLe32(out, t.faces);
        PutLe32(out, t.mips);
        PutLe32(out, scheme);
        for (int i = 0; i < 4; i++)
            PutLe32(out, 0);
        for (int i = 0; i < 4; i++)
            PutLe32(out, 0);
        usize index = out.Size();
        for (u32 i = 0; i < t.mips * 6; i++)
            PutLe32(out, 0);
        Array<u64> offsets;
        offsets.Resize(t.mips);
        for (u32 mip = t.mips; mip-- > 0;)
        {
            while (out.Size() % 16)
                out.Push(0);
            offsets[mip] = out.Size();
            out.Append(levels[mip].Data(), levels[mip].Size());
        }
        for (u32 mip = 0; mip < t.mips; mip++)
        {
            u64 values[3] = {offsets[mip], levels[mip].Size(), sizes[mip]};
            std::memcpy(out.Data() + index + mip * 24, values, 24);
        }
        return out;
    }

    // PVR 3 with a predefined code (high 0) or channel names and sizes.
    inline Array<u8> WritePvr(const TextureData& t, u32 low, u32 high, u32 channelType, u32 colorSpace)
    {
        Array<u8> out;
        PutLe32(out, 0x03525650);
        PutLe32(out, 0);
        PutLe32(out, low);
        PutLe32(out, high);
        PutLe32(out, colorSpace);
        PutLe32(out, channelType);
        PutLe32(out, t.height);
        PutLe32(out, t.width);
        PutLe32(out, t.depth);
        PutLe32(out, t.layers);
        PutLe32(out, t.faces);
        PutLe32(out, t.mips);
        PutLe32(out, 12);
        for (int i = 0; i < 12; i++)
            Put8(out, 0x55);
        for (u32 mip = 0; mip < t.mips; mip++)
            for (u32 layer = 0; layer < t.layers; layer++)
                for (u32 face = 0; face < t.faces; face++)
                    PutSurface(out, t, layer, face, mip);
        return out;
    }
}
