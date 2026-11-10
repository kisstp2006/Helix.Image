#include <Helix/Image/Texture/Convert.h>
#include <Helix/Image/Texture/Implementation/BuiltInBlockCodecs.h>

#include <cmath>
#include <cstring>

// ETC2 and EAC encoders. Color: ETC1 individual and differential blocks (both flips, every table, a search around the
// average colors), and from Normal on the T, H and planar modes of ETC2. The ETC2 modes are told apart by an overflow of
// the differential fields, so their free bits are set to give exactly that overflow. EAC: a search of base, multiplier
// and table.
namespace helix::image::etc
{
    namespace
    {
        constexpr i32 kModifiers[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
        constexpr i32 kDistances[8] = {3, 6, 11, 16, 23, 32, 41, 64};
        constexpr i32 kAlphaModifiers[16][8] = {
            {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12}, {-2, -4, -6, -13, 1, 3, 5, 12},
            {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},  {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},
            {-2, -6, -8, -10, 1, 5, 7, 9},  {-2, -5, -8, -10, 1, 4, 7, 9},   {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
            {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},   {-3, -5, -7, -9, 2, 4, 6, 8},
        };

        i32 Clamp(i32 v, i32 lo = 0, i32 hi = 255) { return v < lo ? lo : v > hi ? hi : v; }

        struct Block
        {
            // Texels in the column order of the format: texel x * 4 + y.
            i32 rgb[16][3];
            bool transparent[16];
        };

        u32 Error(const i32* a, const i32* b)
        {
            u32 e = 0;
            for (int c = 0; c < 3; c++)
            {
                i32 d = a[c] - b[c];
                e += u32(d * d);
            }
            return e;
        }

        void Put(u64& v, u32 high, u32 low, u32 value) { v |= (u64(value) & ((u64(1) << (high - low + 1)) - 1)) << low; }

        void Store(u64 v, u8* out)
        {
            for (int i = 0; i < 8; i++)
                out[i] = u8(v >> (56 - 8 * i));
        }

        void PutIndices(u64& v, const u32* indices)
        {
            for (u32 i = 0; i < 16; i++)
            {
                Put(v, 16 + i, 16 + i, indices[i] >> 1);
                Put(v, i, i, indices[i] & 1);
            }
        }

        // ETC indices: 0 +small, 1 +large, 2 -small, 3 -large.
        i32 Modifier(u32 table, u32 index) { return index == 0 ? kModifiers[table][0] : index == 1 ? kModifiers[table][1] : index == 2 ? -kModifiers[table][0] : -kModifiers[table][1]; }

        // The best table and indices for the texels of one half with a base color; the error.
        u32 Half(const Block& b, const u32* members, const i32* base, bool punchThrough, u32& table, u32* indices)
        {
            u32 best = ~0u;
            for (u32 t = 0; t < 8; t++)
            {
                u32 total = 0, chosen[8];
                for (u32 i = 0; i < 8; i++)
                {
                    u32 texel = members[i];
                    if (b.transparent[texel])
                    {
                        chosen[i] = 2;
                        continue;
                    }
                    u32 bestError = ~0u;
                    for (u32 k = 0; k < 4; k++)
                    {
                        if (punchThrough && k == 2)
                            continue;
                        i32 m = punchThrough && k == 0 ? 0 : Modifier(t, k);
                        i32 c[3] = {Clamp(base[0] + m), Clamp(base[1] + m), Clamp(base[2] + m)};
                        u32 e = Error(b.rgb[texel], c);
                        if (e < bestError)
                        {
                            bestError = e;
                            chosen[i] = k;
                        }
                    }
                    total += bestError;
                }
                if (total < best)
                {
                    best = total;
                    table = t;
                    for (u32 i = 0; i < 8; i++)
                        indices[members[i]] = chosen[i];
                }
            }
            return best;
        }

        struct Result
        {
            u32 error = ~0u;
            u8 block[8];
        };

        void Keep(Result& r, u32 error, u64 v)
        {
            if (error < r.error)
            {
                r.error = error;
                Store(v, r.block);
            }
        }

        u32 Expand4(i32 v) { return u32(v * 17); }
        u32 Expand5(i32 v) { return u32((v << 3) | (v >> 2)); }

        // Individual and differential blocks; `punchThrough` (ETC2 RGBA1) allows only differential ones.
        void Etc1(const Block& b, bool punchThrough, bool opaque, i32 radius, Result& result)
        {
            for (u32 flip = 0; flip < 2; flip++)
            {
                u32 members[2][8], counts[2] = {};
                float average[2][3] = {};
                for (u32 i = 0; i < 16; i++)
                {
                    u32 x = i / 4, y = i % 4, half = flip ? (y >= 2) : (x >= 2);
                    members[half][counts[half]++] = i;
                }
                for (u32 h = 0; h < 2; h++)
                {
                    u32 n = 0;
                    for (u32 i = 0; i < 8; i++)
                        if (!b.transparent[members[h][i]])
                        {
                            n++;
                            for (int c = 0; c < 3; c++)
                                average[h][c] += float(b.rgb[members[h][i]][c]);
                        }
                    for (int c = 0; c < 3; c++)
                        average[h][c] = n ? average[h][c] / float(n) : 0.0f;
                }
                // Individual: 4 bits per channel and half.
                if (!punchThrough)
                {
                    u32 bestError = 0, tables[2], indices[16];
                    i32 bases[2][3];
                    for (u32 h = 0; h < 2; h++)
                    {
                        u32 best = ~0u;
                        i32 q[3];
                        for (int c = 0; c < 3; c++)
                            q[c] = i32(std::lround(average[h][c] / 17.0f));
                        for (i32 dr = -radius; dr <= radius; dr++)
                            for (i32 dg = -radius; dg <= radius; dg++)
                                for (i32 db = -radius; db <= radius; db++)
                                {
                                    i32 c4[3] = {Clamp(q[0] + dr, 0, 15), Clamp(q[1] + dg, 0, 15), Clamp(q[2] + db, 0, 15)};
                                    i32 base[3] = {i32(Expand4(c4[0])), i32(Expand4(c4[1])), i32(Expand4(c4[2]))};
                                    u32 table, chosen[16];
                                    u32 e = Half(b, members[h], base, false, table, chosen);
                                    if (e < best)
                                    {
                                        best = e;
                                        tables[h] = table;
                                        std::memcpy(bases[h], c4, sizeof(c4));
                                        for (u32 i = 0; i < 8; i++)
                                            indices[members[h][i]] = chosen[members[h][i]];
                                    }
                                }
                        bestError += best;
                    }
                    u64 v = 0;
                    Put(v, 63, 60, u32(bases[0][0]));
                    Put(v, 59, 56, u32(bases[1][0]));
                    Put(v, 55, 52, u32(bases[0][1]));
                    Put(v, 51, 48, u32(bases[1][1]));
                    Put(v, 47, 44, u32(bases[0][2]));
                    Put(v, 43, 40, u32(bases[1][2]));
                    Put(v, 39, 37, tables[0]);
                    Put(v, 36, 34, tables[1]);
                    Put(v, 32, 32, flip);
                    PutIndices(v, indices);
                    Keep(result, bestError, v);
                }
                // Differential: 5 bits for the first half, a 3 bit signed difference for the second.
                i32 q[2][3];
                for (u32 h = 0; h < 2; h++)
                    for (int c = 0; c < 3; c++)
                        q[h][c] = Clamp(i32(std::lround(average[h][c] * 31.0f / 255.0f)), 0, 31);
                u32 best[2] = {~0u, ~0u}, tables[2], indices[16];
                i32 bases[2][3];
                for (u32 h = 0; h < 2; h++)
                    for (i32 dr = -radius; dr <= radius; dr++)
                        for (i32 dg = -radius; dg <= radius; dg++)
                            for (i32 db = -radius; db <= radius; db++)
                            {
                                i32 c5[3] = {Clamp(q[h][0] + dr, 0, 31), Clamp(q[h][1] + dg, 0, 31), Clamp(q[h][2] + db, 0, 31)};
                                if (h == 1)
                                {
                                    bool fits = true;
                                    for (int c = 0; c < 3; c++)
                                        fits = fits && c5[c] - bases[0][c] >= -4 && c5[c] - bases[0][c] <= 3;
                                    if (!fits)
                                        continue;
                                }
                                i32 base[3] = {i32(Expand5(c5[0])), i32(Expand5(c5[1])), i32(Expand5(c5[2]))};
                                u32 table, chosen[16];
                                u32 e = Half(b, members[h], base, !opaque, table, chosen);
                                if (e < best[h])
                                {
                                    best[h] = e;
                                    tables[h] = table;
                                    std::memcpy(bases[h], c5, sizeof(c5));
                                    for (u32 i = 0; i < 8; i++)
                                        indices[members[h][i]] = chosen[members[h][i]];
                                }
                            }
                if (best[1] == ~0u)
                {
                    // No second color within reach of the first: use the first one for both halves.
                    std::memcpy(bases[1], bases[0], sizeof(bases[0]));
                    i32 base[3] = {i32(Expand5(bases[1][0])), i32(Expand5(bases[1][1])), i32(Expand5(bases[1][2]))};
                    u32 chosen[16];
                    best[1] = Half(b, members[1], base, !opaque, tables[1], chosen);
                    for (u32 i = 0; i < 8; i++)
                        indices[members[1][i]] = chosen[members[1][i]];
                }
                u64 v = 0;
                Put(v, 63, 59, u32(bases[0][0]));
                Put(v, 58, 56, u32(bases[1][0] - bases[0][0]));
                Put(v, 55, 51, u32(bases[0][1]));
                Put(v, 50, 48, u32(bases[1][1] - bases[0][1]));
                Put(v, 47, 43, u32(bases[0][2]));
                Put(v, 42, 40, u32(bases[1][2] - bases[0][2]));
                Put(v, 39, 37, tables[0]);
                Put(v, 36, 34, tables[1]);
                Put(v, 33, 33, opaque ? 1 : 0);
                Put(v, 32, 32, flip);
                PutIndices(v, indices);
                Keep(result, best[0] + best[1], v);
            }
        }

        // Sets the free bits `bits` (most significant first) so the 5 bit field at `field` plus the 3 bit difference
        // below it overflows (`overflow`) or stays in range.
        void SetOverflow(u64& v, u32 field, const u32* freeBits, u32 count, bool overflow)
        {
            for (u32 combo = 0; combo < (1u << count); combo++)
            {
                u64 t = v;
                for (u32 i = 0; i < count; i++)
                {
                    t &= ~(u64(1) << freeBits[i]);
                    t |= u64((combo >> i) & 1) << freeBits[i];
                }
                i32 base = i32((t >> (field - 4)) & 31), delta = i32((t >> (field - 7)) & 7);
                delta = delta >= 4 ? delta - 8 : delta;
                bool overflows = base + delta < 0 || base + delta > 31;
                if (overflows == overflow)
                {
                    v = t;
                    return;
                }
            }
        }

        // Two groups of texels by two means along the principal axis.
        void Split(const Block& b, float centers[2][3], u32* group)
        {
            float mean[3] = {}, lo[3] = {255, 255, 255}, hi[3] = {};
            for (u32 i = 0; i < 16; i++)
                for (int c = 0; c < 3; c++)
                {
                    mean[c] += float(b.rgb[i][c]) / 16.0f;
                    lo[c] = std::fmin(lo[c], float(b.rgb[i][c]));
                    hi[c] = std::fmax(hi[c], float(b.rgb[i][c]));
                }
            for (int c = 0; c < 3; c++)
            {
                centers[0][c] = lo[c];
                centers[1][c] = hi[c];
            }
            for (int iteration = 0; iteration < 4; iteration++)
            {
                float sum[2][3] = {};
                u32 counts[2] = {};
                for (u32 i = 0; i < 16; i++)
                {
                    float d[2] = {};
                    for (u32 k = 0; k < 2; k++)
                        for (int c = 0; c < 3; c++)
                            d[k] += (float(b.rgb[i][c]) - centers[k][c]) * (float(b.rgb[i][c]) - centers[k][c]);
                    group[i] = d[1] < d[0] ? 1 : 0;
                    counts[group[i]]++;
                    for (int c = 0; c < 3; c++)
                        sum[group[i]][c] += float(b.rgb[i][c]);
                }
                for (u32 k = 0; k < 2; k++)
                    for (int c = 0; c < 3; c++)
                        centers[k][c] = counts[k] ? sum[k][c] / float(counts[k]) : mean[c];
            }
        }

        // The error and indices of a 4 color palette.
        u32 Paint(const Block& b, const i32 paint[4][3], bool punchThrough, u32* indices)
        {
            u32 total = 0;
            for (u32 i = 0; i < 16; i++)
            {
                if (b.transparent[i])
                {
                    indices[i] = 2;
                    continue;
                }
                u32 best = ~0u;
                for (u32 k = 0; k < 4; k++)
                {
                    if (punchThrough && k == 2)
                        continue;
                    u32 e = Error(b.rgb[i], paint[k]);
                    if (e < best)
                    {
                        best = e;
                        indices[i] = k;
                    }
                }
                total += best;
            }
            return total;
        }

        void ModeT(const Block& b, bool opaque, Result& result)
        {
            float centers[2][3];
            u32 group[16];
            Split(b, centers, group);
            for (u32 single = 0; single < 2; single++)
            {
                i32 c1[3], c2[3];
                for (int c = 0; c < 3; c++)
                {
                    c1[c] = Clamp(i32(std::lround(centers[single][c] / 17.0f)), 0, 15);
                    c2[c] = Clamp(i32(std::lround(centers[1 - single][c] / 17.0f)), 0, 15);
                }
                for (u32 d = 0; d < 8; d++)
                {
                    i32 paint[4][3];
                    for (int c = 0; c < 3; c++)
                    {
                        paint[0][c] = i32(Expand4(c1[c]));
                        paint[1][c] = Clamp(i32(Expand4(c2[c])) + kDistances[d]);
                        paint[2][c] = i32(Expand4(c2[c]));
                        paint[3][c] = Clamp(i32(Expand4(c2[c])) - kDistances[d]);
                    }
                    u32 indices[16];
                    u32 e = Paint(b, paint, !opaque, indices);
                    u64 v = 0;
                    Put(v, 60, 59, u32(c1[0]) >> 2);
                    Put(v, 57, 56, u32(c1[0]) & 3);
                    Put(v, 55, 52, u32(c1[1]));
                    Put(v, 51, 48, u32(c1[2]));
                    Put(v, 47, 44, u32(c2[0]));
                    Put(v, 43, 40, u32(c2[1]));
                    Put(v, 39, 36, u32(c2[2]));
                    Put(v, 35, 34, d >> 1);
                    Put(v, 33, 33, opaque ? 1 : 0);
                    Put(v, 32, 32, d & 1);
                    PutIndices(v, indices);
                    static constexpr u32 kFree[4] = {63, 62, 61, 58};
                    SetOverflow(v, 63, kFree, 4, true);
                    Keep(result, e, v);
                }
            }
        }

        void ModeH(const Block& b, bool opaque, Result& result)
        {
            float centers[2][3];
            u32 group[16];
            Split(b, centers, group);
            i32 c[2][3];
            for (u32 k = 0; k < 2; k++)
                for (int ch = 0; ch < 3; ch++)
                    c[k][ch] = Clamp(i32(std::lround(centers[k][ch] / 17.0f)), 0, 15);
            for (u32 d = 0; d < 8; d++)
            {
                // The lowest bit of the distance is the order of the two colors.
                i32 c1[3], c2[3];
                u32 value0 = u32((c[0][0] << 8) | (c[0][1] << 4) | c[0][2]), value1 = u32((c[1][0] << 8) | (c[1][1] << 4) | c[1][2]);
                bool firstLarger = value0 >= value1;
                bool swap = (d & 1) ? !firstLarger : firstLarger;
                std::memcpy(c1, c[swap ? 1 : 0], sizeof(c1));
                std::memcpy(c2, c[swap ? 0 : 1], sizeof(c2));
                u32 v1 = u32((c1[0] << 8) | (c1[1] << 4) | c1[2]), v2 = u32((c2[0] << 8) | (c2[1] << 4) | c2[2]);
                if (((v1 >= v2) ? 1u : 0u) != (d & 1))
                    continue;
                i32 paint[4][3];
                for (int ch = 0; ch < 3; ch++)
                {
                    paint[0][ch] = Clamp(i32(Expand4(c1[ch])) + kDistances[d]);
                    paint[1][ch] = Clamp(i32(Expand4(c1[ch])) - kDistances[d]);
                    paint[2][ch] = Clamp(i32(Expand4(c2[ch])) + kDistances[d]);
                    paint[3][ch] = Clamp(i32(Expand4(c2[ch])) - kDistances[d]);
                }
                u32 indices[16];
                u32 e = Paint(b, paint, !opaque, indices);
                u64 v = 0;
                Put(v, 62, 59, u32(c1[0]));
                Put(v, 58, 56, u32(c1[1]) >> 1);
                Put(v, 52, 52, u32(c1[1]) & 1);
                Put(v, 51, 51, u32(c1[2]) >> 3);
                Put(v, 49, 47, u32(c1[2]) & 7);
                Put(v, 46, 43, u32(c2[0]));
                Put(v, 42, 39, u32(c2[1]));
                Put(v, 38, 35, u32(c2[2]));
                Put(v, 34, 34, d >> 2);
                Put(v, 33, 33, opaque ? 1 : 0);
                Put(v, 32, 32, (d >> 1) & 1);
                PutIndices(v, indices);
                static constexpr u32 kRed[1] = {63}, kGreen[4] = {55, 54, 53, 50};
                SetOverflow(v, 63, kRed, 1, false);
                SetOverflow(v, 55, kGreen, 4, true);
                Keep(result, e, v);
            }
        }

        // Planar: a least squares plane through the texels, as colors at the origin, the right and the bottom edge.
        void ModePlanar(const Block& b, Result& result)
        {
            i32 o[3], h[3], w[3];
            for (int c = 0; c < 3; c++)
            {
                // c(x, y) = a + bx * x + by * y over x, y in 0..3.
                float sum = 0, sx = 0, sy = 0;
                for (u32 i = 0; i < 16; i++)
                {
                    float v = float(b.rgb[i][c]), x = float(i / 4), y = float(i % 4);
                    sum += v;
                    sx += v * (x - 1.5f);
                    sy += v * (y - 1.5f);
                }
                float bx = sx / 20.0f, by = sy / 20.0f, a = sum / 16.0f - 1.5f * bx - 1.5f * by;
                float origin = a, right = a + 4.0f * bx, bottom = a + 4.0f * by;
                u32 bits = c == 1 ? 7 : 6, levels = (1u << bits) - 1;
                o[c] = Clamp(i32(std::lround(origin * float(levels) / 255.0f)), 0, i32(levels));
                h[c] = Clamp(i32(std::lround(right * float(levels) / 255.0f)), 0, i32(levels));
                w[c] = Clamp(i32(std::lround(bottom * float(levels) / 255.0f)), 0, i32(levels));
            }
            auto e6 = [](i32 x) { return (x << 2) | (x >> 4); };
            auto e7 = [](i32 x) { return (x << 1) | (x >> 6); };
            u32 error = 0;
            for (u32 i = 0; i < 16; i++)
            {
                i32 x = i32(i / 4), y = i32(i % 4), c[3];
                for (int k = 0; k < 3; k++)
                {
                    i32 O = k == 1 ? e7(o[k]) : e6(o[k]), H = k == 1 ? e7(h[k]) : e6(h[k]), V = k == 1 ? e7(w[k]) : e6(w[k]);
                    c[k] = Clamp((x * (H - O) + y * (V - O) + 4 * O + 2) >> 2);
                }
                error += Error(b.rgb[i], c);
            }
            u64 v = 0;
            Put(v, 62, 57, u32(o[0]));
            Put(v, 56, 56, u32(o[1]) >> 6);
            Put(v, 54, 49, u32(o[1]) & 63);
            Put(v, 48, 48, u32(o[2]) >> 5);
            Put(v, 44, 43, (u32(o[2]) >> 3) & 3);
            Put(v, 41, 39, u32(o[2]) & 7);
            Put(v, 38, 34, u32(h[0]) >> 1);
            Put(v, 33, 33, 1);
            Put(v, 32, 32, u32(h[0]) & 1);
            Put(v, 31, 25, u32(h[1]));
            Put(v, 24, 19, u32(h[2]));
            Put(v, 18, 13, u32(w[0]));
            Put(v, 12, 6, u32(w[1]));
            Put(v, 5, 0, u32(w[2]));
            static constexpr u32 kRed[1] = {63}, kGreen[1] = {55}, kBlue[4] = {47, 46, 45, 42};
            SetOverflow(v, 63, kRed, 1, false);
            SetOverflow(v, 55, kGreen, 1, false);
            SetOverflow(v, 47, kBlue, 4, true);
            Keep(result, error, v);
        }

        void EncodeColor(const u8* rgba, bool punchThrough, CompressionQuality quality, u8* out)
        {
            Block b;
            bool anyTransparent = false;
            for (u32 i = 0; i < 16; i++)
            {
                u32 x = i / 4, y = i % 4;
                const u8* t = rgba + (y * 4 + x) * 4;
                for (int c = 0; c < 3; c++)
                    b.rgb[i][c] = t[c];
                b.transparent[i] = punchThrough && t[3] < 128;
                anyTransparent = anyTransparent || b.transparent[i];
            }
            bool opaque = !anyTransparent;
            Result result;
            i32 radius = quality == CompressionQuality::Fast ? 0 : quality == CompressionQuality::Normal ? 1 : 2;
            Etc1(b, punchThrough, opaque, radius, result);
            if (quality != CompressionQuality::Fast)
            {
                ModeT(b, opaque, result);
                ModeH(b, opaque, result);
                if (opaque)
                    ModePlanar(b, result);
            }
            std::memcpy(out, result.block, 8);
        }

        // EAC: `values` in the domain of the variant (8 bit alpha, 11 bit unsigned, 11 bit signed).
        enum class Eac
        {
            Alpha,
            Unsigned,
            Signed,
        };

        i32 EacValue(Eac kind, i32 base, i32 multiplier, i32 modifier)
        {
            if (kind == Eac::Alpha)
                return Clamp(base + modifier * multiplier);
            if (kind == Eac::Unsigned)
                return Clamp(multiplier ? base * 8 + 4 + modifier * multiplier * 8 : base * 8 + 4 + modifier, 0, 2047);
            return Clamp(multiplier ? base * 8 + modifier * multiplier * 8 : base * 8 + modifier, -1023, 1023);
        }

        void EncodeEac(const i32* values, Eac kind, CompressionQuality quality, u8* out)
        {
            i32 lo = values[0], hi = values[0];
            for (u32 i = 0; i < 16; i++)
            {
                lo = values[i] < lo ? values[i] : lo;
                hi = values[i] > hi ? values[i] : hi;
            }
            i32 scale = kind == Eac::Alpha ? 1 : 8;
            i32 center = ((lo + hi) / 2 - (kind == Eac::Unsigned ? 4 : 0)) / scale;
            i32 baseLo = kind == Eac::Signed ? -127 : 0, baseHi = kind == Eac::Signed ? 127 : 255;
            i32 baseRadius = quality == CompressionQuality::Fast ? 0 : quality == CompressionQuality::Normal ? 2 : 6;
            u64 best = ~0ull;
            u64 bestBits = 0;
            for (u32 t = 0; t < 16; t++)
            {
                const i32* m = kAlphaModifiers[t];
                // The multiplier that spreads the table over the value range, and its neighbours.
                i32 span = m[7] - m[3];
                i32 guess = (hi - lo + span * scale / 2) / (span * scale);
                for (i32 dm = -1; dm <= 1; dm++)
                    for (i32 db = -baseRadius; db <= baseRadius; db++)
                    {
                        i32 multiplier = Clamp(guess + dm, kind == Eac::Alpha ? 1 : 0, 15), base = Clamp(center + db, baseLo, baseHi);
                        u64 error = 0, bits = 0;
                        for (u32 i = 0; i < 16; i++)
                        {
                            // Texel i of the block is column x = i / 4, row y = i % 4; values come in rows.
                            u32 x = i / 4, y = i % 4;
                            i32 target = values[y * 4 + x];
                            u32 bestIndex = 0;
                            i64 bestError = -1;
                            for (u32 k = 0; k < 8; k++)
                            {
                                i64 d = EacValue(kind, base, multiplier, m[k]) - target;
                                if (bestError < 0 || d * d < bestError)
                                {
                                    bestError = d * d;
                                    bestIndex = k;
                                }
                            }
                            error += u64(bestError);
                            bits |= u64(bestIndex) << (45 - 3 * i);
                        }
                        if (error < best)
                        {
                            best = error;
                            bestBits = (u64(u8(base)) << 56) | (u64(multiplier) << 52) | (u64(t) << 48) | bits;
                        }
                    }
            }
            Store(bestBits, out);
        }

        void EncodeRgb(const u8* texels, u8* block, CompressionQuality quality) { EncodeColor(texels, false, quality, block); }
        void EncodeRgba1(const u8* texels, u8* block, CompressionQuality quality) { EncodeColor(texels, true, quality, block); }

        void EncodeRgba(const u8* texels, u8* block, CompressionQuality quality)
        {
            i32 alpha[16];
            for (u32 i = 0; i < 16; i++)
                alpha[i] = texels[i * 4 + 3];
            EncodeEac(alpha, Eac::Alpha, quality, block);
            EncodeColor(texels, false, quality, block + 8);
        }

        template <u32 Channels, bool Signed>
        void EncodeR11(const u8* texels, u8* block, CompressionQuality quality)
        {
            for (u32 c = 0; c < Channels; c++)
            {
                i32 values[16];
                for (u32 i = 0; i < 16; i++)
                {
                    u16 v;
                    std::memcpy(&v, texels + (i * Channels + c) * 2, 2);
                    if constexpr (Signed)
                    {
                        float f = HalfToFloat(v);
                        f = f < -1.0f ? -1.0f : f > 1.0f ? 1.0f : f;
                        values[i] = i32(std::lround(f * 1023.0f));
                    }
                    else
                        values[i] = i32((u32(v) * 2047 + 32767) / 65535);
                }
                EncodeEac(values, Signed ? Eac::Signed : Eac::Unsigned, quality, block + 8 * c);
            }
        }
    }

    void AddEncoders(BlockCodec* codecs, usize count)
    {
        SetEncoder(codecs, count, PixelFormat::ETC2RGB, &EncodeRgb);
        SetEncoder(codecs, count, PixelFormat::ETC2RGBA1, &EncodeRgba1);
        SetEncoder(codecs, count, PixelFormat::ETC2RGBA, &EncodeRgba);
        SetEncoder(codecs, count, PixelFormat::EACR11, &EncodeR11<1, false>);
        SetEncoder(codecs, count, PixelFormat::EACR11Snorm, &EncodeR11<1, true>);
        SetEncoder(codecs, count, PixelFormat::EACRG11, &EncodeR11<2, false>);
        SetEncoder(codecs, count, PixelFormat::EACRG11Snorm, &EncodeR11<2, true>);
    }
}
