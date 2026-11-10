#pragma once

#include <Helix/Core/Basics/Types.h>

// The parts of ASTC shared by the decoder and the encoder: block modes, the integer sequence coding of endpoint and
// weight values, unquantization, partition selection and weight infill.
namespace helix::image::astc
{
    struct BlockMode
    {
        u32 width = 0, height = 0;
        u32 weightLevels = 0;
        bool dualPlane = false;
    };

    // The 11 bit block mode; false for reserved modes.
    inline bool DecodeBlockMode(u32 m, BlockMode& out)
    {
        static constexpr u32 kLevels[2][8] = {{0, 0, 2, 3, 4, 5, 6, 8}, {0, 0, 10, 12, 16, 20, 24, 32}};
        u32 r, a = (m >> 5) & 3, b = (m >> 7) & 3;
        bool high = (m >> 9) & 1, dual = (m >> 10) & 1;
        if (m & 3)
        {
            r = ((m >> 4) & 1) | ((m & 3) << 1);
            switch ((m >> 2) & 3)
            {
            case 0: out.width = b + 4; out.height = a + 2; break;
            case 1: out.width = b + 8; out.height = a + 2; break;
            case 2: out.width = a + 2; out.height = b + 8; break;
            default:
                if ((m >> 8) & 1)
                {
                    out.width = ((m >> 7) & 1) + 2;
                    out.height = a + 2;
                }
                else
                {
                    out.width = a + 2;
                    out.height = ((m >> 7) & 1) + 6;
                }
                break;
            }
        }
        else
        {
            r = ((m >> 4) & 1) | (((m >> 2) & 3) << 1);
            if (((m >> 2) & 3) == 0)
                return false;
            switch ((m >> 7) & 3)
            {
            case 0: out.width = 12; out.height = a + 2; break;
            case 1: out.width = a + 2; out.height = 12; break;
            case 2:
                out.width = a + 6;
                out.height = ((m >> 9) & 3) + 6;
                high = false;
                dual = false;
                break;
            default:
                if (a == 0)
                {
                    out.width = 6;
                    out.height = 10;
                }
                else if (a == 1)
                {
                    out.width = 10;
                    out.height = 6;
                }
                else
                    return false;
                break;
            }
        }
        if (r < 2)
            return false;
        out.weightLevels = kLevels[high ? 1 : 0][r];
        out.dualPlane = dual;
        return true;
    }

    // How a range of levels is coded: plain bits, or a trit or quint plus bits.
    struct Range
    {
        u32 bits, trits, quints;
    };

    inline Range RangeOf(u32 levels)
    {
        for (u32 bits = 0; bits <= 8; bits++)
        {
            if (levels == (1u << bits))
                return {bits, 0, 0};
            if (levels == 3u << bits)
                return {bits, 1, 0};
            if (levels == 5u << bits)
                return {bits, 0, 1};
        }
        return {0, 0, 0};
    }

    inline u32 SequenceBits(u32 levels, u32 count)
    {
        Range r = RangeOf(levels);
        return r.bits * count + (r.trits ? (8 * count + 4) / 5 : 0) + (r.quints ? (7 * count + 2) / 3 : 0);
    }

    // The most levels the endpoint values can have in the bits left; -1 when not even 6 levels fit.
    inline i32 ColorLevels(u32 values, u32 bits)
    {
        static constexpr u32 kLevels[] = {256, 192, 160, 128, 96, 80, 64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6};
        for (u32 levels : kLevels)
            if (SequenceBits(levels, values) <= bits)
                return i32(levels);
        return -1;
    }

    inline void DecodeTrits(u32 t, u32* out)
    {
        u32 c, t4, t3;
        if (((t >> 2) & 7) == 7)
        {
            c = (((t >> 5) & 7) << 2) | (t & 3);
            t4 = 2;
            t3 = 2;
        }
        else
        {
            c = t & 31;
            if (((t >> 5) & 3) == 3)
            {
                t4 = 2;
                t3 = (t >> 7) & 1;
            }
            else
            {
                t4 = (t >> 7) & 1;
                t3 = (t >> 5) & 3;
            }
        }
        u32 t2, t1, t0;
        if ((c & 3) == 3)
        {
            t2 = 2;
            t1 = (c >> 4) & 1;
            t0 = (((c >> 3) & 1) << 1) | (((c >> 2) & 1) & ~((c >> 3) & 1));
        }
        else if (((c >> 2) & 3) == 3)
        {
            t2 = 2;
            t1 = 2;
            t0 = c & 3;
        }
        else
        {
            t2 = (c >> 4) & 1;
            t1 = (c >> 2) & 3;
            t0 = (((c >> 1) & 1) << 1) | ((c & 1) & ~((c >> 1) & 1));
        }
        out[0] = t0;
        out[1] = t1;
        out[2] = t2;
        out[3] = t3;
        out[4] = t4;
    }

    inline void DecodeQuints(u32 q, u32* out)
    {
        u32 q0, q1, q2;
        if (((q >> 1) & 3) == 3 && ((q >> 5) & 3) == 0)
        {
            u32 b0 = q & 1;
            q2 = (b0 << 2) | ((((q >> 4) & 1) & ~b0) << 1) | (((q >> 3) & 1) & ~b0);
            q1 = 4;
            q0 = 4;
        }
        else
        {
            u32 c;
            if (((q >> 1) & 3) == 3)
            {
                q2 = 4;
                c = (((q >> 3) & 3) << 3) | ((~(q >> 5) & 3) << 1) | (q & 1);
            }
            else
            {
                q2 = (q >> 5) & 3;
                c = q & 31;
            }
            if ((c & 7) == 5)
            {
                q1 = 4;
                q0 = (c >> 3) & 3;
            }
            else
            {
                q1 = (c >> 3) & 3;
                q0 = c & 7;
            }
        }
        out[0] = q0;
        out[1] = q1;
        out[2] = q2;
    }

    // `count` values of the range starting at bit `start`; weights are read from the top of the block down.
    inline void DecodeSequence(const u8* block, u32 start, u32 levels, u32 count, u32* out, bool reversed)
    {
        Range r = RangeOf(levels);
        u32 total = SequenceBits(levels, count), at = 0;
        auto read = [&](u32 n) {
            u32 v = 0;
            for (u32 i = 0; i < n; i++, at++)
            {
                if (at >= total)
                    continue;
                u32 position = reversed ? 127 - (start + at) : start + at;
                v |= u32((block[position >> 3] >> (position & 7)) & 1) << i;
            }
            return v;
        };
        if (r.trits)
        {
            static constexpr u32 kSplit[5] = {2, 2, 1, 2, 1};
            for (u32 i = 0; i < count; i += 5)
            {
                u32 m[5], t = 0, shift = 0;
                for (u32 k = 0; k < 5; k++)
                {
                    m[k] = read(r.bits);
                    t |= read(kSplit[k]) << shift;
                    shift += kSplit[k];
                }
                u32 trits[5];
                DecodeTrits(t, trits);
                for (u32 k = 0; k < 5 && i + k < count; k++)
                    out[i + k] = (trits[k] << r.bits) | m[k];
            }
        }
        else if (r.quints)
        {
            static constexpr u32 kSplit[3] = {3, 2, 2};
            for (u32 i = 0; i < count; i += 3)
            {
                u32 m[3], q = 0, shift = 0;
                for (u32 k = 0; k < 3; k++)
                {
                    m[k] = read(r.bits);
                    q |= read(kSplit[k]) << shift;
                    shift += kSplit[k];
                }
                u32 quints[3];
                DecodeQuints(q, quints);
                for (u32 k = 0; k < 3 && i + k < count; k++)
                    out[i + k] = (quints[k] << r.bits) | m[k];
            }
        }
        else
            for (u32 i = 0; i < count; i++)
                out[i] = read(r.bits);
    }

    // An endpoint value of the range to 0 to 255.
    inline u32 UnquantizeColor(u32 v, u32 levels)
    {
        Range r = RangeOf(levels);
        if (!r.trits && !r.quints)
        {
            // Plain bits are repeated to fill 8 bits.
            u32 out = 0, have = 0;
            while (have < 8)
            {
                out = (out << r.bits) | v;
                have += r.bits;
            }
            return out >> (have - 8);
        }
        u32 m = v & ((1u << r.bits) - 1), d = v >> r.bits;
        u32 a = (m & 1) ? 0x1FF : 0;
        u32 b1 = (m >> 1) & 1, c1 = (m >> 2) & 1, d1 = (m >> 3) & 1, e1 = (m >> 4) & 1, f1 = (m >> 5) & 1;
        u32 b = 0, c = 0;
        if (r.trits)
            switch (r.bits)
            {
            case 1: c = 204; break;
            case 2: b = (b1 << 8) | (b1 << 4) | (b1 << 2) | (b1 << 1); c = 93; break;
            case 3: b = (c1 << 8) | (b1 << 7) | (c1 << 3) | (b1 << 2) | (c1 << 1) | b1; c = 44; break;
            case 4: b = (d1 << 8) | (c1 << 7) | (b1 << 6) | (d1 << 2) | (c1 << 1) | b1; c = 22; break;
            case 5: b = (e1 << 8) | (d1 << 7) | (c1 << 6) | (b1 << 5) | (e1 << 1) | d1; c = 11; break;
            case 6: b = (f1 << 8) | (e1 << 7) | (d1 << 6) | (c1 << 5) | (b1 << 4) | f1; c = 5; break;
            default: break;
            }
        else
            switch (r.bits)
            {
            case 1: c = 113; break;
            case 2: b = (b1 << 8) | (b1 << 3) | (b1 << 2); c = 54; break;
            case 3: b = (c1 << 8) | (b1 << 7) | (c1 << 2) | (b1 << 1) | c1; c = 26; break;
            case 4: b = (d1 << 8) | (c1 << 7) | (b1 << 6) | (d1 << 1) | c1; c = 13; break;
            case 5: b = (e1 << 8) | (d1 << 7) | (c1 << 6) | (b1 << 5) | e1; c = 6; break;
            default: break;
            }
        u32 t = d * c + b;
        t ^= a;
        return (a & 0x80) | (t >> 2);
    }

    // A weight value of the range to 0 to 64.
    inline u32 UnquantizeWeight(u32 v, u32 levels)
    {
        Range r = RangeOf(levels);
        u32 t;
        if (!r.trits && !r.quints)
        {
            u32 out = 0, have = 0;
            while (have < 6)
            {
                out = (out << r.bits) | v;
                have += r.bits;
            }
            t = out >> (have - 6);
        }
        else if (r.bits == 0)
        {
            static constexpr u32 kTrits[3] = {0, 32, 63}, kQuints[5] = {0, 16, 32, 47, 63};
            t = r.trits ? kTrits[v < 3 ? v : 2] : kQuints[v < 5 ? v : 4];
        }
        else
        {
            u32 m = v & ((1u << r.bits) - 1), d = v >> r.bits;
            u32 a = (m & 1) ? 0x7F : 0, b1 = (m >> 1) & 1, c1 = (m >> 2) & 1;
            u32 b = 0, c = 0;
            if (r.trits)
                switch (r.bits)
                {
                case 1: c = 50; break;
                case 2: b = (b1 << 6) | (b1 << 2) | b1; c = 23; break;
                default: b = (c1 << 6) | (b1 << 5) | (c1 << 1) | b1; c = 11; break;
                }
            else
                switch (r.bits)
                {
                case 1: c = 28; break;
                default: b = (b1 << 6) | (b1 << 1); c = 13; break;
                }
            t = d * c + b;
            t ^= a;
            t = (a & 0x20) | (t >> 2);
        }
        return t > 32 ? t + 1 : t;
    }

    inline u32 Hash52(u32 p)
    {
        p ^= p >> 15;
        p -= p << 17;
        p += p << 7;
        p += p << 4;
        p ^= p >> 5;
        p += p << 16;
        p ^= p >> 7;
        p ^= p >> 3;
        p ^= p << 6;
        p ^= p >> 17;
        return p;
    }

    inline u32 SelectPartition(u32 seed, u32 x, u32 y, u32 partitions, bool smallBlock)
    {
        if (smallBlock)
        {
            x <<= 1;
            y <<= 1;
        }
        seed += (partitions - 1) * 1024;
        u32 rnum = Hash52(seed);
        u8 s[13];
        s[1] = u8(rnum & 0xF);
        s[2] = u8((rnum >> 4) & 0xF);
        s[3] = u8((rnum >> 8) & 0xF);
        s[4] = u8((rnum >> 12) & 0xF);
        s[5] = u8((rnum >> 16) & 0xF);
        s[6] = u8((rnum >> 20) & 0xF);
        s[7] = u8((rnum >> 24) & 0xF);
        s[8] = u8((rnum >> 28) & 0xF);
        s[9] = u8((rnum >> 18) & 0xF);
        s[10] = u8((rnum >> 22) & 0xF);
        s[11] = u8((rnum >> 26) & 0xF);
        s[12] = u8(((rnum >> 30) | (rnum << 2)) & 0xF);
        for (int i = 1; i <= 12; i++)
            s[i] = u8(s[i] * s[i]);
        u32 sh1, sh2;
        if (seed & 1)
        {
            sh1 = (seed & 2) ? 4 : 5;
            sh2 = partitions == 3 ? 6 : 5;
        }
        else
        {
            sh1 = partitions == 3 ? 6 : 5;
            sh2 = (seed & 2) ? 4 : 5;
        }
        u32 sh3 = (seed & 0x10) ? sh1 : sh2;
        for (int i = 1; i <= 8; i++)
            s[i] = u8(s[i] >> (i & 1 ? sh1 : sh2));
        for (int i = 9; i <= 12; i++)
            s[i] = u8(s[i] >> sh3);
        u32 a = (s[1] * x + s[2] * y + (rnum >> 14)) & 0x3F;
        u32 b = (s[3] * x + s[4] * y + (rnum >> 10)) & 0x3F;
        u32 c = (s[5] * x + s[6] * y + (rnum >> 6)) & 0x3F;
        u32 d = (s[7] * x + s[8] * y + (rnum >> 2)) & 0x3F;
        if (partitions < 4)
            d = 0;
        if (partitions < 3)
            c = 0;
        if (a >= b && a >= c && a >= d)
            return 0;
        if (b >= c && b >= d)
            return 1;
        if (c >= d)
            return 2;
        return 3;
    }

    // The weight of texel (x, y) of a `bw` x `bh` block from a `gw` x `gh` grid of (dual plane interleaved) weights.
    inline u32 InfillWeight(const u32* weights, u32 gw, u32 gh, u32 bw, u32 bh, u32 x, u32 y, u32 plane, u32 planes)
    {
        if (gw == bw && gh == bh)
            return weights[(y * gw + x) * planes + plane];
        u32 ds = (1024 + bw / 2) / (bw - 1), dt = (1024 + bh / 2) / (bh - 1);
        u32 gs = (ds * x * (gw - 1) + 32) >> 6, gt = (dt * y * (gh - 1) + 32) >> 6;
        u32 js = gs >> 4, fs = gs & 0xF, jt = gt >> 4, ft = gt & 0xF;
        u32 w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
        auto at = [&](u32 sx, u32 sy) {
            sx = sx < gw ? sx : gw - 1;
            sy = sy < gh ? sy : gh - 1;
            return weights[(sy * gw + sx) * planes + plane];
        };
        return (at(js, jt) * w00 + at(js + 1, jt) * w01 + at(js, jt + 1) * w10 + at(js + 1, jt + 1) * w11 + 8) >> 4;
    }
}
