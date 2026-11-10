#include <Helix/Image/Compression/Deflate.h>
#include <Helix/Image/Compression/Implementation/DeflateTables.h>

#include <cstring>

namespace helix::image
{
    namespace
    {
        using namespace deflate;

        constexpr u32 kFastBits = 10;
        constexpr usize kOutputSlack = 258 + 8;

        // Canonical Huffman decoder: a direct table for codes up to kFastBits, a counting walk for longer ones.
        struct Huffman
        {
            u16 fast[1 << kFastBits];
            u16 counts[16];
            u16 symbols[288];

            bool Build(const u8* lengths, u32 n)
            {
                std::memset(counts, 0, sizeof(counts));
                for (u32 i = 0; i < n; i++)
                    counts[lengths[i]]++;
                counts[0] = 0;
                i32 left = 1;
                for (u32 len = 1; len < 16; len++)
                {
                    left = (left << 1) - counts[len];
                    if (left < 0)
                        return false;
                }
                u16 offsets[16];
                offsets[1] = 0;
                for (u32 len = 1; len < 15; len++)
                    offsets[len + 1] = u16(offsets[len] + counts[len]);
                for (u32 i = 0; i < n; i++)
                    if (lengths[i])
                        symbols[offsets[lengths[i]]++] = u16(i);
                std::memset(fast, 0, sizeof(fast));
                u32 code = 0, index = 0;
                for (u32 len = 1; len <= kFastBits; len++)
                {
                    for (u32 k = 0; k < counts[len]; k++, code++, index++)
                    {
                        u32 rev = ReverseBits(code, len);
                        for (u32 fill = rev; fill < (1u << kFastBits); fill += 1u << len)
                            fast[fill] = u16(symbols[index] | (len << 9));
                    }
                    code <<= 1;
                }
                return true;
            }
        };

        struct Reader
        {
            const u8* data;
            usize size;
            usize pos = 0;
            u64 bits = 0;
            u32 count = 0;

            // Past the end the stream reads as zero bits; Overrun reports whether any of them were consumed.
            void Refill()
            {
                while (count <= 56)
                {
                    u64 byte = pos < size ? data[pos] : 0;
                    pos++;
                    bits |= byte << count;
                    count += 8;
                }
            }

            u32 Get(u32 n)
            {
                if (count < n)
                    Refill();
                u32 v = u32(bits & ((1ull << n) - 1));
                bits >>= n;
                count -= n;
                return v;
            }

            void AlignToByte()
            {
                u32 drop = count & 7;
                bits >>= drop;
                count -= drop;
            }

            // Bytes consumed so far, counting buffered whole bytes as unread.
            usize Consumed() const { return pos - count / 8; }
            bool Overrun() const { return Consumed() > size; }

            i32 Decode(const Huffman& h)
            {
                if (count < 16)
                    Refill();
                u16 e = h.fast[bits & ((1u << kFastBits) - 1)];
                if (e)
                {
                    u32 len = e >> 9;
                    bits >>= len;
                    count -= len;
                    return e & 511;
                }
                i32 code = 0, first = 0, index = 0;
                for (u32 len = 1; len < 16; len++)
                {
                    code |= i32(Get(1));
                    i32 n = h.counts[len];
                    if (code - n < first)
                        return h.symbols[index + (code - first)];
                    index += n;
                    first = (first + n) << 1;
                    code <<= 1;
                }
                return -1;
            }
        };

        bool InflateBlocks(Reader& r, Array<u8>& out)
        {
            usize start = out.Size();
            // The symbol loop writes through `base` into space appended in large steps; `written` is the real size.
            usize written = start, space = start;
            u8* base = out.Data();
            auto grow = [&](usize needed) {
                out.AppendUninitialized(Max<usize>(needed, out.Size() / 2 + 65536));
                space = out.Size();
                base = out.Data();
            };
            Huffman* lit = New<Huffman>(HeapAllocator());
            Huffman* dist = New<Huffman>(HeapAllocator());
            bool ok = true;
            for (bool last = false; ok && !last;)
            {
                last = r.Get(1) != 0;
                u32 type = r.Get(2);
                if (type == 0)
                {
                    r.AlignToByte();
                    u32 len = r.Get(16), nlen = r.Get(16);
                    if ((len ^ 0xFFFF) != nlen)
                    {
                        ok = false;
                        break;
                    }
                    usize from = r.Consumed();
                    if (from + len > r.size)
                    {
                        ok = false;
                        break;
                    }
                    out.Append(r.data + from, len);
                    written = space = out.Size();
                    base = out.Data();
                    r.pos = from + len;
                    r.bits = 0;
                    r.count = 0;
                    continue;
                }
                u8 lengths[288 + 32];
                u32 nlit = 288, ndist = 30;
                if (type == 1)
                    FixedLengths(lengths, lengths + 288);
                else if (type == 2)
                {
                    nlit = r.Get(5) + 257;
                    ndist = r.Get(5) + 1;
                    u32 ncode = r.Get(4) + 4;
                    u8 codeLengths[19] = {};
                    for (u32 i = 0; i < ncode; i++)
                        codeLengths[kCodeLengthOrder[i]] = u8(r.Get(3));
                    Huffman* lengthCode = New<Huffman>(HeapAllocator());
                    ok = lengthCode->Build(codeLengths, 19) && nlit <= 286 && ndist <= 30;
                    u8 all[288 + 32] = {};
                    for (u32 i = 0; ok && i < nlit + ndist;)
                    {
                        i32 sym = r.Decode(*lengthCode);
                        if (sym < 0 || r.Overrun())
                            ok = false;
                        else if (sym < 16)
                            all[i++] = u8(sym);
                        else
                        {
                            u32 repeat = sym == 16 ? 3 + r.Get(2) : sym == 17 ? 3 + r.Get(3) : 11 + r.Get(7);
                            if ((sym == 16 && i == 0) || i + repeat > nlit + ndist)
                            {
                                ok = false;
                                break;
                            }
                            u8 value = sym == 16 ? all[i - 1] : 0;
                            for (u32 k = 0; k < repeat; k++)
                                all[i++] = value;
                        }
                    }
                    Delete(HeapAllocator(), lengthCode);
                    std::memcpy(lengths, all, nlit);
                    std::memset(lengths + nlit, 0, 288 - nlit);
                    std::memcpy(lengths + 288, all + nlit, ndist);
                    std::memset(lengths + 288 + ndist, 0, 32 - ndist);
                }
                else
                    ok = false;
                if (!ok || !lit->Build(lengths, 288) || !dist->Build(lengths + 288, 30))
                {
                    ok = false;
                    break;
                }
                for (;;)
                {
                    // One symbol needs at most 48 bits and 258 bytes plus copy slack, so both are secured once here.
                    if (written + kOutputSlack > space)
                        grow(kOutputSlack);
                    bool inside = r.pos + 8 <= r.size;
                    if (inside)
                    {
                        u64 chunk;
                        std::memcpy(&chunk, r.data + r.pos, 8);
                        r.bits |= chunk << r.count;
                        r.pos += (63 - r.count) >> 3;
                        r.count |= 56;
                    }
                    i32 sym = r.Decode(*lit);
                    if (sym < 0 || (!inside && r.Overrun()))
                    {
                        ok = false;
                        break;
                    }
                    if (sym < 256)
                    {
                        base[written++] = u8(sym);
                        // The refill left enough bits for two more short literals without touching the input.
                        for (int extra = 0; inside && extra < 2; extra++)
                        {
                            u16 e = lit->fast[r.bits & ((1u << kFastBits) - 1)];
                            if (!e || (e & 511) >= 256)
                                break;
                            r.bits >>= e >> 9;
                            r.count -= e >> 9;
                            base[written++] = u8(e);
                        }
                        continue;
                    }
                    if (sym == 256)
                        break;
                    sym -= 257;
                    if (sym >= 29)
                    {
                        ok = false;
                        break;
                    }
                    u32 length = kLengthBase[sym] + r.Get(kLengthExtra[sym]);
                    i32 d = r.Decode(*dist);
                    if (d < 0 || d >= 30)
                    {
                        ok = false;
                        break;
                    }
                    u32 distance = kDistBase[d] + r.Get(kDistExtra[d]);
                    if (distance > written - start)
                    {
                        ok = false;
                        break;
                    }
                    u8* dst = base + written;
                    const u8* src = dst - distance;
                    if (distance >= 8)
                    {
                        // Eight bytes per step; the last step may write up to seven bytes of slack past the match.
                        for (u32 i = 0; i < length; i += 8)
                            std::memcpy(dst + i, src + i, 8);
                    }
                    else if (distance == 1)
                        std::memset(dst, src[0], length);
                    else
                        for (u32 i = 0; i < length; i++)
                            dst[i] = src[i];
                    written += length;
                }
                out.Resize(written);
                space = written;
            }
            Delete(HeapAllocator(), lit);
            Delete(HeapAllocator(), dist);
            return ok && !r.Overrun();
        }
    }

    bool Inflate(Span<const u8> in, Array<u8>& out)
    {
        Reader r{in.Data(), in.Size()};
        return InflateBlocks(r, out);
    }

    bool ZlibDecompress(Span<const u8> in, Array<u8>& out)
    {
        if (in.Size() < 6)
            return false;
        u32 cmf = in[0], flg = in[1];
        if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20))
            return false;
        usize start = out.Size();
        Reader r{in.Data() + 2, in.Size() - 2};
        if (!InflateBlocks(r, out))
            return false;
        usize end = 2 + r.Consumed();
        if (end + 4 > in.Size())
            return false;
        u32 expected = (u32(in[end]) << 24) | (u32(in[end + 1]) << 16) | (u32(in[end + 2]) << 8) | in[end + 3];
        return Adler32(out.Data() + start, out.Size() - start) == expected;
    }

    u32 Adler32(const void* data, usize size, u32 adler)
    {
        const u8* p = static_cast<const u8*>(data);
        u32 a = adler & 0xFFFF, b = adler >> 16;
        while (size)
        {
            usize n = size < 5552 ? size : 5552;
            size -= n;
            for (usize i = 0; i < n; i++)
            {
                a += p[i];
                b += a;
            }
            p += n;
            a %= 65521;
            b %= 65521;
        }
        return (b << 16) | a;
    }
}
