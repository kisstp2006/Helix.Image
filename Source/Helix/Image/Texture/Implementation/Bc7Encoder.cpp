#include <Helix/Image/Texture/Implementation/BptcTables.h>
#include <Helix/Image/Texture/Implementation/BuiltInBlockCodecs.h>

#include <cmath>
#include <cstring>

// The BC7 encoder: per subset a principal axis fit, quantization with the best p bits, nearest palette indices and a
// least squares refinement; partitions are ranked by a quick line fit error and the best ones encoded fully.
// Fast: mode 6. Normal: modes 1, 3, 5 and 6 (1 and 3 only for opaque blocks). Best: every mode, rotation and partition.
namespace helix::image::bptc
{
    namespace
    {
        struct Texels
        {
            i32 v[16][4];
        };

        // The principal axis of the texels of a subset (power iteration on the covariance), with their mean.
        void Axis(const Texels& t, const u32* members, u32 count, u32 channels, float mean[4], float axis[4])
        {
            for (u32 c = 0; c < 4; c++)
                mean[c] = axis[c] = 0.0f;
            for (u32 i = 0; i < count; i++)
                for (u32 c = 0; c < channels; c++)
                    mean[c] += float(t.v[members[i]][c]);
            for (u32 c = 0; c < channels; c++)
                mean[c] /= float(count);
            float cov[4][4] = {};
            for (u32 i = 0; i < count; i++)
                for (u32 a = 0; a < channels; a++)
                    for (u32 b = 0; b < channels; b++)
                        cov[a][b] += (float(t.v[members[i]][a]) - mean[a]) * (float(t.v[members[i]][b]) - mean[b]);
            float v[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            for (int iteration = 0; iteration < 8; iteration++)
            {
                float n[4] = {};
                for (u32 a = 0; a < channels; a++)
                    for (u32 b = 0; b < channels; b++)
                        n[a] += cov[a][b] * v[b];
                float length = 0.0f;
                for (u32 a = 0; a < channels; a++)
                    length += n[a] * n[a];
                length = std::sqrt(length);
                if (length < 1e-12f)
                    break;
                for (u32 a = 0; a < channels; a++)
                    v[a] = n[a] / length;
            }
            for (u32 c = 0; c < channels; c++)
                axis[c] = v[c];
        }

        // What is left after the best line through the texels: the quick error of a partition.
        float LineError(const Texels& t, const u32* members, u32 count, u32 channels)
        {
            float mean[4], axis[4];
            Axis(t, members, count, channels, mean, axis);
            float error = 0.0f;
            for (u32 i = 0; i < count; i++)
            {
                float d[4], along = 0.0f;
                for (u32 c = 0; c < channels; c++)
                {
                    d[c] = float(t.v[members[i]][c]) - mean[c];
                    along += d[c] * axis[c];
                }
                for (u32 c = 0; c < channels; c++)
                {
                    float r = d[c] - along * axis[c];
                    error += r * r;
                }
            }
            return error;
        }

        // Quantizes an endpoint to `bits` (plus a p bit when `pBit` is 0 or 1) and gives the stored value and the 8 bit
        // value it decodes to.
        void Quantize(float value, u32 bits, i32 pBit, u32& stored, u32& decoded)
        {
            float v = value < 0.0f ? 0.0f : value > 255.0f ? 255.0f : value;
            if (pBit < 0)
            {
                u32 levels = (1u << bits) - 1;
                stored = u32(std::lround(v * float(levels) / 255.0f));
                decoded = Unquantize(stored, bits);
                return;
            }
            u32 levels = (1u << (bits + 1)) - 1;
            i32 full = i32(std::lround(v * float(levels) / 255.0f));
            i32 s = (full - pBit) >> 1;
            s = s < 0 ? 0 : s > i32((1u << bits) - 1) ? i32((1u << bits) - 1) : s;
            // The neighbour with the same p bit may be closer.
            u32 best = u32(s), bestDecoded = Unquantize((u32(s) << 1) | u32(pBit), bits + 1);
            for (i32 n = s - 1; n <= s + 1; n++)
            {
                if (n < 0 || n > i32((1u << bits) - 1))
                    continue;
                u32 d = Unquantize((u32(n) << 1) | u32(pBit), bits + 1);
                if (std::fabs(float(d) - v) < std::fabs(float(bestDecoded) - v))
                {
                    best = u32(n);
                    bestDecoded = d;
                }
            }
            stored = best;
            decoded = bestDecoded;
        }

        struct Fit
        {
            float e[2][4];
            u32 stored[2][4];
            u32 decoded[2][4];
            u32 p[2];
        };

        struct Choice
        {
            u32 mode, partition, rotation, selection;
        };

        u64 Distance(const i32* a, const u32* b, u32 first, u32 last)
        {
            u64 d = 0;
            for (u32 c = first; c < last; c++)
            {
                i32 x = a[c] - i32(b[c]);
                d += u64(x * x);
            }
            return d;
        }

        // Nearest palette entry of every member for the channels [first, last); the total error.
        u64 Assign(const Texels& t, const u32* members, u32 count, const Fit& s, u32 bits, u32 first, u32 last, u32* indices)
        {
            const u8* w = Weights(bits);
            u32 levels = 1u << bits;
            u32 palette[16][4];
            for (u32 k = 0; k < levels; k++)
                for (u32 c = 0; c < 4; c++)
                    palette[k][c] = Interpolate(s.decoded[0][c], s.decoded[1][c], w[k]);
            u64 total = 0;
            for (u32 i = 0; i < count; i++)
            {
                u64 best = ~0ull;
                u32 bestIndex = 0;
                for (u32 k = 0; k < levels; k++)
                {
                    u64 d = Distance(t.v[members[i]], palette[k], first, last);
                    if (d < best)
                    {
                        best = d;
                        bestIndex = k;
                    }
                }
                indices[members[i]] = bestIndex;
                total += best;
            }
            return total;
        }

        // Endpoints quantized with the p bits that suit them best (each endpoint its own, or one per subset).
        void QuantizeSubset(Fit& s, const Bc7Mode& m, u32 first, u32 last, u32 bits)
        {
            bool hasP = m.endpointPBits || m.sharedPBits;
            for (u32 e = 0; e < 2; e++)
                s.p[e] = 0;
            float bestError = 1e30f;
            u32 combinations = !hasP ? 1 : m.sharedPBits ? 2 : 4;
            Fit best = s;
            for (u32 combo = 0; combo < combinations; combo++)
            {
                Fit trial = s;
                trial.p[0] = combo & 1;
                trial.p[1] = m.sharedPBits ? (combo & 1) : (combo >> 1) & 1;
                float error = 0.0f;
                for (u32 e = 0; e < 2; e++)
                    for (u32 c = first; c < last; c++)
                    {
                        Quantize(trial.e[e][c], bits, hasP ? i32(trial.p[e]) : -1, trial.stored[e][c], trial.decoded[e][c]);
                        float d = float(trial.decoded[e][c]) - trial.e[e][c];
                        error += d * d;
                    }
                if (error < bestError)
                {
                    bestError = error;
                    best = trial;
                }
            }
            s = best;
        }

        // Least squares endpoints for the given indices.
        void Refit(const Texels& t, const u32* members, u32 count, const u32* indices, u32 bits, u32 first, u32 last, Fit& s)
        {
            const u8* w = Weights(bits);
            float aa = 0, ab = 0, bb = 0, ax[4] = {}, bx[4] = {};
            for (u32 i = 0; i < count; i++)
            {
                float b = float(w[indices[members[i]]]) / 64.0f, a = 1.0f - b;
                aa += a * a;
                ab += a * b;
                bb += b * b;
                for (u32 c = first; c < last; c++)
                {
                    ax[c] += a * float(t.v[members[i]][c]);
                    bx[c] += b * float(t.v[members[i]][c]);
                }
            }
            float det = aa * bb - ab * ab;
            if (std::fabs(det) < 1e-6f)
                return;
            for (u32 c = first; c < last; c++)
            {
                s.e[0][c] = (ax[c] * bb - bx[c] * ab) / det;
                s.e[1][c] = (bx[c] * aa - ax[c] * ab) / det;
            }
        }

        // Fits, quantizes and indexes one subset for the channels [first, last) with `bits` index bits.
        u64 FitSubset(const Texels& t, const u32* members, u32 count, const Bc7Mode& m, u32 endpointBits, u32 indexBits, u32 first, u32 last,
                      u32 refinements, Fit& s, u32* indices)
        {
            float mean[4], axis[4];
            Texels shifted = t;
            if (first > 0)
                for (u32 i = 0; i < 16; i++)
                    for (u32 c = 0; c < last - first; c++)
                        shifted.v[i][c] = t.v[i][first + c];
            Axis(shifted, members, count, last - first, mean, axis);
            float lo = 1e30f, hi = -1e30f;
            for (u32 i = 0; i < count; i++)
            {
                float along = 0.0f;
                for (u32 c = 0; c < last - first; c++)
                    along += (float(shifted.v[members[i]][c]) - mean[c]) * axis[c];
                lo = along < lo ? along : lo;
                hi = along > hi ? along : hi;
            }
            for (u32 c = 0; c < last - first; c++)
            {
                s.e[0][first + c] = mean[c] + axis[c] * lo;
                s.e[1][first + c] = mean[c] + axis[c] * hi;
            }
            QuantizeSubset(s, m, first, last, endpointBits);
            u64 error = Assign(t, members, count, s, indexBits, first, last, indices);
            for (u32 r = 0; r < refinements; r++)
            {
                Fit trial = s;
                u32 trialIndices[16];
                std::memcpy(trialIndices, indices, sizeof(trialIndices));
                Refit(t, members, count, trialIndices, indexBits, first, last, trial);
                QuantizeSubset(trial, m, first, last, endpointBits);
                u64 e = Assign(t, members, count, trial, indexBits, first, last, trialIndices);
                if (e >= error)
                    break;
                error = e;
                s = trial;
                std::memcpy(indices, trialIndices, sizeof(trialIndices));
            }
            return error;
        }

        // Encodes the block in one mode, partition, rotation and index selection; the error, and the block in `out`.
        u64 EncodeMode(const Texels& source, const Choice& choice, u32 refinements, u8* out)
        {
            const Bc7Mode& m = kBc7Modes[choice.mode];
            Texels t = source;
            if (choice.rotation)
                for (u32 i = 0; i < 16; i++)
                {
                    i32 a = t.v[i][3];
                    t.v[i][3] = t.v[i][choice.rotation - 1];
                    t.v[i][choice.rotation - 1] = a;
                }
            u32 members[3][16], counts[3] = {};
            for (u32 i = 0; i < 16; i++)
            {
                u32 s = Subset(m.subsets, choice.partition, i);
                members[s][counts[s]++] = i;
            }
            Fit subsets[3];
            u32 colorIndices[16] = {}, alphaIndices[16] = {};
            u64 error = 0;
            bool separate = m.secondaryBits > 0;
            // Mode 4 with selection 1 gives color the 3 bit indices and alpha the 2 bit ones.
            u32 colorIndexBits = separate && choice.selection ? m.secondaryBits : m.indexBits;
            u32 alphaIndexBits = separate ? (choice.selection ? m.indexBits : m.secondaryBits) : m.indexBits;
            for (u32 s = 0; s < m.subsets; s++)
            {
                if (counts[s] == 0)
                    continue;
                if (separate)
                {
                    Fit alpha;
                    error += FitSubset(t, members[s], counts[s], m, m.colorBits, colorIndexBits, 0, 3, refinements, subsets[s], colorIndices);
                    error += FitSubset(t, members[s], counts[s], m, m.alphaBits, alphaIndexBits, 3, 4, refinements, alpha, alphaIndices);
                    for (u32 e = 0; e < 2; e++)
                    {
                        subsets[s].stored[e][3] = alpha.stored[e][3];
                        subsets[s].decoded[e][3] = alpha.decoded[e][3];
                    }
                }
                else
                {
                    u32 last = m.alphaBits ? 4 : 3;
                    error += FitSubset(t, members[s], counts[s], m, m.colorBits, m.indexBits, 0, last, refinements, subsets[s], colorIndices);
                    if (!m.alphaBits)
                        for (u32 i = 0; i < counts[s]; i++)
                        {
                            i32 d = t.v[members[s][i]][3] - 255;
                            error += u64(d * d);
                        }
                }
            }
            // The anchor index of each subset must have its top bit clear: swap the endpoints and mirror the indices.
            for (u32 s = 0; s < m.subsets; s++)
            {
                u32 anchor = s == 0 ? 0 : m.subsets == 2 ? kAnchors2[choice.partition] : kAnchors3[s - 1][choice.partition];
                u32 top = 1u << (colorIndexBits - 1);
                if (colorIndices[anchor] & top)
                {
                    for (u32 c = 0; c < (separate ? 3u : 4u); c++)
                    {
                        u32 tmp = subsets[s].stored[0][c];
                        subsets[s].stored[0][c] = subsets[s].stored[1][c];
                        subsets[s].stored[1][c] = tmp;
                    }
                    u32 p = subsets[s].p[0];
                    subsets[s].p[0] = subsets[s].p[1];
                    subsets[s].p[1] = p;
                    for (u32 i = 0; i < counts[s]; i++)
                        colorIndices[members[s][i]] = (1u << colorIndexBits) - 1 - colorIndices[members[s][i]];
                }
            }
            if (separate && (alphaIndices[0] & (1u << (alphaIndexBits - 1))))
            {
                u32 tmp = subsets[0].stored[0][3];
                subsets[0].stored[0][3] = subsets[0].stored[1][3];
                subsets[0].stored[1][3] = tmp;
                for (u32 i = 0; i < 16; i++)
                    alphaIndices[i] = (1u << alphaIndexBits) - 1 - alphaIndices[i];
            }

            BitWriter128 w;
            w.Put(1u << choice.mode, choice.mode + 1);
            w.Put(choice.partition, m.partitionBits);
            w.Put(choice.rotation, m.rotationBits);
            w.Put(choice.selection, m.selectionBits);
            for (u32 c = 0; c < 3; c++)
                for (u32 s = 0; s < m.subsets; s++)
                    for (u32 e = 0; e < 2; e++)
                        w.Put(subsets[s].stored[e][c], m.colorBits);
            if (m.alphaBits)
                for (u32 s = 0; s < m.subsets; s++)
                    for (u32 e = 0; e < 2; e++)
                        w.Put(subsets[s].stored[e][3], m.alphaBits);
            for (u32 s = 0; s < m.subsets; s++)
            {
                if (m.sharedPBits)
                    w.Put(subsets[s].p[0], 1);
                else if (m.endpointPBits)
                {
                    w.Put(subsets[s].p[0], 1);
                    w.Put(subsets[s].p[1], 1);
                }
            }
            // Index sets in the order the decoder reads them: mode 4 with selection 1 stores the alpha set first.
            const u32* primary = separate && choice.selection ? alphaIndices : colorIndices;
            const u32* secondary = separate && choice.selection ? colorIndices : alphaIndices;
            for (u32 i = 0; i < 16; i++)
                w.Put(primary[i], m.indexBits - (IsAnchor(m.subsets, choice.partition, i) ? 1 : 0));
            if (separate)
                for (u32 i = 0; i < 16; i++)
                    w.Put(secondary[i], m.secondaryBits - (i == 0 ? 1 : 0));
            std::memcpy(out, w.block, 16);
            return error;
        }

        void EncodeBc7(const u8* texels, u8* block, CompressionQuality quality)
        {
            Texels t;
            bool opaque = true;
            for (u32 i = 0; i < 16; i++)
            {
                for (u32 c = 0; c < 4; c++)
                    t.v[i][c] = texels[i * 4 + c];
                opaque = opaque && texels[i * 4 + 3] == 255;
            }
            u32 modes[8], modeCount = 0;
            if (quality == CompressionQuality::Fast)
                modes[modeCount++] = 6;
            else if (quality == CompressionQuality::Normal)
            {
                if (opaque)
                {
                    modes[modeCount++] = 1;
                    modes[modeCount++] = 3;
                }
                modes[modeCount++] = 5;
                modes[modeCount++] = 6;
            }
            else
                for (u32 m = 0; m < 8; m++)
                    modes[modeCount++] = m;
            u32 refinements = quality == CompressionQuality::Fast ? 1 : quality == CompressionQuality::Normal ? 2 : 4;
            u32 keep = quality == CompressionQuality::Best ? 8 : 3;
            u64 best = ~0ull;
            u8 trial[16];
            for (u32 k = 0; k < modeCount; k++)
            {
                u32 mode = modes[k];
                const Bc7Mode& m = kBc7Modes[mode];
                u32 partitions = 1u << m.partitionBits;
                // Rank the partitions by how well a line fits each subset.
                u32 ranked[64];
                float scores[64];
                u32 ranks = 0;
                for (u32 p = 0; p < partitions; p++)
                {
                    u32 members[3][16], counts[3] = {};
                    for (u32 i = 0; i < 16; i++)
                    {
                        u32 s = Subset(m.subsets, p, i);
                        members[s][counts[s]++] = i;
                    }
                    float score = 0.0f;
                    for (u32 s = 0; s < m.subsets; s++)
                        if (counts[s])
                            score += LineError(t, members[s], counts[s], m.alphaBits ? 4 : 3);
                    u32 at = ranks++;
                    while (at > 0 && scores[at - 1] > score)
                    {
                        scores[at] = scores[at - 1];
                        ranked[at] = ranked[at - 1];
                        at--;
                    }
                    scores[at] = score;
                    ranked[at] = p;
                }
                u32 tried = ranks < keep ? ranks : keep;
                for (u32 r = 0; r < tried; r++)
                    for (u32 rotation = 0; rotation < (1u << m.rotationBits); rotation++)
                        for (u32 selection = 0; selection < (1u << m.selectionBits); selection++)
                        {
                            if (quality != CompressionQuality::Best && rotation > 0 && opaque)
                                continue;
                            u64 e = EncodeMode(t, {mode, ranked[r], rotation, selection}, refinements, trial);
                            if (e < best)
                            {
                                best = e;
                                std::memcpy(block, trial, 16);
                            }
                        }
            }
        }
    }

    void AddBc6Encoders(BlockCodec* codecs, usize count);

    void AddEncoders(BlockCodec* codecs, usize count)
    {
        SetEncoder(codecs, count, PixelFormat::BC7, &EncodeBc7);
        AddBc6Encoders(codecs, count);
    }
}
