#include <Helix/Image/Codecs/Implementation/BuiltInDecoders.h>
#include <Helix/Image/Codecs/Implementation/ByteReader.h>
#include <Helix/Image/Compression/Deflate.h>

#include <Helix/Core/Strings/Format.h>

#include <cstring>

// Baseline TIFF, the first image of the file: strips or tiles; no compression, PackBits, LZW or Deflate; horizontal and
// floating point predictors; 1 to 16 bit unsigned and 32 bit float samples; gray, RGB and palette, with an alpha sample
// (associated alpha made straight).
namespace helix::image
{
    namespace
    {
        enum Tag : u16
        {
            kWidth = 256,
            kHeight = 257,
            kBitsPerSample = 258,
            kCompression = 259,
            kPhotometric = 262,
            kFillOrder = 266,
            kStripOffsets = 273,
            kSamplesPerPixel = 277,
            kRowsPerStrip = 278,
            kStripByteCounts = 279,
            kPlanar = 284,
            kPredictor = 317,
            kColorMap = 320,
            kTileWidth = 322,
            kTileLength = 323,
            kTileOffsets = 324,
            kTileByteCounts = 325,
            kExtraSamples = 338,
            kSampleFormat = 339,
        };

        struct Ifd
        {
            u32 width = 0, height = 0, samples = 1, bits = 1;
            u32 compression = 1, photometric = 1, planar = 1, predictor = 1, sampleFormat = 1, fillOrder = 1, extraSamples = 0;
            // The first extra sample is associated (premultiplied) alpha.
            bool associated = false;
            u32 rowsPerStrip = 0xFFFFFFFFu, tileWidth = 0, tileHeight = 0;
            Array<u64> offsets, counts, colorMap;
            bool tiled = false;
        };

        bool Fail(String& error, const char* what)
        {
            error = what;
            return false;
        }

        bool Fail(String& error, String what)
        {
            error = Move(what);
            return false;
        }

        bool MatchesTiff(Span<const u8> data)
        {
            return data.Size() >= 8 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) || (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42));
        }

        // The values of an entry of type BYTE, SHORT or LONG, inline or at their offset.
        bool Values(ByteReader& r, u16 type, u32 count, Array<u64>& out)
        {
            u32 size = type == 1 ? 1 : type == 3 ? 2 : type == 4 ? 4 : 0;
            if (!size || count > (1u << 24))
                return false;
            if (u64(size) * count > 4 && !r.Seek(r.U32()))
                return false;
            out.Clear();
            for (u32 i = 0; i < count && r.ok; i++)
                out.Push(size == 1 ? r.U8() : size == 2 ? r.U16() : r.U32());
            return r.ok;
        }

        bool ReadIfd(Span<const u8> data, Ifd& d, bool& bigEndian, String& error)
        {
            bigEndian = data[0] == 'M';
            ByteReader r(data, bigEndian);
            r.Skip(4);
            if (!r.Seek(r.U32()))
                return Fail(error, "the first directory is outside the file");
            u32 entries = r.U16();
            if (!r.ok || entries == 0 || entries > 4096)
                return Fail(error, "the first directory is empty or damaged");
            Array<u64> v;
            Array<u64> bitsPerSample, sampleFormats;
            for (u32 i = 0; i < entries; i++)
            {
                u16 tag = r.U16(), type = r.U16();
                u32 count = r.U32();
                if (!r.ok)
                    return Fail(error, "a directory entry is cut");
                usize following = r.at + 4;
                bool known = Values(r, type, count, v) && !v.Empty();
                r.at = following;
                r.ok = true;
                if (!known)
                    continue;
                switch (tag)
                {
                case kWidth: d.width = u32(v[0]); break;
                case kHeight: d.height = u32(v[0]); break;
                case kBitsPerSample: bitsPerSample = v; break;
                case kCompression: d.compression = u32(v[0]); break;
                case kPhotometric: d.photometric = u32(v[0]); break;
                case kFillOrder: d.fillOrder = u32(v[0]); break;
                case kStripOffsets: d.offsets = v; break;
                case kTileOffsets:
                    d.offsets = v;
                    d.tiled = true;
                    break;
                case kSamplesPerPixel: d.samples = u32(v[0]); break;
                case kRowsPerStrip: d.rowsPerStrip = u32(v[0]); break;
                case kStripByteCounts: case kTileByteCounts: d.counts = v; break;
                case kPlanar: d.planar = u32(v[0]); break;
                case kPredictor: d.predictor = u32(v[0]); break;
                case kColorMap: d.colorMap = v; break;
                case kTileWidth: d.tileWidth = u32(v[0]); break;
                case kTileLength: d.tileHeight = u32(v[0]); break;
                case kExtraSamples:
                    d.extraSamples = u32(v.Size());
                    d.associated = !v.Empty() && v[0] == 1;
                    break;
                case kSampleFormat: sampleFormats = v; break;
                default: break;
                }
            }
            if (!bitsPerSample.Empty())
            {
                d.bits = u32(bitsPerSample[0]);
                for (u64 b : bitsPerSample)
                    if (b != d.bits)
                        return Fail(error, "samples of different sizes");
            }
            if (!sampleFormats.Empty())
                d.sampleFormat = u32(sampleFormats[0]);
            if (!d.width || !d.height || d.width > (1u << 24) || d.height > (1u << 24))
                return Fail(error, "no valid image size");
            if (d.samples == 0 || d.samples > 8)
                return Fail(error, "an unsupported number of samples");
            bool fits = (d.sampleFormat == 1 && (d.bits == 1 || d.bits == 2 || d.bits == 4 || d.bits == 8 || d.bits == 16)) ||
                        (d.sampleFormat == 3 && d.bits == 32);
            if (!fits)
                return Fail(error, "an unsupported sample size or format");
            if (d.fillOrder != 1)
                return Fail(error, "reversed bit order is not supported");
            if (d.planar != 1 && d.planar != 2)
                return Fail(error, "an unknown planar configuration");
            if (d.tiled && (!d.tileWidth || !d.tileHeight || d.tileWidth % 16 || d.tileHeight % 16 || d.tileWidth > 8192 || d.tileHeight > 8192))
                return Fail(error, "an invalid tile size");
            if (u64(d.width) * d.height * d.samples * (d.bits > 8 ? d.bits / 8 : 1) > kMaxTextureBytes)
                return Fail(error, "the image is too large");
            return true;
        }

        bool PackBits(Span<const u8> in, u8* out, usize size)
        {
            usize at = 0, written = 0;
            while (written < size && at < in.Size())
            {
                i32 n = i8(in[at++]);
                if (n >= 0)
                {
                    usize count = usize(n) + 1;
                    if (at + count > in.Size() || written + count > size)
                        return false;
                    std::memcpy(out + written, in.Data() + at, count);
                    at += count;
                    written += count;
                }
                else if (n != -128)
                {
                    usize count = usize(1 - n);
                    if (at >= in.Size() || written + count > size)
                        return false;
                    std::memset(out + written, in[at++], count);
                    written += count;
                }
            }
            return written == size;
        }

        // TIFF LZW: codes from 9 to 12 bits, most significant bit first, the width growing one code early.
        bool Lzw(Span<const u8> in, u8* out, usize size)
        {
            if (in.Size() >= 2 && in[0] == 0 && (in[1] & 1))
                return false;
            static constexpr u32 kClear = 256, kEnd = 257;
            u16 prefix[4096];
            u8 suffix[4096], first[4096];
            u16 length[4096];
            for (u32 i = 0; i < 256; i++)
            {
                prefix[i] = 0xFFFF;
                suffix[i] = first[i] = u8(i);
                length[i] = 1;
            }
            u32 next = 258, width = 9, previous = 0xFFFFFFFFu;
            u64 bits = 0;
            u32 count = 0;
            usize at = 0, written = 0;
            while (written < size)
            {
                while (count < width && at < in.Size())
                {
                    bits = (bits << 8) | in[at++];
                    count += 8;
                }
                if (count < width)
                    break;
                u32 code = u32(bits >> (count - width)) & ((1u << width) - 1);
                count -= width;
                if (code == kEnd)
                    break;
                if (code == kClear)
                {
                    next = 258;
                    width = 9;
                    previous = 0xFFFFFFFFu;
                    continue;
                }
                u32 emit = code;
                u8 head;
                if (code < next && (code < 256 || code >= 258))
                    head = first[code];
                else if (code == next && previous != 0xFFFFFFFFu)
                {
                    head = first[previous];
                    emit = previous;
                }
                else
                    return false;
                // Writes the string of `emit` backwards, then the head byte again for the code that is being defined.
                usize n = length[emit];
                if (written + n > size)
                    n = size - written;
                u32 c = emit;
                for (usize i = length[emit]; i-- > 0;)
                {
                    if (i < n)
                        out[written + i] = suffix[c];
                    c = prefix[c];
                }
                written += n;
                if (code == next && written < size)
                    out[written++] = head;
                if (previous != 0xFFFFFFFFu && next < 4096)
                {
                    prefix[next] = u16(previous);
                    suffix[next] = head;
                    first[next] = first[previous];
                    length[next] = u16(length[previous] + 1);
                    next++;
                }
                if (next + 1 >= (1u << width) && width < 12)
                    width++;
                previous = code;
            }
            return written == size;
        }

        bool Decompress(const Ifd& d, Span<const u8> in, u8* out, usize size)
        {
            switch (d.compression)
            {
            case 1:
                if (in.Size() < size)
                    return false;
                std::memcpy(out, in.Data(), size);
                return true;
            case 5: return Lzw(in, out, size);
            case 8: case 32946:
            {
                Array<u8> inflated;
                if (!ZlibDecompress(in, inflated) || inflated.Size() < size)
                    return false;
                std::memcpy(out, inflated.Data(), size);
                return true;
            }
            case 32773: return PackBits(in, out, size);
            default: return false;
            }
        }

        // Undoes the predictor on rows of `rowSamples` samples of `bytes` bytes each, in the byte order of the file.
        bool Unpredict(const Ifd& d, u8* rows, u32 count, u32 width, u32 samplesPerRow, bool bigEndian)
        {
            u32 channels = samplesPerRow / width;
            if (d.predictor == 1)
                return true;
            if (d.predictor == 2 && d.bits == 8)
            {
                for (u32 y = 0; y < count; y++)
                {
                    u8* row = rows + usize(y) * samplesPerRow;
                    for (u32 i = channels; i < samplesPerRow; i++)
                        row[i] = u8(row[i] + row[i - channels]);
                }
                return true;
            }
            if (d.predictor == 2 && d.bits == 16)
            {
                for (u32 y = 0; y < count; y++)
                {
                    u8* row = rows + usize(y) * samplesPerRow * 2;
                    auto get = [&](u32 i) { return bigEndian ? u32(row[2 * i] << 8 | row[2 * i + 1]) : u32(row[2 * i] | row[2 * i + 1] << 8); };
                    auto set = [&](u32 i, u32 v) {
                        row[2 * i] = u8(bigEndian ? v >> 8 : v);
                        row[2 * i + 1] = u8(bigEndian ? v : v >> 8);
                    };
                    for (u32 i = channels; i < samplesPerRow; i++)
                        set(i, (get(i) + get(i - channels)) & 0xFFFF);
                }
                return true;
            }
            if (d.predictor == 3 && d.bits == 32)
            {
                // Bytes were differenced along the row and stored as planes, most significant byte first.
                usize bytes = usize(samplesPerRow) * 4;
                Array<u8> plain;
                plain.Resize(bytes);
                for (u32 y = 0; y < count; y++)
                {
                    u8* row = rows + usize(y) * bytes;
                    for (usize i = channels; i < bytes; i++)
                        row[i] = u8(row[i] + row[i - channels]);
                    for (u32 i = 0; i < samplesPerRow; i++)
                        for (u32 b = 0; b < 4; b++)
                            plain[usize(i) * 4 + (bigEndian ? b : 3 - b)] = row[usize(b) * samplesPerRow + i];
                    std::memcpy(row, plain.Data(), bytes);
                }
                return true;
            }
            return false;
        }

        bool DecodeTiff(Span<const u8> data, TextureData& out, String& error)
        {
            Ifd d;
            bool big = false;
            if (!ReadIfd(data, d, big, error))
                return false;
            u32 color = d.photometric == 2 ? 3 : 1;
            switch (d.compression)
            {
            case 1: case 5: case 8: case 32946: case 32773: break;
            case 2: case 3: case 4: return Fail(error, "an unsupported compression (CCITT fax)");
            case 6: case 7: return Fail(error, "an unsupported compression (JPEG)");
            default: return Fail(error, Format("an unsupported compression ({})", d.compression));
            }
            if (d.photometric > 3)
                return Fail(error, "an unsupported color model (only gray, RGB and palette)");
            if (d.samples < color)
                return Fail(error, "fewer samples than the color model needs");
            if (d.photometric == 3 && (d.bits > 8 || d.colorMap.Size() < 3u * (1u << d.bits)))
                return Fail(error, "a palette image without a full color map");
            bool alpha = d.samples > color;
            u32 planes = d.planar == 2 ? d.samples : 1;
            u32 perPixel = d.planar == 2 ? 1 : d.samples;
            u32 segmentWidth = d.tiled ? d.tileWidth : d.width;
            u32 segmentHeight = d.tiled ? d.tileHeight : (d.rowsPerStrip == 0 || d.rowsPerStrip > d.height ? d.height : d.rowsPerStrip);
            u32 across = (d.width + segmentWidth - 1) / segmentWidth, down = (d.height + segmentHeight - 1) / segmentHeight;
            usize segments = usize(across) * down * planes;
            if (d.offsets.Size() < segments)
                return Fail(error, "fewer strips or tiles than the image needs");
            if (d.compression != 1 && d.counts.Size() < segments)
                return Fail(error, "strip or tile sizes are missing");

            // The samples of the file, in its byte order: one buffer per plane, whole rows of the image.
            usize rowBytes = (usize(d.width) * perPixel * d.bits + 7) / 8;
            usize segmentRowBytes = (usize(segmentWidth) * perPixel * d.bits + 7) / 8;
            Array<Array<u8>> raw;
            for (u32 p = 0; p < planes; p++)
                raw.Emplace().Resize(rowBytes * d.height);
            Array<u8> segment;
            segment.Resize(segmentRowBytes * segmentHeight);
            for (usize s = 0; s < segments; s++)
            {
                u32 plane = u32(s / (usize(across) * down));
                usize index = s % (usize(across) * down);
                u32 x0 = u32(index % across) * segmentWidth, y0 = u32(index / across) * segmentHeight;
                u32 rows = d.tiled ? segmentHeight : (y0 + segmentHeight > d.height ? d.height - y0 : segmentHeight);
                u64 offset = d.offsets[s];
                u64 available = offset < data.Size() ? data.Size() - offset : 0;
                u64 count = d.counts.Size() > s ? d.counts[s] : available;
                if (offset > data.Size() || count > available)
                    return Fail(error, "a strip or tile is outside the file");
                usize size = segmentRowBytes * rows;
                if (!Decompress(d, Span<const u8>(data.Data() + offset, usize(count)), segment.Data(), size))
                    return Fail(error, "a strip or tile does not decompress to its size");
                if (d.bits >= 8 && !Unpredict(d, segment.Data(), rows, segmentWidth, segmentWidth * perPixel, big))
                    return Fail(error, "an unsupported predictor");
                if (d.bits < 8 && d.predictor != 1)
                    return Fail(error, "an unsupported predictor");
                usize skip = (usize(x0) * perPixel * d.bits) / 8;
                usize copy = Min(segmentRowBytes, rowBytes - skip);
                for (u32 y = 0; y < rows && y0 + y < d.height; y++)
                    std::memcpy(raw[plane].Data() + usize(y0 + y) * rowBytes + skip, segment.Data() + usize(y) * segmentRowBytes, copy);
            }

            PixelFormat format = d.bits == 32 ? PixelFormat::RGBA32Float : d.bits == 16 ? PixelFormat::RGBA16 : PixelFormat::RGBA8;
            if (!out.Allocate(format, d.width, d.height))
                return Fail(error, "the image is too large");
            auto sample = [&](u32 x, u32 y, u32 c) -> u32 {
                const u8* row = raw[d.planar == 2 ? c : 0].Data() + usize(y) * rowBytes;
                u32 i = d.planar == 2 ? x : x * d.samples + c;
                switch (d.bits)
                {
                case 1: return (row[i >> 3] >> (7 - (i & 7))) & 1;
                case 2: return (row[i >> 2] >> (6 - 2 * (i & 3))) & 3;
                case 4: return (row[i >> 1] >> (4 - 4 * (i & 1))) & 15;
                case 8: return row[i];
                case 16: return big ? u32(row[2 * i] << 8 | row[2 * i + 1]) : u32(row[2 * i] | row[2 * i + 1] << 8);
                default:
                {
                    const u8* q = row + usize(i) * 4;
                    return big ? (u32(q[0]) << 24) | (u32(q[1]) << 16) | (u32(q[2]) << 8) | q[3] : q[0] | (u32(q[1]) << 8) | (u32(q[2]) << 16) | (u32(q[3]) << 24);
                }
                }
            };
            u32 maximum = d.bits >= 16 ? 0xFFFFu : (1u << d.bits) - 1;
            for (u32 y = 0; y < d.height; y++)
                for (u32 x = 0; x < d.width; x++)
                {
                    u32 rgba[4];
                    if (d.photometric == 3)
                    {
                        u32 i = sample(x, y, 0), n = 1u << d.bits;
                        for (u32 c = 0; c < 3; c++)
                            rgba[c] = u32(d.colorMap[c * n + i] >> 8);
                        rgba[3] = alpha ? sample(x, y, 1) : 255u;
                    }
                    else
                    {
                        for (u32 c = 0; c < 3; c++)
                            rgba[c] = sample(x, y, color == 3 ? c : 0);
                        if (d.photometric == 0 && d.bits != 32)
                            rgba[0] = rgba[1] = rgba[2] = maximum - rgba[0];
                        rgba[3] = alpha ? sample(x, y, color) : (d.bits == 32 ? 0x3F800000u : maximum);
                        // Associated alpha back to straight alpha, as every other decoder gives it.
                        if (alpha && d.associated)
                        {
                            if (d.bits == 32)
                            {
                                float f[4];
                                std::memcpy(f, rgba, 16);
                                for (u32 c = 0; c < 3 && f[3] > 0; c++)
                                    f[c] /= f[3];
                                std::memcpy(rgba, f, 16);
                            }
                            else if (rgba[3] > 0)
                                for (u32 c = 0; c < 3; c++)
                                    rgba[c] = Min(maximum, (rgba[c] * maximum + rgba[3] / 2) / rgba[3]);
                        }
                    }
                    usize p = (usize(y) * d.width + x) * 4;
                    if (format == PixelFormat::RGBA32Float)
                        std::memcpy(out.data.Data() + p * 4, rgba, 16);
                    else if (format == PixelFormat::RGBA16)
                        for (u32 c = 0; c < 4; c++)
                        {
                            u16 v = u16(rgba[c]);
                            std::memcpy(out.data.Data() + (p + c) * 2, &v, 2);
                        }
                    else
                        for (u32 c = 0; c < 4; c++)
                            out.data[p + c] = d.photometric == 3 && c < 3 ? u8(rgba[c]) : u8(rgba[c] * 255u / maximum);
                }
            return true;
        }
    }

    const ImageDecoder& TiffImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"tif", "tiff"};
        static const ImageDecoder kDecoder{"TIFF", Span<const StringView>(kExtensions, 2), &MatchesTiff, &DecodeTiff};
        return kDecoder;
    }
}
