#include <Helix/Image/Codecs/Implementation/BuiltInDecoders.h>
#include <Helix/Image/Codecs/Implementation/ByteReader.h>
#include <Helix/Image/Compression/Deflate.h>

#include <cstring>

// OpenEXR, single part: scanline and tiled (the first level) images, compression NONE, RLE, ZIPS, ZIP and PIZ, HALF,
// FLOAT and UINT channels. R, G, B, A (also with a layer prefix) or Y become RGBA; the data window is the image.
namespace helix::image
{
    namespace
    {
        enum class Compression : u8
        {
            None = 0,
            Rle = 1,
            Zips = 2,
            Zip = 3,
            Piz = 4,
        };

        struct Channel
        {
            StringView name;
            u32 type = 1;
            u32 bytes = 2;
        };

        struct Header
        {
            Array<Channel> channels;
            Compression compression = Compression::None;
            i32 xMin = 0, yMin = 0, xMax = -1, yMax = -1;
            bool tiled = false;
            u32 tileWidth = 0, tileHeight = 0, levelMode = 0, rounding = 0;
            u32 width = 0, height = 0;
        };

        bool Fail(String& error, const char* what)
        {
            error = what;
            return false;
        }

        bool MatchesExr(Span<const u8> data) { return data.Size() >= 8 && data[0] == 0x76 && data[1] == 0x2F && data[2] == 0x31 && data[3] == 0x01; }

        bool CString(ByteReader& r, StringView& out)
        {
            usize start = r.at;
            while (r.at < r.size && r.data[r.at])
                r.at++;
            if (r.at >= r.size || r.at - start > 255)
                return r.ok = false;
            out = StringView(reinterpret_cast<const char*>(r.data) + start, r.at - start);
            r.at++;
            return true;
        }

        bool ReadHeader(ByteReader& r, Header& h, String& error)
        {
            r.Skip(4);
            u32 version = r.U32();
            if ((version & 0xFF) != 2)
                return Fail(error, "an unknown version");
            if (version & 0x800)
                return Fail(error, "deep data is not an image");
            if (version & 0x1000)
                return Fail(error, "multi part files are not supported");
            h.tiled = (version & 0x200) != 0;
            bool sawChannels = false, sawWindow = false;
            for (int attributes = 0; attributes < 1024; attributes++)
            {
                StringView name, type;
                if (!CString(r, name))
                    return Fail(error, "the header is cut");
                if (name.Empty())
                    break;
                if (!CString(r, type))
                    return Fail(error, "the header is cut");
                u32 size = r.U32();
                if (!r.ok || !r.Has(size))
                    return Fail(error, "an attribute runs past the end of the file");
                ByteReader v(Span<const u8>(r.data + r.at, size));
                r.at += size;
                if (name == StringView("channels") && type == StringView("chlist"))
                {
                    while (v.ok && v.at < v.size && v.data[v.at])
                    {
                        Channel c;
                        CString(v, c.name);
                        c.type = v.U32();
                        v.Skip(4);
                        u32 xs = v.U32(), ys = v.U32();
                        if (!v.ok || c.type > 2)
                            return Fail(error, "a damaged channel list");
                        if (xs != 1 || ys != 1)
                            return Fail(error, "subsampled channels are not supported");
                        c.bytes = c.type == 1 ? 2 : 4;
                        h.channels.Push(c);
                    }
                    sawChannels = !h.channels.Empty() && h.channels.Size() <= 64;
                }
                else if (name == StringView("compression"))
                {
                    u8 c = v.U8();
                    if (c > 4)
                        return Fail(error, "a compression that is not NONE, RLE, ZIPS, ZIP or PIZ");
                    h.compression = Compression(c);
                }
                else if (name == StringView("dataWindow"))
                {
                    h.xMin = i32(v.U32());
                    h.yMin = i32(v.U32());
                    h.xMax = i32(v.U32());
                    h.yMax = i32(v.U32());
                    sawWindow = v.ok;
                }
                else if (name == StringView("tiles"))
                {
                    h.tileWidth = v.U32();
                    h.tileHeight = v.U32();
                    u8 mode = v.U8();
                    h.levelMode = mode & 0xF;
                    h.rounding = mode >> 4;
                }
            }
            if (!r.ok || !sawChannels || !sawWindow)
                return Fail(error, "the header lacks channels or a data window");
            i64 w = i64(h.xMax) - h.xMin + 1, hh = i64(h.yMax) - h.yMin + 1;
            if (w <= 0 || hh <= 0 || w > (1 << 24) || hh > (1 << 24) || u64(w) * u64(hh) * 16 > kMaxTextureBytes)
                return Fail(error, "no valid data window");
            h.width = u32(w);
            h.height = u32(hh);
            if (h.tiled && (!h.tileWidth || !h.tileHeight || h.tileWidth > (1u << 16) || h.tileHeight > (1u << 16) || h.levelMode > 2))
                return Fail(error, "no valid tile description");
            return true;
        }

        u32 LinesPerChunk(Compression c) { return c == Compression::Zip ? 16 : c == Compression::Piz ? 32 : 1; }

        // Bytes of RLE and ZIP data: deltas, then the two halves of the bytes interleaved.
        void Unpredict(Array<u8>& bytes, u8* out)
        {
            usize n = bytes.Size();
            for (usize i = 1; i < n; i++)
                bytes[i] = u8(bytes[i - 1] + bytes[i] - 128);
            usize half = (n + 1) / 2;
            for (usize i = 0; i < n; i++)
                out[i] = i & 1 ? bytes[half + i / 2] : bytes[i / 2];
        }

        bool Rle(Span<const u8> in, Array<u8>& out, usize size)
        {
            usize at = 0;
            while (at < in.Size() && out.Size() < size)
            {
                i32 count = i8(in[at++]);
                if (count < 0)
                {
                    usize n = usize(-count);
                    if (at + n > in.Size() || out.Size() + n > size)
                        return false;
                    out.Append(in.Data() + at, n);
                    at += n;
                }
                else
                {
                    usize n = usize(count) + 1;
                    if (at >= in.Size() || out.Size() + n > size)
                        return false;
                    for (usize i = 0; i < n; i++)
                        out.Push(in[at]);
                    at++;
                }
            }
            return out.Size() == size;
        }

        // PIZ: a bitmap of used 16 bit values, Huffman coded values, then a wavelet per channel.
        namespace piz
        {
            constexpr u32 kEncodeSize = (1u << 16) + 1;
            constexpr u32 kDecodeBits = 14;
            constexpr u32 kDecodeSize = 1u << kDecodeBits;
            constexpr u32 kShortZeroRun = 59, kLongZeroRun = 63, kShortestLongRun = 2 + kLongZeroRun - kShortZeroRun;

            struct Entry
            {
                u32 length = 0;
                u32 literal = 0;
                // Codes longer than the table: their symbols, searched by code.
                Array<u32> longs;
            };

            struct Bits
            {
                const u8* at;
                const u8* end;
                u64 c = 0;
                i32 lc = 0;

                bool Get(u32 count, u64& value)
                {
                    while (lc < i32(count))
                    {
                        if (at >= end)
                            return false;
                        c = (c << 8) | *at++;
                        lc += 8;
                    }
                    lc -= i32(count);
                    value = (c >> lc) & ((u64(1) << count) - 1);
                    return true;
                }
            };

            bool UnpackTable(Bits& b, u32 im, u32 iM, Array<u64>& code)
            {
                for (; im <= iM; im++)
                {
                    u64 l;
                    if (!b.Get(6, l))
                        return false;
                    code[im] = l;
                    if (l == kLongZeroRun || l >= kShortZeroRun)
                    {
                        u64 run;
                        if (l == kLongZeroRun)
                        {
                            if (!b.Get(8, run))
                                return false;
                            run += kShortestLongRun;
                        }
                        else
                            run = l - kShortZeroRun + 2;
                        if (im + run > iM + 1)
                            return false;
                        while (run--)
                            code[im++] = 0;
                        im--;
                    }
                }
                // Canonical codes: each entry becomes length | code << 6.
                u64 n[59] = {};
                for (u64 l : code)
                    n[l]++;
                u64 c = 0;
                for (int i = 58; i > 0; i--)
                {
                    u64 next = (c + n[i]) >> 1;
                    n[i] = c;
                    c = next;
                }
                for (u64& l : code)
                    if (l > 0)
                        l = l | (n[l]++ << 6);
                return true;
            }

            bool BuildDecoder(const Array<u64>& code, u32 im, u32 iM, Array<Entry>& table)
            {
                for (; im <= iM; im++)
                {
                    u64 c = code[im] >> 6;
                    u32 l = u32(code[im] & 63);
                    if (c >> l)
                        return false;
                    if (l > kDecodeBits)
                    {
                        Entry& e = table[usize(c >> (l - kDecodeBits))];
                        if (e.length)
                            return false;
                        e.literal++;
                        e.longs.Push(im);
                    }
                    else if (l)
                    {
                        usize first = usize(c << (kDecodeBits - l));
                        for (usize i = 0; i < (usize(1) << (kDecodeBits - l)); i++)
                        {
                            Entry& e = table[first + i];
                            if (e.length || !e.longs.Empty())
                                return false;
                            e.length = l;
                            e.literal = im;
                        }
                    }
                }
                return true;
            }

            bool Emit(u32 symbol, u32 runCode, Bits& b, u16* out, usize& n, usize size)
            {
                if (symbol != runCode)
                {
                    if (n >= size)
                        return false;
                    out[n++] = u16(symbol);
                    return true;
                }
                u64 count;
                if (!b.Get(8, count) || n == 0 || n + count > size)
                    return false;
                u16 s = out[n - 1];
                while (count--)
                    out[n++] = s;
                return true;
            }

            bool Huffman(Span<const u8> in, u16* out, usize size)
            {
                if (in.Empty())
                    return size == 0;
                ByteReader r(in);
                u32 im = r.U32(), iM = r.U32();
                r.Skip(4);
                u32 nBits = r.U32();
                r.Skip(4);
                if (!r.ok || im >= kEncodeSize || iM >= kEncodeSize || im > iM || u64(nBits) > u64(r.size - r.at) * 8)
                    return false;
                Array<u64> code;
                code.Resize(kEncodeSize);
                Bits table{r.data + r.at, r.data + r.size};
                if (!UnpackTable(table, im, iM, code))
                    return false;
                Array<Entry> decode;
                decode.Resize(kDecodeSize);
                if (!BuildDecoder(code, im, iM, decode))
                    return false;
                const u8* start = table.at;
                if (u64(nBits) > u64(r.data + r.size - start) * 8)
                    return false;
                Bits b{start, start + (nBits + 7) / 8};
                usize n = 0;
                while (b.at < b.end)
                {
                    b.c = (b.c << 8) | *b.at++;
                    b.lc += 8;
                    while (b.lc >= i32(kDecodeBits))
                    {
                        const Entry& e = decode[usize((b.c >> (b.lc - kDecodeBits)) & (kDecodeSize - 1))];
                        if (e.length)
                        {
                            b.lc -= i32(e.length);
                            if (!Emit(e.literal, iM, b, out, n, size))
                                return false;
                            continue;
                        }
                        bool found = false;
                        for (u32 symbol : e.longs)
                        {
                            u32 l = u32(code[symbol] & 63);
                            while (b.lc < i32(l) && b.at < b.end)
                            {
                                b.c = (b.c << 8) | *b.at++;
                                b.lc += 8;
                            }
                            if (b.lc >= i32(l) && (code[symbol] >> 6) == ((b.c >> (b.lc - l)) & ((u64(1) << l) - 1)))
                            {
                                b.lc -= i32(l);
                                if (!Emit(symbol, iM, b, out, n, size))
                                    return false;
                                found = true;
                                break;
                            }
                        }
                        if (!found)
                            return false;
                    }
                }
                i32 unused = i32((8 - nBits) & 7);
                b.c >>= unused;
                b.lc -= unused;
                while (b.lc > 0)
                {
                    const Entry& e = decode[usize((b.c << (kDecodeBits - b.lc)) & (kDecodeSize - 1))];
                    if (!e.length || i32(e.length) > b.lc)
                        return false;
                    b.lc -= i32(e.length);
                    if (!Emit(e.literal, iM, b, out, n, size))
                        return false;
                }
                return n == size;
            }

            void Decode14(u16 l, u16 h, u16& a, u16& b)
            {
                i32 ls = i16(l), hs = i16(h);
                i32 ai = ls + (hs & 1) + (hs >> 1);
                a = u16(i16(ai));
                b = u16(i16(ai - hs));
            }

            void Decode16(u16 l, u16 h, u16& a, u16& b)
            {
                i32 m = l, d = h;
                i32 bb = (m - (d >> 1)) & 0xFFFF;
                i32 aa = (d + bb - 0x8000) & 0xFFFF;
                b = u16(bb);
                a = u16(aa);
            }

            // The inverse of the two dimensional Haar like wavelet of OpenEXR, on values `ox` apart in a row and rows
            // `oy` apart.
            void Wavelet(u16* in, i32 nx, i32 ox, i32 ny, i32 oy, u16 maximum)
            {
                bool w14 = maximum < (1 << 14);
                auto pair = [&](u16 l, u16 h, u16& a, u16& b) { w14 ? Decode14(l, h, a, b) : Decode16(l, h, a, b); };
                i32 n = nx > ny ? ny : nx;
                i32 p = 1;
                while (p <= n)
                    p <<= 1;
                p >>= 1;
                i32 p2 = p;
                p >>= 1;
                while (p >= 1)
                {
                    i32 oy1 = oy * p, oy2 = oy * p2, ox1 = ox * p, ox2 = ox * p2;
                    i32 y = 0;
                    for (; y <= oy * (ny - p2); y += oy2)
                    {
                        i32 x = y;
                        for (; x <= y + ox * (nx - p2); x += ox2)
                        {
                            u16 i00, i01, i10, i11;
                            pair(in[x], in[x + oy1], i00, i10);
                            pair(in[x + ox1], in[x + oy1 + ox1], i01, i11);
                            pair(i00, i01, in[x], in[x + ox1]);
                            pair(i10, i11, in[x + oy1], in[x + oy1 + ox1]);
                        }
                        if (nx & p)
                        {
                            u16 i00;
                            pair(in[x], in[x + oy1], i00, in[x + oy1]);
                            in[x] = i00;
                        }
                    }
                    if (ny & p)
                    {
                        for (i32 x = y; x <= y + ox * (nx - p2); x += ox2)
                        {
                            u16 i00;
                            pair(in[x], in[x + ox1], i00, in[x + ox1]);
                            in[x] = i00;
                        }
                    }
                    p2 = p;
                    p >>= 1;
                }
            }

            bool Uncompress(Span<const u8> in, const Header& h, u32 width, u32 lines, u8* out, usize size)
            {
                ByteReader r(in);
                u32 minNonZero = r.U16(), maxNonZero = r.U16();
                if (!r.ok || maxNonZero >= 8192)
                    return false;
                u8 bitmap[8192] = {};
                if (minNonZero <= maxNonZero)
                {
                    Span<const u8> used = r.Bytes(maxNonZero - minNonZero + 1);
                    if (!r.ok)
                        return false;
                    std::memcpy(bitmap + minNonZero, used.Data(), used.Size());
                }
                Array<u16> lut;
                lut.Resize(65536);
                u32 values = 0;
                for (u32 k = 0; k < 65536; k++)
                    if (k == 0 || (bitmap[k >> 3] & (1 << (k & 7))))
                        lut[values++] = u16(k);
                u16 maximum = u16(values - 1);
                u32 length = r.U32();
                if (!r.ok || !r.Has(length))
                    return false;
                usize count = size / 2;
                Array<u16> samples;
                samples.Resize(count);
                if (!Huffman(Span<const u8>(r.data + r.at, length), samples.Data(), count))
                    return false;
                usize at = 0;
                for (const Channel& c : h.channels)
                {
                    u32 words = c.bytes / 2;
                    for (u32 j = 0; j < words; j++)
                        Wavelet(samples.Data() + at + j, i32(width), i32(words), i32(lines), i32(width * words), maximum);
                    at += usize(width) * lines * words;
                }
                for (u16& s : samples)
                    s = s <= maximum ? lut[s] : 0;
                // From one block per channel to rows of channels.
                u8* dst = out;
                for (u32 y = 0; y < lines; y++)
                {
                    usize start = 0;
                    for (const Channel& c : h.channels)
                    {
                        usize n = usize(width) * (c.bytes / 2);
                        std::memcpy(dst, samples.Data() + start + usize(y) * n, n * 2);
                        dst += n * 2;
                        start += n * lines;
                    }
                }
                return true;
            }
        }

        // One chunk to its raw rows: for each row, for each channel, `width` samples, little endian.
        bool Unpack(const Header& h, Span<const u8> in, u32 width, u32 lines, Array<u8>& out)
        {
            usize rowBytes = 0;
            for (const Channel& c : h.channels)
                rowBytes += usize(c.bytes) * width;
            usize size = rowBytes * lines;
            out.Resize(size);
            if (in.Size() == size || h.compression == Compression::None)
            {
                if (in.Size() < size)
                    return false;
                std::memcpy(out.Data(), in.Data(), size);
                return true;
            }
            Array<u8> bytes;
            switch (h.compression)
            {
            case Compression::Rle:
                if (!Rle(in, bytes, size))
                    return false;
                Unpredict(bytes, out.Data());
                return true;
            case Compression::Zips: case Compression::Zip:
                if (!ZlibDecompress(in, bytes) || bytes.Size() != size)
                    return false;
                Unpredict(bytes, out.Data());
                return true;
            case Compression::Piz: return piz::Uncompress(in, h, width, lines, out.Data(), size);
            default: return false;
            }
        }

        float HalfToFloat(u16 h)
        {
            u32 sign = u32(h >> 15) << 31, exponent = (h >> 10) & 31, mantissa = h & 1023, bits;
            if (exponent == 0)
            {
                if (mantissa == 0)
                    bits = sign;
                else
                {
                    exponent = 113;
                    while (!(mantissa & 1024))
                    {
                        mantissa <<= 1;
                        exponent--;
                    }
                    bits = sign | (exponent << 23) | ((mantissa & 1023) << 13);
                }
            }
            else if (exponent == 31)
                bits = sign | 0x7F800000u | (mantissa << 13);
            else
                bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }

        // Which channel feeds R, G, B and A (-1 for none): exact names first, then the names after the last dot.
        void MapChannels(const Header& h, i32 source[4])
        {
            static constexpr const char* kNames[4] = {"R", "G", "B", "A"};
            for (int pass = 0; pass < 2; pass++)
                for (int c = 0; c < 4; c++)
                {
                    if (pass == 1 && source[c] >= 0)
                        continue;
                    if (pass == 0)
                        source[c] = -1;
                    for (usize i = 0; i < h.channels.Size(); i++)
                    {
                        StringView n = h.channels[i].name;
                        bool exact = n == StringView(kNames[c]);
                        bool suffix = n.Size() >= 2 && n[n.Size() - 2] == '.' && n[n.Size() - 1] == kNames[c][0];
                        if ((pass == 0 && exact) || (pass == 1 && suffix))
                        {
                            source[c] = i32(i);
                            break;
                        }
                    }
                }
            if (source[0] < 0 && source[1] < 0 && source[2] < 0)
            {
                // Luminance, or else the first channel, as gray.
                i32 y = 0;
                for (usize i = 0; i < h.channels.Size(); i++)
                    if (h.channels[i].name == StringView("Y"))
                        y = i32(i);
                source[0] = source[1] = source[2] = y;
            }
        }

        bool DecodeExr(Span<const u8> data, TextureData& out, String& error)
        {
            ByteReader r(data);
            Header h;
            if (!ReadHeader(r, h, error))
                return false;
            i32 source[4];
            MapChannels(h, source);
            bool allHalf = true;
            for (int c = 0; c < 4; c++)
                if (source[c] >= 0 && h.channels[usize(source[c])].type != 1)
                    allHalf = false;
            PixelFormat format = allHalf ? PixelFormat::RGBA16Float : PixelFormat::RGBA32Float;
            if (!out.Allocate(format, h.width, h.height))
                return Fail(error, "the image is too large");
            out.colorSpace = ColorSpace::Linear;

            // Chunks: scanline blocks, or the tiles of the first level.
            u32 chunkWidth = h.tiled ? h.tileWidth : h.width, chunkHeight = h.tiled ? h.tileHeight : LinesPerChunk(h.compression);
            u32 across = h.tiled ? (h.width + chunkWidth - 1) / chunkWidth : 1, down = (h.height + chunkHeight - 1) / chunkHeight;
            u64 chunks = u64(across) * down;
            if (chunks > (1u << 24))
                return Fail(error, "too many chunks");
            Array<u64> offsets;
            for (u64 i = 0; i < chunks; i++)
                offsets.Push(r.U64());
            if (!r.ok)
                return Fail(error, "the offset table is cut");

            Array<usize> channelOffset;
            usize pixelBytes = 0;
            for (const Channel& c : h.channels)
            {
                channelOffset.Push(pixelBytes);
                pixelBytes += c.bytes;
            }
            Array<u8> raw;
            for (u64 chunk = 0; chunk < chunks; chunk++)
            {
                ByteReader c(data);
                if (!c.Seek(offsets[usize(chunk)]))
                    return Fail(error, "a chunk is outside the file");
                u32 x0 = 0, y0 = 0;
                if (h.tiled)
                {
                    u32 tx = c.U32(), ty = c.U32(), lx = c.U32(), ly = c.U32();
                    if (!c.ok || lx || ly || tx >= across || ty >= down)
                        return Fail(error, "a tile outside the first level");
                    x0 = tx * chunkWidth;
                    y0 = ty * chunkHeight;
                }
                else
                {
                    i64 y = i64(i32(c.U32())) - h.yMin;
                    if (!c.ok || y < 0 || y >= i64(h.height) || y % chunkHeight)
                        return Fail(error, "a scanline chunk outside the data window");
                    y0 = u32(y);
                }
                u32 size = c.U32();
                Span<const u8> packed = c.Bytes(size);
                if (!c.ok)
                    return Fail(error, "a chunk runs past the end of the file");
                u32 width = Min(chunkWidth, h.width - x0), lines = Min(chunkHeight, h.height - y0);
                if (!Unpack(h, packed, width, lines, raw))
                    return Fail(error, "a chunk does not decompress to its size");
                for (u32 y = 0; y < lines; y++)
                {
                    const u8* row = raw.Data() + usize(y) * width * pixelBytes;
                    for (u32 x = 0; x < width; x++)
                    {
                        usize p = (usize(y0 + y) * h.width + x0 + x) * 4;
                        for (int k = 0; k < 4; k++)
                        {
                            float f = k == 3 ? 1.0f : 0.0f;
                            u16 half = k == 3 ? 0x3C00 : 0;
                            if (source[k] >= 0)
                            {
                                const Channel& ch = h.channels[usize(source[k])];
                                const u8* s = row + channelOffset[usize(source[k])] * width + usize(x) * ch.bytes;
                                if (ch.type == 1)
                                {
                                    half = u16(s[0] | (s[1] << 8));
                                    f = HalfToFloat(half);
                                }
                                else
                                {
                                    u32 bits = s[0] | (u32(s[1]) << 8) | (u32(s[2]) << 16) | (u32(s[3]) << 24);
                                    if (ch.type == 2)
                                        std::memcpy(&f, &bits, 4);
                                    else
                                        f = float(bits);
                                }
                            }
                            if (format == PixelFormat::RGBA16Float)
                                std::memcpy(out.data.Data() + (p + k) * 2, &half, 2);
                            else
                                std::memcpy(out.data.Data() + (p + k) * 4, &f, 4);
                        }
                    }
                }
            }
            return true;
        }
    }

    const ImageDecoder& ExrImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"exr"};
        static const ImageDecoder kDecoder{"EXR", Span<const StringView>(kExtensions, 1), &MatchesExr, &DecodeExr};
        return kDecoder;
    }
}
