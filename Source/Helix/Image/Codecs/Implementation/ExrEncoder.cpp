#include <Helix/Image/Codecs/Implementation/BuiltInEncoders.h>
#include <Helix/Image/Compression/Deflate.h>

#include <cstring>

// OpenEXR scanline images with the EncodeToEXR flags: half or float R, G, B, A (as many as the texture has), NONE, RLE,
// ZIP or PIZ.
namespace helix::image
{
    namespace
    {
        void Put16(Array<u8>& out, u32 v)
        {
            out.Push(u8(v));
            out.Push(u8(v >> 8));
        }
        void Put32(Array<u8>& out, u32 v)
        {
            Put16(out, v & 0xFFFF);
            Put16(out, v >> 16);
        }
        void PutFloat(Array<u8>& out, float f)
        {
            u32 v;
            std::memcpy(&v, &f, 4);
            Put32(out, v);
        }
        void PutText(Array<u8>& out, const char* text)
        {
            for (; *text; text++)
                out.Push(u8(*text));
            out.Push(0);
        }

        // RLE and ZIP data: the two halves of the bytes split, then deltas.
        Array<u8> Predict(const u8* raw, usize n)
        {
            usize half = (n + 1) / 2;
            Array<u8> split;
            split.Resize(n);
            for (usize i = 0; i < n; i++)
                split[i & 1 ? half + i / 2 : i / 2] = raw[i];
            Array<u8> out;
            out.Resize(n);
            for (usize i = 0; i < n; i++)
                out[i] = i == 0 ? split[0] : u8(split[i] - split[i - 1] + 128);
            return out;
        }

        void Rle(const Array<u8>& in, Array<u8>& out)
        {
            for (usize i = 0; i < in.Size();)
            {
                usize run = 1;
                while (i + run < in.Size() && run < 128 && in[i + run] == in[i])
                    run++;
                if (run >= 3)
                {
                    out.Push(u8(run - 1));
                    out.Push(in[i]);
                    i += run;
                    continue;
                }
                usize literal = 0;
                while (i + literal < in.Size() && literal < 127 &&
                       !(i + literal + 2 < in.Size() && in[i + literal] == in[i + literal + 1] && in[i + literal] == in[i + literal + 2]))
                    literal++;
                out.Push(u8(-i32(literal)));
                out.Append(in.Data() + i, literal);
                i += literal;
            }
        }

        namespace piz
        {
            constexpr u32 kEncodeSize = (1u << 16) + 1;
            constexpr u32 kShortZeroRun = 59, kLongZeroRun = 63, kShortestLongRun = 2 + kLongZeroRun - kShortZeroRun, kLongestLongRun = 255 + kShortestLongRun;

            struct BitWriter
            {
                Array<u8>& out;
                u64 c = 0;
                u32 lc = 0;
                u64 bits = 0;

                void Put(u32 count, u64 value)
                {
                    c = (c << count) | value;
                    lc += count;
                    bits += count;
                    while (lc >= 8)
                    {
                        lc -= 8;
                        out.Push(u8(c >> lc));
                    }
                }
                void Flush()
                {
                    if (lc)
                        out.Push(u8(c << (8 - lc)));
                    lc = 0;
                }
            };

            u32 Length(u64 code) { return u32(code & 63); }
            u64 Code(u64 code) { return code >> 6; }

            // Huffman lengths for the symbols that occur, then canonical codes as length | code << 6.
            void BuildTable(const Array<u64>& frequency, u32 im, u32 iM, Array<u64>& code)
            {
                struct Node
                {
                    u64 weight;
                    i32 parent;
                };
                Array<Node> nodes;
                Array<u32> symbolNode;
                symbolNode.Resize(kEncodeSize);
                Array<u32> heap;
                for (u32 s = im; s <= iM; s++)
                    if (frequency[s])
                    {
                        symbolNode[s] = u32(nodes.Size());
                        heap.Push(u32(nodes.Size()));
                        nodes.Push({frequency[s], -1});
                    }
                auto less = [&](u32 a, u32 b) { return nodes[a].weight < nodes[b].weight || (nodes[a].weight == nodes[b].weight && a < b); };
                auto down = [&](usize i) {
                    while (true)
                    {
                        usize l = 2 * i + 1, r = l + 1, m = i;
                        if (l < heap.Size() && less(heap[l], heap[m]))
                            m = l;
                        if (r < heap.Size() && less(heap[r], heap[m]))
                            m = r;
                        if (m == i)
                            return;
                        u32 t = heap[i];
                        heap[i] = heap[m];
                        heap[m] = t;
                        i = m;
                    }
                };
                auto pop = [&]() {
                    u32 top = heap[0];
                    heap[0] = heap.Back();
                    heap.Pop();
                    if (!heap.Empty())
                        down(0);
                    return top;
                };
                auto push = [&](u32 node) {
                    heap.Push(node);
                    for (usize i = heap.Size() - 1; i > 0;)
                    {
                        usize p = (i - 1) / 2;
                        if (!less(heap[i], heap[p]))
                            break;
                        u32 t = heap[i];
                        heap[i] = heap[p];
                        heap[p] = t;
                        i = p;
                    }
                };
                for (usize i = heap.Size() / 2; i-- > 0;)
                    down(i);
                while (heap.Size() > 1)
                {
                    u32 a = pop(), b = pop();
                    u32 parent = u32(nodes.Size());
                    nodes.Push({nodes[a].weight + nodes[b].weight, -1});
                    nodes[a].parent = i32(parent);
                    nodes[b].parent = i32(parent);
                    push(parent);
                }
                code.Clear();
                code.Resize(kEncodeSize);
                for (u32 s = im; s <= iM; s++)
                    if (frequency[s])
                    {
                        u32 depth = 0;
                        for (i32 n = i32(symbolNode[s]); nodes[usize(n)].parent >= 0; n = nodes[usize(n)].parent)
                            depth++;
                        code[s] = depth ? depth : 1;
                    }
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
            }

            void PackTable(const Array<u64>& code, u32 im, u32 iM, BitWriter& w)
            {
                for (; im <= iM; im++)
                {
                    u32 l = Length(code[im]);
                    if (l == 0)
                    {
                        u32 run = 1;
                        while (im < iM && run < kLongestLongRun && Length(code[im + 1]) == 0)
                        {
                            im++;
                            run++;
                        }
                        if (run >= 2)
                        {
                            if (run >= kShortestLongRun)
                            {
                                w.Put(6, kLongZeroRun);
                                w.Put(8, run - kShortestLongRun);
                            }
                            else
                                w.Put(6, kShortZeroRun + run - 2);
                            continue;
                        }
                    }
                    w.Put(6, l);
                }
                w.Flush();
            }

            void Send(u64 symbol, u32 run, u64 runCode, BitWriter& w)
            {
                if (Length(symbol) + Length(runCode) + 8 < Length(symbol) * run)
                {
                    w.Put(Length(symbol), Code(symbol));
                    w.Put(Length(runCode), Code(runCode));
                    w.Put(8, run);
                }
                else
                    for (u32 i = 0; i <= run; i++)
                        w.Put(Length(symbol), Code(symbol));
            }

            void Huffman(const u16* raw, usize n, Array<u8>& out)
            {
                if (n == 0)
                    return;
                Array<u64> frequency;
                frequency.Resize(kEncodeSize);
                for (usize i = 0; i < n; i++)
                    frequency[raw[i]]++;
                u32 im = 0, iM = 0xFFFF;
                while (!frequency[im])
                    im++;
                while (!frequency[iM])
                    iM--;
                // The symbol after the largest stands for "repeat the previous value".
                iM++;
                frequency[iM] = 1;
                Array<u64> code;
                BuildTable(frequency, im, iM, code);
                usize header = out.Size();
                for (int i = 0; i < 5; i++)
                    Put32(out, 0);
                usize tableStart = out.Size();
                BitWriter table{out};
                PackTable(code, im, iM, table);
                usize tableLength = out.Size() - tableStart;
                BitWriter data{out};
                u32 s = raw[0], run = 0;
                for (usize i = 1; i < n; i++)
                {
                    if (s == raw[i] && run < 255)
                        run++;
                    else
                    {
                        Send(code[s], run, code[iM], data);
                        run = 0;
                    }
                    s = raw[i];
                }
                Send(code[s], run, code[iM], data);
                u64 bits = data.bits;
                data.Flush();
                u32 values[5] = {im, iM, u32(tableLength), u32(bits), 0};
                std::memcpy(out.Data() + header, values, 20);
            }

            void Encode14(u16 a, u16 b, u16& l, u16& h)
            {
                i16 as = i16(a), bs = i16(b);
                l = u16(i16((as + bs) >> 1));
                h = u16(i16(as - bs));
            }

            void Encode16(u16 a, u16 b, u16& l, u16& h)
            {
                i32 ao = (i32(a) + 0x8000) & 0xFFFF;
                i32 m = (ao + i32(b)) >> 1, d = ao - i32(b);
                if (d < 0)
                    m = (m + 0x8000) & 0xFFFF;
                d &= 0xFFFF;
                l = u16(m);
                h = u16(d);
            }

            void Wavelet(u16* in, i32 nx, i32 ox, i32 ny, i32 oy, u16 maximum)
            {
                bool w14 = maximum < (1 << 14);
                auto pair = [&](u16 a, u16 b, u16& l, u16& h) { w14 ? Encode14(a, b, l, h) : Encode16(a, b, l, h); };
                i32 n = nx > ny ? ny : nx;
                i32 p = 1, p2 = 2;
                while (p2 <= n)
                {
                    i32 oy1 = oy * p, oy2 = oy * p2, ox1 = ox * p, ox2 = ox * p2;
                    i32 y = 0;
                    for (; y <= oy * (ny - p2); y += oy2)
                    {
                        i32 x = y;
                        for (; x <= y + ox * (nx - p2); x += ox2)
                        {
                            u16 i00, i01, i10, i11;
                            pair(in[x], in[x + ox1], i00, i01);
                            pair(in[x + oy1], in[x + oy1 + ox1], i10, i11);
                            pair(i00, i10, in[x], in[x + oy1]);
                            pair(i01, i11, in[x + ox1], in[x + oy1 + ox1]);
                        }
                        if (nx & p)
                        {
                            u16 i00;
                            pair(in[x], in[x + oy1], i00, in[x + oy1]);
                            in[x] = i00;
                        }
                    }
                    if (ny & p)
                        for (i32 x = y; x <= y + ox * (nx - p2); x += ox2)
                        {
                            u16 i00;
                            pair(in[x], in[x + ox1], i00, in[x + ox1]);
                            in[x] = i00;
                        }
                    p = p2;
                    p2 <<= 1;
                }
            }

            // Rows of channels (`words` 16 bit words per sample) to one block per channel, the used value bitmap, the
            // wavelet and the Huffman code.
            void Compress(const u8* raw, u32 width, u32 lines, u32 channels, u32 words, Array<u8>& out)
            {
                usize perChannel = usize(width) * lines * words, count = perChannel * channels;
                Array<u16> samples;
                samples.Resize(count);
                usize rowWords = usize(width) * words;
                for (u32 y = 0; y < lines; y++)
                    for (u32 c = 0; c < channels; c++)
                        std::memcpy(samples.Data() + c * perChannel + usize(y) * rowWords, raw + (usize(y) * channels + c) * rowWords * 2, rowWords * 2);
                u8 bitmap[8192] = {};
                for (u16 s : samples)
                    bitmap[s >> 3] |= u8(1 << (s & 7));
                bitmap[0] &= 0xFE;
                u32 minNonZero = 8191, maxNonZero = 0;
                for (u32 i = 0; i < 8192; i++)
                    if (bitmap[i])
                    {
                        minNonZero = i < minNonZero ? i : minNonZero;
                        maxNonZero = i > maxNonZero ? i : maxNonZero;
                    }
                Array<u16> lut;
                lut.Resize(65536);
                u32 k = 0;
                for (u32 i = 0; i < 65536; i++)
                    lut[i] = (i == 0 || (bitmap[i >> 3] & (1 << (i & 7)))) ? u16(k++) : u16(0);
                u16 maximum = u16(k - 1);
                for (u16& s : samples)
                    s = lut[s];
                Put16(out, minNonZero);
                Put16(out, maxNonZero);
                if (minNonZero <= maxNonZero)
                    out.Append(bitmap + minNonZero, maxNonZero - minNonZero + 1);
                for (u32 c = 0; c < channels; c++)
                    for (u32 j = 0; j < words; j++)
                        Wavelet(samples.Data() + c * perChannel + j, i32(width), i32(words), i32(lines), i32(width * words), maximum);
                usize lengthAt = out.Size();
                Put32(out, 0);
                Huffman(samples.Data(), count, out);
                u32 length = u32(out.Size() - lengthAt - 4);
                std::memcpy(out.Data() + lengthAt, &length, 4);
            }
        }

        bool EncodeExr(const TextureData& texture, const EncodeOptions& options, Array<u8>& out, String& error)
        {
            u32 channels = FormatInfo(texture.format).channels;
            channels = channels ? channels : 1;
            bool asFloat = (options.exrFlags & ExrOutputAsFloat) != 0;
            u32 compression = options.exrFlags & ExrCompressPiz ? 4 : options.exrFlags & ExrCompressZip ? 3 : options.exrFlags & ExrCompressRle ? 1 : 0;
            Array<u8> rgba;
            if (!FirstSurface(texture, PixelFormat::RGBA32Float, rgba, error))
                return false;
            const float* pixels = reinterpret_cast<const float*>(rgba.Data());
            u32 width = texture.width, height = texture.height;
            // Channels sorted by name, as the format wants: A, B, G, R.
            static constexpr const char* kNames[4] = {"R", "G", "B", "A"};
            u32 order[4], count = 0;
            for (u32 c = 4; c-- > 0;)
                if (c < channels)
                    order[count++] = c;

            Put32(out, 20000630);
            Put32(out, 2);
            auto attribute = [&](const char* name, const char* type, u32 size) {
                PutText(out, name);
                PutText(out, type);
                Put32(out, size);
            };
            attribute("channels", "chlist", count * 18 + 1);
            for (u32 i = 0; i < count; i++)
            {
                PutText(out, kNames[order[i]]);
                Put32(out, asFloat ? 2 : 1);
                Put32(out, 0);
                Put32(out, 1);
                Put32(out, 1);
            }
            out.Push(0);
            attribute("compression", "compression", 1);
            out.Push(u8(compression));
            for (const char* window : {"dataWindow", "displayWindow"})
            {
                attribute(window, "box2i", 16);
                Put32(out, 0);
                Put32(out, 0);
                Put32(out, width - 1);
                Put32(out, height - 1);
            }
            attribute("lineOrder", "lineOrder", 1);
            out.Push(0);
            attribute("pixelAspectRatio", "float", 4);
            PutFloat(out, 1.0f);
            attribute("screenWindowCenter", "v2f", 8);
            PutFloat(out, 0.0f);
            PutFloat(out, 0.0f);
            attribute("screenWindowWidth", "float", 4);
            PutFloat(out, 1.0f);
            out.Push(0);

            u32 lines = compression == 3 ? 16 : compression == 4 ? 32 : 1;
            u32 chunks = (height + lines - 1) / lines;
            usize table = out.Size();
            for (u32 i = 0; i < chunks; i++)
            {
                Put32(out, 0);
                Put32(out, 0);
            }
            u32 bytes = asFloat ? 4 : 2;
            Array<u8> raw, packed;
            for (u32 chunk = 0; chunk < chunks; chunk++)
            {
                u32 y0 = chunk * lines, n = height - y0 < lines ? height - y0 : lines;
                raw.Clear();
                for (u32 y = y0; y < y0 + n; y++)
                    for (u32 i = 0; i < count; i++)
                        for (u32 x = 0; x < width; x++)
                        {
                            float f = pixels[(usize(y) * width + x) * 4 + order[i]];
                            if (asFloat)
                                PutFloat(raw, f);
                            else
                                Put16(raw, FloatToHalf(f));
                        }
                packed.Clear();
                if (compression == 1)
                    Rle(Predict(raw.Data(), raw.Size()), packed);
                else if (compression == 3)
                {
                    Array<u8> predicted = Predict(raw.Data(), raw.Size());
                    ZlibCompress(Span<const u8>(predicted.Data(), predicted.Size()), packed);
                }
                else if (compression == 4)
                    piz::Compress(raw.Data(), width, n, count, bytes / 2, packed);
                // Data that does not get smaller is stored as it is.
                const Array<u8>& stored = compression == 0 || packed.Size() >= raw.Size() ? raw : packed;
                u64 offset = out.Size();
                std::memcpy(out.Data() + table + usize(chunk) * 8, &offset, 8);
                Put32(out, y0);
                Put32(out, u32(stored.Size()));
                out.Append(stored.Data(), stored.Size());
            }
            return true;
        }
    }

    const ImageEncoder& ExrImageEncoder()
    {
        static constexpr StringView kExtensions[] = {"exr"};
        static const ImageEncoder kEncoder{"EXR", Span<const StringView>(kExtensions, 1), &EncodeExr};
        return kEncoder;
    }
}
