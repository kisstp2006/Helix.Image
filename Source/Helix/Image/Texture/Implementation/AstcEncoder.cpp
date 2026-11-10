#include <Helix/Image/Texture/Implementation/AstcCommon.h>
#include <Helix/Image/Texture/Implementation/BuiltInBlockCodecs.h>

#include <cmath>
#include <cstring>

// The ASTC LDR encoder. Candidates (block mode, partitions, dual plane) get endpoints along the principal axis of each
// partition, ideal weights by projection sampled onto the weight grid, quantized values, and are packed and decoded
// again so the error is exact. Fast: one partition. Normal: two partitions and a separate alpha plane too. Best: more
// block modes and partition seeds. Blocks of one color become void extent blocks.
namespace helix::image::astc
{
    namespace
    {
        struct Codes
        {
            // The trit and quint blocks that decode to each tuple.
            u8 trits[3][3][3][3][3];
            u8 quints[5][5][5];
            // Nearest code of each 8 bit endpoint value, per color range; of each weight 0..64, per weight range.
            u8 color[21][256];
            u8 weight[12][65];

            static u32 ColorIndex(u32 levels)
            {
                static constexpr u32 kLevels[21] = {2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 160, 192, 256};
                for (u32 i = 0; i < 21; i++)
                    if (kLevels[i] == levels)
                        return i;
                return 0;
            }

            static u32 WeightIndex(u32 levels)
            {
                static constexpr u32 kLevels[12] = {2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32};
                for (u32 i = 0; i < 12; i++)
                    if (kLevels[i] == levels)
                        return i;
                return 0;
            }

            Codes()
            {
                for (u32 t = 256; t-- > 0;)
                {
                    u32 v[5];
                    DecodeTrits(t, v);
                    trits[v[0]][v[1]][v[2]][v[3]][v[4]] = u8(t);
                }
                for (u32 q = 128; q-- > 0;)
                {
                    u32 v[3];
                    DecodeQuints(q, v);
                    quints[v[0]][v[1]][v[2]] = u8(q);
                }
                static constexpr u32 kColor[21] = {2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 160, 192, 256};
                for (u32 r = 0; r < 21; r++)
                    for (u32 value = 0; value < 256; value++)
                    {
                        u32 best = 0, bestError = ~0u;
                        for (u32 code = 0; code < kColor[r]; code++)
                        {
                            i32 d = i32(UnquantizeColor(code, kColor[r])) - i32(value);
                            if (u32(d * d) < bestError)
                            {
                                bestError = u32(d * d);
                                best = code;
                            }
                        }
                        color[r][value] = u8(best);
                    }
                static constexpr u32 kWeight[12] = {2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32};
                for (u32 r = 0; r < 12; r++)
                    for (u32 value = 0; value <= 64; value++)
                    {
                        u32 best = 0, bestError = ~0u;
                        for (u32 code = 0; code < kWeight[r]; code++)
                        {
                            i32 d = i32(UnquantizeWeight(code, kWeight[r])) - i32(value);
                            if (u32(d * d) < bestError)
                            {
                                bestError = u32(d * d);
                                best = code;
                            }
                        }
                        weight[r][value] = u8(best);
                    }
            }
        };

        const Codes& CodeTables()
        {
            static const Codes codes;
            return codes;
        }

        void PutBit(u8* block, u32 position, u32 bit) { block[position >> 3] |= u8((bit & 1) << (position & 7)); }

        void PutBits(u8* block, u32 at, u32 value, u32 count)
        {
            for (u32 i = 0; i < count; i++)
                PutBit(block, at + i, value >> i);
        }

        // The inverse of DecodeSequence.
        void EncodeSequence(u8* block, u32 start, u32 levels, u32 count, const u32* values, bool reversed)
        {
            const Codes& codes = CodeTables();
            Range r = RangeOf(levels);
            u32 total = SequenceBits(levels, count), at = 0;
            auto write = [&](u32 v, u32 n) {
                for (u32 i = 0; i < n; i++, at++)
                    if (at < total)
                        PutBit(block, reversed ? 127 - (start + at) : start + at, v >> i);
            };
            u32 mask = (1u << r.bits) - 1;
            if (r.trits)
            {
                static constexpr u32 kSplit[5] = {2, 2, 1, 2, 1};
                for (u32 i = 0; i < count; i += 5)
                {
                    u32 t[5] = {}, m[5] = {};
                    for (u32 k = 0; k < 5 && i + k < count; k++)
                    {
                        t[k] = values[i + k] >> r.bits;
                        m[k] = values[i + k] & mask;
                    }
                    u32 code = codes.trits[t[0]][t[1]][t[2]][t[3]][t[4]], shift = 0;
                    for (u32 k = 0; k < 5; k++)
                    {
                        write(m[k], r.bits);
                        write(code >> shift, kSplit[k]);
                        shift += kSplit[k];
                    }
                }
            }
            else if (r.quints)
            {
                static constexpr u32 kSplit[3] = {3, 2, 2};
                for (u32 i = 0; i < count; i += 3)
                {
                    u32 q[3] = {}, m[3] = {};
                    for (u32 k = 0; k < 3 && i + k < count; k++)
                    {
                        q[k] = values[i + k] >> r.bits;
                        m[k] = values[i + k] & mask;
                    }
                    u32 code = codes.quints[q[0]][q[1]][q[2]], shift = 0;
                    for (u32 k = 0; k < 3; k++)
                    {
                        write(m[k], r.bits);
                        write(code >> shift, kSplit[k]);
                        shift += kSplit[k];
                    }
                }
            }
            else
                for (u32 i = 0; i < count; i++)
                    write(values[i], r.bits);
        }

        struct Mode
        {
            u32 bits;
            BlockMode mode;
            u32 weightBits;
            float score;
        };

        // The valid block modes of a block size, best first by a guess of their quality.
        struct Modes
        {
            Mode modes[2048];
            u32 count = 0;

            Modes(u32 bw, u32 bh)
            {
                for (u32 m = 0; m < 2048; m++)
                {
                    BlockMode b;
                    if ((m & 0x1FF) == 0x1FC || !DecodeBlockMode(m, b) || b.width > bw || b.height > bh)
                        continue;
                    u32 weights = b.width * b.height * (b.dualPlane ? 2 : 1), bits = SequenceBits(b.weightLevels, weights);
                    if (weights > 64 || bits < 24 || bits > 96)
                        continue;
                    // The endpoints of one RGBA partition must still fit at 6 levels or more.
                    if (ColorLevels(8, 128 - bits - 17 - (b.dualPlane ? 2 : 0)) < 0)
                        continue;
                    float score = float(b.width * b.height) * std::log2(float(b.weightLevels)) / float(bw * bh) * 8.0f +
                                  std::log2(float(ColorLevels(8, 128 - bits - 17))) * 2.0f;
                    Mode& out = modes[count++];
                    out = {m, b, bits, score};
                }
                for (u32 i = 1; i < count; i++)
                    for (u32 j = i; j > 0 && modes[j - 1].score < modes[j].score; j--)
                    {
                        Mode t = modes[j - 1];
                        modes[j - 1] = modes[j];
                        modes[j] = t;
                    }
            }
        };

        struct Candidate
        {
            const Mode* mode;
            u32 partitions;
            u32 seed;
            // Dual plane: the component on the second plane.
            u32 plane2;
        };

        // Encodes the block as the candidate and returns the exact error of its decoded texels.
        u64 EncodeCandidate(const u8* texels, u32 bw, u32 bh, const Candidate& c, bool opaque, u8* out)
        {
            const Codes& codes = CodeTables();
            const BlockMode& m = c.mode->mode;
            u32 n = bw * bh;
            bool dual = m.dualPlane;
            u32 cem = opaque && !dual ? 8 : 12, valuesPerPartition = cem == 12 ? 8 : 6;
            u32 colorStart = c.partitions == 1 ? 17 : 29, belowWeights = 128 - c.mode->weightBits - (dual ? 2 : 0);
            i32 colorLevels = ColorLevels(valuesPerPartition * c.partitions, belowWeights - colorStart);
            if (colorLevels < 0)
                return ~0ull;
            u32 partition[144];
            for (u32 i = 0; i < n; i++)
                partition[i] = c.partitions > 1 ? SelectPartition(c.seed, i % bw, i / bw, c.partitions, n < 31) : 0;

            // Per partition: the line through its texels, and each texel's place along it (0 to 1).
            float ideal[2][144] = {};
            float endpoints[4][2][4];
            u32 lineChannels = dual ? 3 : (cem == 12 ? 4 : 3);
            for (u32 p = 0; p < c.partitions; p++)
            {
                float mean[4] = {}, axis[4] = {1, 1, 1, 1};
                u32 count = 0;
                for (u32 i = 0; i < n; i++)
                    if (partition[i] == p)
                    {
                        count++;
                        for (u32 ch = 0; ch < 4; ch++)
                            mean[ch] += float(texels[i * 4 + ch]);
                    }
                if (!count)
                {
                    for (u32 ch = 0; ch < 4; ch++)
                        endpoints[p][0][ch] = endpoints[p][1][ch] = 0;
                    continue;
                }
                for (u32 ch = 0; ch < 4; ch++)
                    mean[ch] /= float(count);
                float cov[4][4] = {};
                for (u32 i = 0; i < n; i++)
                    if (partition[i] == p)
                        for (u32 a = 0; a < lineChannels; a++)
                            for (u32 b = 0; b < lineChannels; b++)
                                cov[a][b] += (float(texels[i * 4 + a]) - mean[a]) * (float(texels[i * 4 + b]) - mean[b]);
                for (int k = 0; k < 8; k++)
                {
                    float v[4] = {}, length = 0;
                    for (u32 a = 0; a < lineChannels; a++)
                        for (u32 b = 0; b < lineChannels; b++)
                            v[a] += cov[a][b] * axis[b];
                    for (u32 a = 0; a < lineChannels; a++)
                        length += v[a] * v[a];
                    length = std::sqrt(length);
                    if (length < 1e-6f)
                        break;
                    for (u32 a = 0; a < lineChannels; a++)
                        axis[a] = v[a] / length;
                }
                for (u32 a = lineChannels; a < 4; a++)
                    axis[a] = 0;
                float lo = 1e30f, hi = -1e30f;
                for (u32 i = 0; i < n; i++)
                    if (partition[i] == p)
                    {
                        float along = 0;
                        for (u32 a = 0; a < lineChannels; a++)
                            along += (float(texels[i * 4 + a]) - mean[a]) * axis[a];
                        lo = along < lo ? along : lo;
                        hi = along > hi ? along : hi;
                    }
                for (u32 ch = 0; ch < 4; ch++)
                {
                    endpoints[p][0][ch] = mean[ch] + axis[ch] * lo;
                    endpoints[p][1][ch] = mean[ch] + axis[ch] * hi;
                }
                for (u32 i = 0; i < n; i++)
                    if (partition[i] == p)
                    {
                        float along = 0;
                        for (u32 a = 0; a < lineChannels; a++)
                            along += (float(texels[i * 4 + a]) - mean[a]) * axis[a];
                        ideal[0][i] = hi > lo ? (along - lo) / (hi - lo) : 0.0f;
                    }
                if (dual)
                {
                    // The second plane carries one component on its own.
                    float a0 = 255, a1 = 0;
                    for (u32 i = 0; i < n; i++)
                    {
                        a0 = std::fmin(a0, float(texels[i * 4 + c.plane2]));
                        a1 = std::fmax(a1, float(texels[i * 4 + c.plane2]));
                    }
                    endpoints[p][0][c.plane2] = a0;
                    endpoints[p][1][c.plane2] = a1;
                    for (u32 i = 0; i < n; i++)
                        ideal[1][i] = a1 > a0 ? (float(texels[i * 4 + c.plane2]) - a0) / (a1 - a0) : 0.0f;
                }
            }

            // Endpoint values; the direct modes want the second endpoint at least as bright, else they blue contract,
            // so a partition whose endpoints are the other way round is swapped and its weights mirrored.
            u32 colorRange = Codes::ColorIndex(u32(colorLevels));
            u32 values[16];
            bool mirrored[4] = {};
            for (u32 p = 0; p < c.partitions; p++)
            {
                u32 q[2][4];
                for (u32 e = 0; e < 2; e++)
                    for (u32 ch = 0; ch < 4; ch++)
                    {
                        float v = endpoints[p][e][ch];
                        u32 byte = u32(v < 0 ? 0 : v > 255 ? 255 : std::lround(v));
                        q[e][ch] = codes.color[colorRange][byte];
                    }
                auto sum = [&](u32 e) { return UnquantizeColor(q[e][0], u32(colorLevels)) + UnquantizeColor(q[e][1], u32(colorLevels)) + UnquantizeColor(q[e][2], u32(colorLevels)); };
                if (sum(1) < sum(0))
                {
                    mirrored[p] = true;
                    for (u32 ch = 0; ch < 4; ch++)
                    {
                        u32 t = q[0][ch];
                        q[0][ch] = q[1][ch];
                        q[1][ch] = t;
                    }
                }
                u32* v = values + p * valuesPerPartition;
                for (u32 ch = 0; ch < (cem == 12 ? 4u : 3u); ch++)
                {
                    v[ch * 2] = q[0][ch];
                    v[ch * 2 + 1] = q[1][ch];
                }
            }

            // Grid weights: the ideal weights at each grid point (bilinear between texels), quantized.
            u32 planes = dual ? 2 : 1, weightRange = Codes::WeightIndex(m.weightLevels);
            u32 weights[64];
            for (u32 gy = 0; gy < m.height; gy++)
                for (u32 gx = 0; gx < m.width; gx++)
                {
                    float tx = m.width > 1 ? float(gx) * float(bw - 1) / float(m.width - 1) : 0.0f;
                    float ty = m.height > 1 ? float(gy) * float(bh - 1) / float(m.height - 1) : 0.0f;
                    u32 x0 = u32(tx), y0 = u32(ty), x1 = x0 + 1 < bw ? x0 + 1 : x0, y1 = y0 + 1 < bh ? y0 + 1 : y0;
                    float fx = tx - float(x0), fy = ty - float(y0);
                    for (u32 plane = 0; plane < planes; plane++)
                    {
                        auto at = [&](u32 x, u32 y) {
                            u32 i = y * bw + x;
                            float w = ideal[plane][i];
                            return plane == 0 && mirrored[partition[i]] ? 1.0f - w : w;
                        };
                        float w = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
                        if (plane == 1 && mirrored[0])
                            w = 1.0f - w;
                        u32 w64 = u32(std::lround((w < 0 ? 0 : w > 1 ? 1 : w) * 64.0f));
                        weights[(gy * m.width + gx) * planes + plane] = codes.weight[weightRange][w64];
                    }
                }

            std::memset(out, 0, 16);
            PutBits(out, 0, c.mode->bits, 11);
            PutBits(out, 11, c.partitions - 1, 2);
            if (c.partitions == 1)
                PutBits(out, 13, cem, 4);
            else
            {
                PutBits(out, 13, c.seed, 10);
                PutBits(out, 23, cem << 2, 6);
            }
            if (dual)
                PutBits(out, belowWeights, c.plane2, 2);
            EncodeSequence(out, colorStart, u32(colorLevels), valuesPerPartition * c.partitions, values, false);
            EncodeSequence(out, 0, m.weightLevels, m.width * m.height * planes, weights, true);

            u8 decoded[144 * 4];
            DecodeBlock(bw, bh, out, decoded);
            u64 error = 0;
            for (u32 i = 0; i < n * 4; i++)
            {
                i32 d = i32(decoded[i]) - i32(texels[i]);
                error += u64(d * d);
            }
            return error;
        }

        // Partition seeds ranked by how well they split the block like two means of its texels.
        u32 RankSeeds(const u8* texels, u32 bw, u32 bh, u32* seeds, u32 keep)
        {
            u32 n = bw * bh;
            float centers[2][4];
            for (u32 ch = 0; ch < 4; ch++)
            {
                centers[0][ch] = 255;
                centers[1][ch] = 0;
            }
            for (u32 i = 0; i < n; i++)
            {
                float l = float(texels[i * 4]) + float(texels[i * 4 + 1]) + float(texels[i * 4 + 2]) + float(texels[i * 4 + 3]);
                float l0 = centers[0][0] + centers[0][1] + centers[0][2] + centers[0][3], l1 = centers[1][0] + centers[1][1] + centers[1][2] + centers[1][3];
                if (l < l0)
                    for (u32 ch = 0; ch < 4; ch++)
                        centers[0][ch] = texels[i * 4 + ch];
                if (l > l1)
                    for (u32 ch = 0; ch < 4; ch++)
                        centers[1][ch] = texels[i * 4 + ch];
            }
            u32 group[144];
            for (int iteration = 0; iteration < 4; iteration++)
            {
                float sum[2][4] = {};
                u32 counts[2] = {};
                for (u32 i = 0; i < n; i++)
                {
                    float d[2] = {};
                    for (u32 k = 0; k < 2; k++)
                        for (u32 ch = 0; ch < 4; ch++)
                            d[k] += (float(texels[i * 4 + ch]) - centers[k][ch]) * (float(texels[i * 4 + ch]) - centers[k][ch]);
                    group[i] = d[1] < d[0] ? 1 : 0;
                    counts[group[i]]++;
                    for (u32 ch = 0; ch < 4; ch++)
                        sum[group[i]][ch] += float(texels[i * 4 + ch]);
                }
                for (u32 k = 0; k < 2; k++)
                    if (counts[k])
                        for (u32 ch = 0; ch < 4; ch++)
                            centers[k][ch] = sum[k][ch] / float(counts[k]);
            }
            u32 kept = 0, mismatches[64];
            for (u32 seed = 0; seed < 1024; seed++)
            {
                u32 same = 0;
                for (u32 i = 0; i < n; i++)
                    same += SelectPartition(seed, i % bw, i / bw, 2, n < 31) == group[i];
                u32 miss = same < n - same ? same : n - same;
                u32 at = kept < keep ? kept++ : keep;
                if (at == keep && miss >= mismatches[keep - 1])
                    continue;
                if (at == keep)
                    at = keep - 1;
                while (at > 0 && mismatches[at - 1] > miss)
                {
                    mismatches[at] = mismatches[at - 1];
                    seeds[at] = seeds[at - 1];
                    at--;
                }
                mismatches[at] = miss;
                seeds[at] = seed;
            }
            return kept;
        }

        template <u32 W, u32 H>
        void Encode(const u8* texels, u8* block, CompressionQuality quality)
        {
            static const Modes modes(W, H);
            constexpr u32 n = W * H;
            bool constant = true, opaque = true;
            for (u32 i = 0; i < n; i++)
            {
                constant = constant && std::memcmp(texels + i * 4, texels, 4) == 0;
                opaque = opaque && texels[i * 4 + 3] == 255;
            }
            if (constant)
            {
                // The void extent block: block mode 0x1FC, LDR, the extent left open, one unorm16 color.
                std::memset(block, 0xFF, 8);
                block[0] = 0xFC;
                block[1] = 0xFD;
                for (u32 c = 0; c < 4; c++)
                {
                    u32 v = texels[c] * 257u;
                    block[8 + 2 * c] = u8(v);
                    block[9 + 2 * c] = u8(v >> 8);
                }
                return;
            }
            u32 tries = quality == CompressionQuality::Fast ? 3 : quality == CompressionQuality::Normal ? 8 : 24;
            u64 best = ~0ull;
            u8 trial[16];
            auto attempt = [&](const Candidate& c) {
                u64 e = EncodeCandidate(texels, W, H, c, opaque, trial);
                if (e < best)
                {
                    best = e;
                    std::memcpy(block, trial, 16);
                }
            };
            u32 seeds[16], seedCount = 0;
            if (quality != CompressionQuality::Fast)
                seedCount = RankSeeds(texels, W, H, seeds, quality == CompressionQuality::Best ? 8 : 2);
            u32 single = 0, dual = 0;
            for (u32 i = 0; i < modes.count && (single < tries || dual < tries); i++)
            {
                const Mode& m = modes.modes[i];
                if (m.mode.dualPlane)
                {
                    if (quality == CompressionQuality::Fast || opaque || dual >= tries)
                        continue;
                    dual++;
                    attempt({&m, 1, 0, 3});
                    continue;
                }
                if (single >= tries)
                    continue;
                single++;
                attempt({&m, 1, 0, 0});
                for (u32 s = 0; s < seedCount; s++)
                    attempt({&m, 2, seeds[s], 0});
            }
        }
    }

    void AddEncoders(BlockCodec* codecs, usize count)
    {
        SetEncoder(codecs, count, PixelFormat::ASTC4x4, &Encode<4, 4>);
        SetEncoder(codecs, count, PixelFormat::ASTC5x4, &Encode<5, 4>);
        SetEncoder(codecs, count, PixelFormat::ASTC5x5, &Encode<5, 5>);
        SetEncoder(codecs, count, PixelFormat::ASTC6x5, &Encode<6, 5>);
        SetEncoder(codecs, count, PixelFormat::ASTC6x6, &Encode<6, 6>);
        SetEncoder(codecs, count, PixelFormat::ASTC8x5, &Encode<8, 5>);
        SetEncoder(codecs, count, PixelFormat::ASTC8x6, &Encode<8, 6>);
        SetEncoder(codecs, count, PixelFormat::ASTC8x8, &Encode<8, 8>);
        SetEncoder(codecs, count, PixelFormat::ASTC10x5, &Encode<10, 5>);
        SetEncoder(codecs, count, PixelFormat::ASTC10x6, &Encode<10, 6>);
        SetEncoder(codecs, count, PixelFormat::ASTC10x8, &Encode<10, 8>);
        SetEncoder(codecs, count, PixelFormat::ASTC10x10, &Encode<10, 10>);
        SetEncoder(codecs, count, PixelFormat::ASTC12x10, &Encode<12, 10>);
        SetEncoder(codecs, count, PixelFormat::ASTC12x12, &Encode<12, 12>);
    }
}
