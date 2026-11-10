#include <Helix/Image/Compression/Deflate.h>
#include <Helix/Image/Compression/Implementation/DeflateTables.h>

#include <Helix/Jobs/Jobs.h>

#include <bit>
#include <cstring>

namespace helix::image
{
    namespace
    {
        using namespace deflate;

        constexpr u32 kWindow = 32768;
        constexpr u32 kHashBits = 15;
        constexpr u32 kMinMatch = 3, kMaxMatch = 258;
        constexpr u32 kBlockTokens = 1 << 16;

        struct BitWriter
        {
            Array<u8>& out;
            u64 bits = 0;
            u32 count = 0;

            void Put(u32 value, u32 n)
            {
                bits |= u64(value) << count;
                count += n;
                while (count >= 8)
                {
                    out.Push(u8(bits));
                    bits >>= 8;
                    count -= 8;
                }
            }

            void Align()
            {
                if (count)
                    Put(0, 8 - count);
            }
        };

        struct Token
        {
            u16 length;
            u16 distance;
        };

        // Length limited code lengths: Huffman tree depths, then the JPEG Annex K.3 rebalancing when a code is too long.
        void BuildLengths(const u32* freq, u32 n, u32 limit, u8* lengths)
        {
            std::memset(lengths, 0, n);
            struct Node
            {
                u32 freq;
                i32 parent;
            };
            Node nodes[2 * 288];
            i32 leaf[288];
            u32 count = 0;
            for (u32 i = 0; i < n; i++)
                if (freq[i])
                {
                    leaf[count] = i32(i);
                    nodes[count++] = {freq[i], -1};
                }
            if (count == 0)
                return;
            if (count == 1)
            {
                lengths[leaf[0]] = 1;
                return;
            }
            bool used[2 * 288] = {};
            u32 total = count;
            for (u32 merges = 0; merges < count - 1; merges++)
            {
                i32 a = -1, b = -1;
                for (u32 i = 0; i < total; i++)
                {
                    if (used[i])
                        continue;
                    if (a < 0 || nodes[i].freq < nodes[a].freq)
                    {
                        b = a;
                        a = i32(i);
                    }
                    else if (b < 0 || nodes[i].freq < nodes[b].freq)
                        b = i32(i);
                }
                used[a] = used[b] = true;
                nodes[total] = {nodes[a].freq + nodes[b].freq, -1};
                nodes[a].parent = nodes[b].parent = i32(total);
                total++;
            }
            u32 blCount[64] = {};
            u32 depth[288];
            u32 maxLength = 0;
            for (u32 i = 0; i < count; i++)
            {
                u32 d = 0;
                for (i32 p = nodes[i].parent; p >= 0; p = nodes[p].parent)
                    d++;
                depth[i] = d;
                blCount[d]++;
                maxLength = Max(maxLength, d);
            }
            for (u32 i = maxLength; i > limit; i--)
            {
                while (blCount[i] > 0)
                {
                    u32 j = i - 2;
                    while (blCount[j] == 0)
                        j--;
                    blCount[i] -= 2;
                    blCount[i - 1] += 1;
                    blCount[j + 1] += 2;
                    blCount[j] -= 1;
                }
            }
            // Most frequent symbols get the shortest codes.
            u32 order[288];
            for (u32 i = 0; i < count; i++)
                order[i] = i;
            for (u32 i = 1; i < count; i++)
            {
                u32 v = order[i], j = i;
                while (j > 0 && (nodes[order[j - 1]].freq < nodes[v].freq || (nodes[order[j - 1]].freq == nodes[v].freq && depth[order[j - 1]] > depth[v])))
                {
                    order[j] = order[j - 1];
                    j--;
                }
                order[j] = v;
            }
            u32 at = 0;
            for (u32 len = 1; len <= limit; len++)
                for (u32 k = 0; k < blCount[len]; k++)
                    lengths[leaf[order[at++]]] = u8(len);
        }

        void BuildCodes(const u8* lengths, u32 n, u16* codes)
        {
            u32 blCount[16] = {}, next[16] = {};
            for (u32 i = 0; i < n; i++)
                blCount[lengths[i]]++;
            blCount[0] = 0;
            u32 code = 0;
            for (u32 len = 1; len < 16; len++)
            {
                code = (code + blCount[len - 1]) << 1;
                next[len] = code;
            }
            for (u32 i = 0; i < n; i++)
                codes[i] = lengths[i] ? u16(ReverseBits(next[lengths[i]]++, lengths[i])) : 0;
        }

        u32 LengthCode(u32 length)
        {
            u32 c = 0;
            while (c < 28 && kLengthBase[c + 1] <= length)
                c++;
            return c;
        }

        // Distances 1..256 index the first half directly, larger ones by (distance - 1) >> 7 in the second half.
        struct DistanceTable
        {
            u8 codes[512];
            DistanceTable()
            {
                for (u32 i = 0; i < 512; i++)
                {
                    u32 distance = i < 256 ? i + 1 : ((i - 256) << 7) + 1;
                    u32 c = 0;
                    while (c < 29 && kDistBase[c + 1] <= distance)
                        c++;
                    codes[i] = u8(c);
                }
            }
        };

        const DistanceTable distanceTable;

        inline u32 DistanceCode(u32 distance)
        {
            return distance <= 256 ? distanceTable.codes[distance - 1] : distanceTable.codes[256 + ((distance - 1) >> 7)];
        }

        struct Encoder
        {
            BitWriter w;
            const u8* data;
            usize size;
            u8 lengthCodes[kMaxMatch + 1];

            void WriteStored(usize begin, usize end, bool last)
            {
                do
                {
                    usize n = Min<usize>(end - begin, 65535);
                    bool final = last && begin + n == end;
                    w.Put(final ? 1 : 0, 1);
                    w.Put(0, 2);
                    w.Align();
                    w.Put(u32(n), 16);
                    w.Put(u32(n) ^ 0xFFFF, 16);
                    w.out.Append(data + begin, n);
                    begin += n;
                } while (begin < end);
            }

            void WriteTokens(const Array<Token>& tokens, const u8* litLen, const u8* distLen)
            {
                u16 litCode[288], distCode[30];
                BuildCodes(litLen, 288, litCode);
                BuildCodes(distLen, 30, distCode);
                for (const Token& t : tokens)
                {
                    if (!t.distance)
                    {
                        w.Put(litCode[t.length], litLen[t.length]);
                        continue;
                    }
                    u32 lc = lengthCodes[t.length];
                    w.Put(litCode[257 + lc], litLen[257 + lc]);
                    w.Put(t.length - kLengthBase[lc], kLengthExtra[lc]);
                    u32 dc = DistanceCode(t.distance);
                    w.Put(distCode[dc], distLen[dc]);
                    w.Put(t.distance - kDistBase[dc], kDistExtra[dc]);
                }
                w.Put(litCode[256], litLen[256]);
            }

            u64 TokenBits(const u32* litFreq, const u32* distFreq, const u8* litLen, const u8* distLen)
            {
                u64 bits = 0;
                for (u32 i = 0; i < 286; i++)
                    bits += u64(litFreq[i]) * (litLen[i] + (i >= 257 ? kLengthExtra[i - 257] : 0));
                for (u32 i = 0; i < 30; i++)
                    bits += u64(distFreq[i]) * (distLen[i] + kDistExtra[i]);
                return bits;
            }

            // Code length sequence of a dynamic header with runs encoded as symbols 16, 17 and 18.
            static void RunLengths(const u8* lengths, u32 n, Array<u16>& symbols)
            {
                for (u32 i = 0; i < n;)
                {
                    u8 v = lengths[i];
                    u32 run = 1;
                    while (i + run < n && lengths[i + run] == v)
                        run++;
                    u32 left = run;
                    if (v == 0)
                    {
                        while (left >= 11)
                        {
                            u32 r = Min(left, 138u);
                            symbols.Push(u16(18 | ((r - 11) << 5)));
                            left -= r;
                        }
                        if (left >= 3)
                        {
                            symbols.Push(u16(17 | ((left - 3) << 5)));
                            left = 0;
                        }
                    }
                    else
                    {
                        symbols.Push(v);
                        left--;
                        while (left >= 3)
                        {
                            u32 r = Min(left, 6u);
                            symbols.Push(u16(16 | ((r - 3) << 5)));
                            left -= r;
                        }
                    }
                    for (; left > 0; left--)
                        symbols.Push(v);
                    i += run;
                }
            }

            void WriteBlock(const Array<Token>& tokens, usize begin, usize end, bool last)
            {
                u32 litFreq[288] = {}, distFreq[30] = {};
                for (const Token& t : tokens)
                {
                    if (!t.distance)
                        litFreq[t.length]++;
                    else
                    {
                        litFreq[257 + lengthCodes[t.length]]++;
                        distFreq[DistanceCode(t.distance)]++;
                    }
                }
                litFreq[256] = 1;
                u8 fixedLit[288], fixedDist[30];
                FixedLengths(fixedLit, fixedDist);

                u8 litLen[288], distLen[30];
                BuildLengths(litFreq, 286, 15, litLen);
                litLen[286] = litLen[287] = 0;
                u32 usedDist = 0;
                for (u32 f : distFreq)
                    usedDist += f ? 1 : 0;
                u32 distForTree[30];
                std::memcpy(distForTree, distFreq, sizeof(distFreq));
                if (usedDist < 2)
                    distForTree[0] = distForTree[1] = Max(1u, distForTree[0]);
                BuildLengths(distForTree, 30, 15, distLen);

                u32 hlit = 286, hdist = 30;
                while (hlit > 257 && !litLen[hlit - 1])
                    hlit--;
                while (hdist > 1 && !distLen[hdist - 1])
                    hdist--;
                u8 combined[286 + 30];
                std::memcpy(combined, litLen, hlit);
                std::memcpy(combined + hlit, distLen, hdist);
                Array<u16> runs;
                RunLengths(combined, hlit + hdist, runs);
                u32 clFreq[19] = {};
                for (u16 s : runs)
                    clFreq[s & 31]++;
                u8 clLen[19];
                BuildLengths(clFreq, 19, 7, clLen);
                u32 hclen = 19;
                while (hclen > 4 && !clLen[kCodeLengthOrder[hclen - 1]])
                    hclen--;

                u64 headerBits = 14 + 3 * hclen;
                for (u16 s : runs)
                {
                    u32 sym = s & 31;
                    headerBits += clLen[sym] + (sym == 16 ? 2 : sym == 17 ? 3 : sym == 18 ? 7 : 0);
                }
                u64 dynamicBits = 3 + headerBits + TokenBits(litFreq, distFreq, litLen, distLen);
                u64 fixedBits = 3 + TokenBits(litFreq, distFreq, fixedLit, fixedDist);
                u64 storedBits = (u64(end - begin) + 5 * ((end - begin) / 65535 + 1)) * 8 + 8;

                if (storedBits <= dynamicBits && storedBits <= fixedBits)
                    WriteStored(begin, end, last);
                else if (fixedBits <= dynamicBits)
                {
                    w.Put(last ? 1 : 0, 1);
                    w.Put(1, 2);
                    WriteTokens(tokens, fixedLit, fixedDist);
                }
                else
                {
                    w.Put(last ? 1 : 0, 1);
                    w.Put(2, 2);
                    w.Put(hlit - 257, 5);
                    w.Put(hdist - 1, 5);
                    w.Put(hclen - 4, 4);
                    for (u32 i = 0; i < hclen; i++)
                        w.Put(clLen[kCodeLengthOrder[i]], 3);
                    u16 clCode[19];
                    BuildCodes(clLen, 19, clCode);
                    for (u16 s : runs)
                    {
                        u32 sym = s & 31, extra = s >> 5;
                        w.Put(clCode[sym], clLen[sym]);
                        if (sym == 16)
                            w.Put(extra, 2);
                        else if (sym == 17)
                            w.Put(extra, 3);
                        else if (sym == 18)
                            w.Put(extra, 7);
                    }
                    WriteTokens(tokens, litLen, distLen);
                }
            }

            // Compresses [begin, size) of the input; earlier bytes only serve as the match dictionary.
            // A chunk that is not final ends on a byte boundary so chunks can be concatenated.
            void Run(int level, usize begin, bool final)
            {
                for (u32 len = kMinMatch; len <= kMaxMatch; len++)
                    lengthCodes[len] = u8(LengthCode(len));
                if (level <= 0 || size - begin < kMinMatch)
                {
                    if (size > begin || final)
                        WriteStored(begin, size, final);
                    return;
                }
                static constexpr u32 kChain[10] = {0, 4, 8, 16, 32, 64, 128, 256, 1024, 4096};
                static constexpr u32 kNice[10] = {0, 8, 16, 32, 64, 128, 128, 258, 258, 258};
                u32 maxChain = kChain[Min(level, 9)], nice = kNice[Min(level, 9)];
                bool lazy = level >= 4;
                Array<i32> head;
                head.Resize(1 << kHashBits, -1);
                Array<i32> prev;
                prev.Resize(kWindow, -1);
                auto hash = [&](usize i) { return ((u32(data[i]) << 10) ^ (u32(data[i + 1]) << 5) ^ data[i + 2]) & ((1u << kHashBits) - 1); };
                auto insert = [&](usize i) {
                    if (i + kMinMatch > size)
                        return;
                    u32 h = hash(i);
                    prev[i & (kWindow - 1)] = head[h];
                    head[h] = i32(i);
                };
                auto findMatch = [&](usize i, u32& bestDist) {
                    u32 best = 0;
                    if (i + kMinMatch > size)
                        return best;
                    u32 limit = u32(Min<usize>(kMaxMatch, size - i));
                    i32 candidate = head[hash(i)];
                    for (u32 chain = 0; candidate >= 0 && chain < maxChain; chain++)
                    {
                        usize c = usize(candidate);
                        if (i - c > kWindow - 1 || c >= i)
                            break;
                        if (data[c + best] == data[i + best])
                        {
                            // Compare eight bytes at a time; the first differing byte comes from the lowest set bit.
                            u32 len = 0;
                            while (len + 8 <= limit)
                            {
                                u64 x, y;
                                std::memcpy(&x, data + c + len, 8);
                                std::memcpy(&y, data + i + len, 8);
                                if (x != y)
                                {
                                    len += u32(std::countr_zero(x ^ y) >> 3);
                                    break;
                                }
                                len += 8;
                            }
                            if (len + 8 > limit)
                                while (len < limit && data[c + len] == data[i + len])
                                    len++;
                            if (len > best)
                            {
                                best = len;
                                bestDist = u32(i - c);
                                if (len >= nice)
                                    break;
                            }
                        }
                        candidate = prev[c & (kWindow - 1)];
                    }
                    return best >= kMinMatch ? best : 0;
                };

                for (usize j = begin > kWindow ? begin - kWindow : 0; j < begin; j++)
                    insert(j);
                Array<Token> tokens;
                usize blockStart = begin;
                usize i = begin;
                while (i < size)
                {
                    u32 dist = 0;
                    u32 len = findMatch(i, dist);
                    insert(i);
                    if (lazy && len && len < nice && i + 1 < size)
                    {
                        u32 nextDist = 0;
                        u32 next = findMatch(i + 1, nextDist);
                        if (next > len)
                        {
                            tokens.Push({data[i], 0});
                            i++;
                            insert(i);
                            len = next;
                            dist = nextDist;
                        }
                    }
                    if (len)
                    {
                        tokens.Push({u16(len), u16(dist)});
                        for (u32 k = 1; k < len; k++)
                            insert(i + k);
                        i += len;
                    }
                    else
                    {
                        tokens.Push({data[i], 0});
                        i++;
                    }
                    if (tokens.Size() >= kBlockTokens)
                    {
                        WriteBlock(tokens, blockStart, i, final && i >= size);
                        tokens.Clear();
                        blockStart = i;
                    }
                }
                if (!tokens.Empty() || blockStart == begin)
                    WriteBlock(tokens, blockStart, size, final);
                if (!final)
                {
                    // An empty stored block pads to the next byte without ending the stream.
                    w.Put(0, 3);
                    w.Align();
                    w.Put(0, 16);
                    w.Put(0xFFFF, 16);
                }
                w.Align();
            }
        };
    }

    namespace
    {
        // Large inputs are cut into chunks that compress independently, each using the 32 KB before it as dictionary.
        // The cut depends only on the input size, so the output is the same with any number of workers.
        constexpr usize kChunkSize = 256 * 1024;

        struct ChunkContext
        {
            const u8* data;
            usize size;
            int level;
            Array<u8>* parts;
            u32 count;
        };

        void CompressChunks(void* user, u32 begin, u32 end)
        {
            const ChunkContext& c = *static_cast<const ChunkContext*>(user);
            for (u32 i = begin; i < end; i++)
            {
                usize from = usize(i) * kChunkSize;
                usize to = i + 1 == c.count ? c.size : from + kChunkSize;
                Encoder e{BitWriter{c.parts[i]}, c.data, to, {}};
                e.Run(c.level, from, i + 1 == c.count);
            }
        }
    }

    void Deflate(Span<const u8> in, Array<u8>& out, int level)
    {
        if (in.Size() < 2 * kChunkSize)
        {
            Encoder e{BitWriter{out}, in.Data(), in.Size(), {}};
            e.Run(level, 0, true);
            return;
        }
        u32 count = u32((in.Size() + kChunkSize - 1) / kChunkSize);
        Array<Array<u8>> parts;
        parts.Resize(count);
        ChunkContext context{in.Data(), in.Size(), level, parts.Data(), count};
        if (jobs::IsInitialized())
            jobs::Complete(jobs::ScheduleParallelFor(&CompressChunks, &context, count, 1));
        else
            CompressChunks(&context, 0, count);
        for (const Array<u8>& part : parts)
            out.Append(part.Data(), part.Size());
    }

    void ZlibCompress(Span<const u8> in, Array<u8>& out, int level)
    {
        u32 flevel = level <= 1 ? 0 : level <= 5 ? 1 : level == 6 ? 2 : 3;
        u32 header = (0x78u << 8) | (flevel << 6);
        header += 31 - header % 31;
        out.Push(u8(header >> 8));
        out.Push(u8(header));
        Deflate(in, out, level);
        u32 adler = Adler32(in.Data(), in.Size());
        for (int shift = 24; shift >= 0; shift -= 8)
            out.Push(u8(adler >> shift));
    }
}
