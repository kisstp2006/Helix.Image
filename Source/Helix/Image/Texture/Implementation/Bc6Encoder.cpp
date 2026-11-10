#include <Helix/Image/Texture/Implementation/BptcTables.h>
#include <Helix/Image/Texture/Implementation/BuiltInBlockCodecs.h>

#include <cmath>
#include <cstring>

// The BC6H encoder. Values are fitted in the space the format interpolates in (where the decoded half is x * 31 / 64,
// or x * 31 / 32 signed), errors are measured on the decoded half values. Fast: mode 11 (one region, 10 bit
// endpoints). Normal: the one region modes and the untransformed two region mode. Best: every mode, the 8 best
// partitions of each two region mode.
namespace helix::image::bptc
{
    namespace
    {
        struct Target
        {
            float x[16][3];
            i32 half[16][3];
        };

        // A half as a signed magnitude, so that differences follow the value order.
        i32 Ordered(u16 h) { return h & 0x8000 ? -i32(h & 0x7FFF) : i32(h); }

        i32 Range(const Bc6Mode& m, bool isSigned, i32& low)
        {
            i32 high = isSigned ? (1 << (m.endpointBits - 1)) - 1 : (1 << m.endpointBits) - 1;
            low = isSigned ? -high : 0;
            return high;
        }

        i32 Quantize(float x, const Bc6Mode& m, bool isSigned)
        {
            i32 low, high = Range(m, isSigned, low);
            float scale = isSigned ? float(1 << (m.endpointBits - 1)) / 32768.0f : float(1 << m.endpointBits) / 65536.0f;
            i32 q = i32(std::lround(x * scale));
            return q < low ? low : q > high ? high : q;
        }

        // Encodes the block in one mode and partition; the error, and the block in `out` (false when the deltas do not
        // fit the mode).
        bool EncodeMode(const Target& t, u32 mode, u32 partition, bool isSigned, u64& error, u8* out)
        {
            const Bc6Mode& m = kBc6Modes[mode];
            u32 members[2][16], counts[2] = {};
            for (u32 i = 0; i < 16; i++)
            {
                u32 s = m.regions == 2 ? (kPartitions2[partition] >> i) & 1 : 0;
                members[s][counts[s]++] = i;
            }
            i32 q[4][3];
            for (u32 s = 0; s < m.regions; s++)
            {
                // The endpoints are the ends of the members along their principal axis.
                float mean[3] = {}, axis[3] = {1, 1, 1};
                for (u32 i = 0; i < counts[s]; i++)
                    for (u32 c = 0; c < 3; c++)
                        mean[c] += t.x[members[s][i]][c] / float(counts[s]);
                float cov[3][3] = {};
                for (u32 i = 0; i < counts[s]; i++)
                    for (u32 a = 0; a < 3; a++)
                        for (u32 b = 0; b < 3; b++)
                            cov[a][b] += (t.x[members[s][i]][a] - mean[a]) * (t.x[members[s][i]][b] - mean[b]);
                for (int k = 0; k < 8; k++)
                {
                    float n[3] = {};
                    for (u32 a = 0; a < 3; a++)
                        for (u32 b = 0; b < 3; b++)
                            n[a] += cov[a][b] * axis[b];
                    float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (length < 1e-6f)
                        break;
                    for (u32 a = 0; a < 3; a++)
                        axis[a] = n[a] / length;
                }
                float lo = 1e30f, hi = -1e30f;
                for (u32 i = 0; i < counts[s]; i++)
                {
                    float along = 0;
                    for (u32 c = 0; c < 3; c++)
                        along += (t.x[members[s][i]][c] - mean[c]) * axis[c];
                    lo = along < lo ? along : lo;
                    hi = along > hi ? along : hi;
                }
                if (counts[s] == 0)
                    lo = hi = 0;
                for (u32 c = 0; c < 3; c++)
                {
                    q[s * 2][c] = Quantize(mean[c] + axis[c] * lo, m, isSigned);
                    q[s * 2 + 1][c] = Quantize(mean[c] + axis[c] * hi, m, isSigned);
                }
            }
            u32 indexBits = m.regions == 2 ? 3 : 4;
            const u8* w = Weights(indexBits);
            u32 indices[16];
            auto assign = [&]() {
                u64 total = 0;
                for (u32 s = 0; s < m.regions; s++)
                {
                    i32 u[2][3];
                    for (u32 e = 0; e < 2; e++)
                        for (u32 c = 0; c < 3; c++)
                            u[e][c] = UnquantizeHalf(q[s * 2 + e][c], m.endpointBits, isSigned);
                    for (u32 i = 0; i < counts[s]; i++)
                    {
                        u32 texel = members[s][i];
                        u64 best = ~0ull;
                        for (u32 k = 0; k < (1u << indexBits); k++)
                        {
                            u64 e = 0;
                            for (u32 c = 0; c < 3; c++)
                            {
                                i32 x = ((64 - i32(w[k])) * u[0][c] + i32(w[k]) * u[1][c] + 32) >> 6;
                                i32 d = Ordered(FinishHalf(x, isSigned)) - t.half[texel][c];
                                e += u64(i64(d) * d);
                            }
                            if (e < best)
                            {
                                best = e;
                                indices[texel] = k;
                            }
                        }
                        total += best;
                    }
                }
                return total;
            };
            error = assign();
            // The anchor index of each region must have its top bit clear.
            for (u32 s = 0; s < m.regions; s++)
            {
                u32 anchor = s == 0 ? 0 : kAnchors2[partition];
                if (indices[anchor] & (1u << (indexBits - 1)))
                {
                    for (u32 c = 0; c < 3; c++)
                    {
                        i32 tmp = q[s * 2][c];
                        q[s * 2][c] = q[s * 2 + 1][c];
                        q[s * 2 + 1][c] = tmp;
                    }
                    for (u32 i = 0; i < counts[s]; i++)
                        indices[members[s][i]] = (1u << indexBits) - 1 - indices[members[s][i]];
                }
            }
            // The stored fields: the first endpoint, then the others as they are or as deltas from it.
            u32 e[4][3] = {};
            u32 mask = (1u << m.endpointBits) - 1;
            for (u32 c = 0; c < 3; c++)
            {
                e[0][c] = u32(q[0][c]) & mask;
                for (u32 k = 1; k < m.regions * 2u; k++)
                {
                    if (m.transformed)
                    {
                        i32 delta = q[k][c] - q[0][c], limit = 1 << (m.deltaBits[c] - 1);
                        if (delta < -limit || delta >= limit)
                            return false;
                        e[k][c] = u32(delta) & ((1u << m.deltaBits[c]) - 1);
                    }
                    else
                        e[k][c] = u32(q[k][c]) & mask;
                }
            }
            BitWriter128 bits;
            bits.Put(m.code, m.code > 1 ? 5 : 2);
            const Bc6Layouts& layout = Bc6LayoutTable();
            for (u32 i = 0; i < layout.count[mode]; i++)
                bits.Put((e[layout.field[mode][i] / 3][layout.field[mode][i] % 3] >> layout.bit[mode][i]) & 1, 1);
            if (m.regions == 2)
                bits.Put(partition, 5);
            for (u32 i = 0; i < 16; i++)
            {
                bool anchor = i == 0 || (m.regions == 2 && i == kAnchors2[partition]);
                bits.Put(indices[i], indexBits - (anchor ? 1 : 0));
            }
            std::memcpy(out, bits.block, 16);
            return true;
        }

        template <bool Signed>
        void EncodeBc6(const u8* texels, u8* block, CompressionQuality quality)
        {
            Target t;
            for (u32 i = 0; i < 16; i++)
                for (u32 c = 0; c < 3; c++)
                {
                    u16 h;
                    std::memcpy(&h, texels + (i * 4 + c) * 2, 2);
                    // Out of range values: negatives in the unsigned format, infinities and NaN.
                    if constexpr (!Signed)
                        h = h & 0x8000 ? u16(0) : h;
                    if ((h & 0x7C00) == 0x7C00)
                        h = u16((h & 0x8000) | 0x7BFF);
                    t.half[i][c] = Ordered(h);
                    float magnitude = float(h & 0x7FFF);
                    if constexpr (Signed)
                        t.x[i][c] = (h & 0x8000 ? -1.0f : 1.0f) * magnitude * 32.0f / 31.0f;
                    else
                        t.x[i][c] = magnitude * 64.0f / 31.0f;
                }
            u32 modes[14], count = 0;
            if (quality == CompressionQuality::Fast)
                modes[count++] = 10;
            else if (quality == CompressionQuality::Normal)
                for (u32 m : {10u, 11u, 12u, 13u, 9u})
                    modes[count++] = m;
            else
                for (u32 m = 0; m < 14; m++)
                    modes[count++] = m;
            u64 best = ~0ull, error;
            u8 trial[16];
            bool found = false;
            for (u32 k = 0; k < count; k++)
            {
                const Bc6Mode& m = kBc6Modes[modes[k]];
                u32 partitions = m.regions == 2 ? (quality == CompressionQuality::Best ? 32u : 8u) : 1u;
                for (u32 p = 0; p < partitions; p++)
                    if (EncodeMode(t, modes[k], p, Signed, error, trial) && error < best)
                    {
                        best = error;
                        found = true;
                        std::memcpy(block, trial, 16);
                    }
            }
            if (!found)
                EncodeMode(t, 10, 0, Signed, error, block);
        }
    }

    void AddBc6Encoders(BlockCodec* codecs, usize count)
    {
        SetEncoder(codecs, count, PixelFormat::BC6H, &EncodeBc6<false>);
        SetEncoder(codecs, count, PixelFormat::BC6HSigned, &EncodeBc6<true>);
    }
}
