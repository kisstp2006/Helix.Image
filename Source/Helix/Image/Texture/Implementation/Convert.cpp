#include <Helix/Image/Texture/Convert.h>

#include <cmath>
#include <cstring>

namespace helix::image
{
    namespace
    {
        using F = PixelFormat;

        // NaN becomes the lowest value.
        float Clamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }
        float ClampSigned(float v) { return v > -1.0f ? (v < 1.0f ? v : 1.0f) : -1.0f; }
        u32 Unorm(float v, u32 maximum) { return u32(Clamp01(v) * float(maximum) + 0.5f); }
        i32 Snorm(float v, i32 maximum) { return i32(std::lround(ClampSigned(v) * float(maximum))); }

        u16 Load16(const u8* p)
        {
            u16 v;
            std::memcpy(&v, p, 2);
            return v;
        }
        u32 Load32(const u8* p)
        {
            u32 v;
            std::memcpy(&v, p, 4);
            return v;
        }
        void Store16(u8* p, u32 v)
        {
            u16 s = u16(v);
            std::memcpy(p, &s, 2);
        }
        void Store32(u8* p, u32 v) { std::memcpy(p, &v, 4); }

        // Unsigned small floats of the packed formats: 5 exponent bits and `mantissa` bits, no sign.
        float SmallFloat(u32 bits, u32 mantissa)
        {
            u32 e = bits >> mantissa, m = bits & ((1u << mantissa) - 1);
            if (e == 31)
                return m ? NAN : INFINITY;
            if (e == 0)
                return std::ldexp(float(m), -14 - i32(mantissa));
            return std::ldexp(1.0f + float(m) / float(1u << mantissa), i32(e) - 15);
        }

        u32 ToSmallFloat(float v, u32 mantissa)
        {
            if (!(v > 0.0f))
                return 0;
            u32 h = FloatToHalf(v);
            if (h >= 0x7C00)
                return 31u << mantissa;
            u32 drop = 10 - mantissa;
            u32 rounded = (h + (1u << (drop - 1))) >> drop;
            u32 largest = (30u << mantissa) | ((1u << mantissa) - 1);
            return rounded > largest ? largest : rounded;
        }

        u32 ToShared(const float* rgb)
        {
            constexpr i32 kBias = 15, kBits = 9;
            const float largest = float(511.0 / 512.0 * 65536.0);
            float c[3];
            for (int i = 0; i < 3; i++)
                c[i] = rgb[i] > 0.0f ? (rgb[i] < largest ? rgb[i] : largest) : 0.0f;
            float m = c[0] > c[1] ? c[0] : c[1];
            m = m > c[2] ? m : c[2];
            i32 exponent = (m > 0.0f ? i32(std::floor(std::log2(m))) : -kBias - 1);
            exponent = (exponent < -kBias - 1 ? -kBias - 1 : exponent) + 1 + kBias;
            float denominator = std::ldexp(1.0f, exponent - kBias - kBits);
            if (u32(std::floor(m / denominator + 0.5f)) == 512)
            {
                denominator *= 2.0f;
                exponent++;
            }
            u32 out = u32(exponent) << 27;
            for (int i = 0; i < 3; i++)
                out |= (u32(std::floor(c[i] / denominator + 0.5f)) & 511) << (9 * i);
            return out;
        }

        void Unpack(F f, const u8* p, float* o)
        {
            o[0] = o[1] = o[2] = 0.0f;
            o[3] = 1.0f;
            switch (f)
            {
            case F::R8: case F::RG8: case F::RGB8: case F::RGBA8:
                for (u32 c = 0; c < FormatInfo(f).channels; c++)
                    o[c] = float(p[c]) / 255.0f;
                break;
            case F::BGRA8:
                o[0] = float(p[2]) / 255.0f;
                o[1] = float(p[1]) / 255.0f;
                o[2] = float(p[0]) / 255.0f;
                o[3] = float(p[3]) / 255.0f;
                break;
            case F::R8Snorm: case F::RG8Snorm: case F::RGBA8Snorm:
                for (u32 c = 0; c < FormatInfo(f).channels; c++)
                {
                    float v = float(i8(p[c])) / 127.0f;
                    o[c] = v < -1.0f ? -1.0f : v;
                }
                break;
            case F::R16: case F::RG16: case F::RGB16: case F::RGBA16:
                for (u32 c = 0; c < FormatInfo(f).channels; c++)
                    o[c] = float(Load16(p + 2 * c)) / 65535.0f;
                break;
            case F::R16Float: case F::RG16Float: case F::RGB16Float: case F::RGBA16Float:
                for (u32 c = 0; c < FormatInfo(f).channels; c++)
                    o[c] = HalfToFloat(Load16(p + 2 * c));
                break;
            case F::R32Float: case F::RG32Float: case F::RGB32Float: case F::RGBA32Float:
                std::memcpy(o, p, FormatInfo(f).channels * 4u);
                break;
            case F::R32Uint: o[0] = float(Load32(p)); break;
            case F::RGB10A2:
            {
                u32 v = Load32(p);
                o[0] = float(v & 1023) / 1023.0f;
                o[1] = float((v >> 10) & 1023) / 1023.0f;
                o[2] = float((v >> 20) & 1023) / 1023.0f;
                o[3] = float(v >> 30) / 3.0f;
                break;
            }
            case F::RG11B10Float:
            {
                u32 v = Load32(p);
                o[0] = SmallFloat(v & 0x7FF, 6);
                o[1] = SmallFloat((v >> 11) & 0x7FF, 6);
                o[2] = SmallFloat(v >> 22, 5);
                break;
            }
            case F::RGB9E5Float:
            {
                u32 v = Load32(p);
                float scale = std::ldexp(1.0f, i32(v >> 27) - 15 - 9);
                for (int c = 0; c < 3; c++)
                    o[c] = float((v >> (9 * c)) & 511) * scale;
                break;
            }
            case F::B5G6R5:
            {
                u32 v = Load16(p);
                o[0] = float(v >> 11) / 31.0f;
                o[1] = float((v >> 5) & 63) / 63.0f;
                o[2] = float(v & 31) / 31.0f;
                break;
            }
            case F::B5G5R5A1:
            {
                u32 v = Load16(p);
                o[0] = float((v >> 10) & 31) / 31.0f;
                o[1] = float((v >> 5) & 31) / 31.0f;
                o[2] = float(v & 31) / 31.0f;
                o[3] = float(v >> 15);
                break;
            }
            case F::B4G4R4A4:
            {
                u32 v = Load16(p);
                o[0] = float((v >> 8) & 15) / 15.0f;
                o[1] = float((v >> 4) & 15) / 15.0f;
                o[2] = float(v & 15) / 15.0f;
                o[3] = float(v >> 12) / 15.0f;
                break;
            }
            default: break;
            }
        }

        void Pack(F f, const float* o, u8* p)
        {
            switch (f)
            {
            case F::R8: case F::RG8: case F::RGB8: case F::RGBA8:
                for (u32 c = 0; c < FormatInfo(f).channels; c++)
                    p[c] = u8(Unorm(o[c], 255));
                break;
            case F::BGRA8:
                p[0] = u8(Unorm(o[2], 255));
                p[1] = u8(Unorm(o[1], 255));
                p[2] = u8(Unorm(o[0], 255));
                p[3] = u8(Unorm(o[3], 255));
                break;
            case F::R8Snorm: case F::RG8Snorm: case F::RGBA8Snorm:
                for (u32 c = 0; c < FormatInfo(f).channels; c++)
                    p[c] = u8(i8(Snorm(o[c], 127)));
                break;
            case F::R16: case F::RG16: case F::RGB16: case F::RGBA16:
                for (u32 c = 0; c < FormatInfo(f).channels; c++)
                    Store16(p + 2 * c, Unorm(o[c], 65535));
                break;
            case F::R16Float: case F::RG16Float: case F::RGB16Float: case F::RGBA16Float:
                for (u32 c = 0; c < FormatInfo(f).channels; c++)
                    Store16(p + 2 * c, FloatToHalf(o[c]));
                break;
            case F::R32Float: case F::RG32Float: case F::RGB32Float: case F::RGBA32Float:
                std::memcpy(p, o, FormatInfo(f).channels * 4u);
                break;
            case F::R32Uint: Store32(p, o[0] > 0.0f ? (o[0] < 4294967040.0f ? u32(o[0] + 0.5f) : 0xFFFFFFFFu) : 0u); break;
            case F::RGB10A2: Store32(p, Unorm(o[0], 1023) | Unorm(o[1], 1023) << 10 | Unorm(o[2], 1023) << 20 | Unorm(o[3], 3) << 30); break;
            case F::RG11B10Float: Store32(p, ToSmallFloat(o[0], 6) | ToSmallFloat(o[1], 6) << 11 | ToSmallFloat(o[2], 5) << 22); break;
            case F::RGB9E5Float: Store32(p, ToShared(o)); break;
            case F::B5G6R5: Store16(p, Unorm(o[0], 31) << 11 | Unorm(o[1], 63) << 5 | Unorm(o[2], 31)); break;
            case F::B5G5R5A1: Store16(p, Unorm(o[0], 31) << 10 | Unorm(o[1], 31) << 5 | Unorm(o[2], 31) | Unorm(o[3], 1) << 15); break;
            case F::B4G4R4A4: Store16(p, Unorm(o[0], 15) << 8 | Unorm(o[1], 15) << 4 | Unorm(o[2], 15) | Unorm(o[3], 15) << 12); break;
            default: break;
            }
        }
    }

    u16 FloatToHalf(float value)
    {
        u32 bits;
        std::memcpy(&bits, &value, 4);
        u32 sign = (bits >> 16) & 0x8000;
        u32 rest = bits & 0x7FFFFFFF;
        if (rest >= 0x7F800000)
            return u16(sign | 0x7C00 | (rest > 0x7F800000 ? 0x200 : 0));
        if (rest >= 0x477FF000)
            return u16(sign | 0x7C00);
        if (rest < 0x38800000)
        {
            // Subnormal halves: shift the mantissa with its hidden bit, rounding to nearest even.
            if (rest < 0x33000000)
                return u16(sign);
            u32 exponent = rest >> 23, mantissa = (rest & 0x7FFFFF) | 0x800000;
            u32 shift = 126 - exponent;
            u32 half = mantissa >> shift, remainder = mantissa & ((1u << shift) - 1), midpoint = 1u << (shift - 1);
            if (remainder > midpoint || (remainder == midpoint && (half & 1)))
                half++;
            return u16(sign | half);
        }
        u32 half = ((rest >> 13) - (112u << 10));
        u32 remainder = rest & 0x1FFF;
        if (remainder > 0x1000 || (remainder == 0x1000 && (half & 1)))
            half++;
        return u16(sign | half);
    }

    float HalfToFloat(u16 value)
    {
        u32 sign = u32(value & 0x8000) << 16, exponent = (value >> 10) & 31, mantissa = value & 1023, bits;
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

    bool CanConvert(PixelFormat format) { return format != PixelFormat::Unknown && format < PixelFormat::Count && !IsBlockFormat(format); }

    void ToFloat(PixelFormat format, const u8* source, float* rgba, usize count)
    {
        u32 stride = FormatInfo(format).bytesPerBlock;
        for (usize i = 0; i < count; i++)
            Unpack(format, source + i * stride, rgba + i * 4);
    }

    void FromFloat(PixelFormat format, const float* rgba, u8* destination, usize count)
    {
        u32 stride = FormatInfo(format).bytesPerBlock;
        for (usize i = 0; i < count; i++)
            Pack(format, rgba + i * 4, destination + i * stride);
    }

    bool ConvertPixels(PixelFormat from, const u8* source, PixelFormat to, u8* destination, usize count)
    {
        if (!CanConvert(from) || !CanConvert(to))
            return false;
        if (from == to)
        {
            std::memcpy(destination, source, count * FormatInfo(from).bytesPerBlock);
            return true;
        }
        constexpr usize kChunk = 256;
        float rgba[kChunk * 4];
        u32 inStride = FormatInfo(from).bytesPerBlock, outStride = FormatInfo(to).bytesPerBlock;
        for (usize at = 0; at < count; at += kChunk)
        {
            usize n = count - at < kChunk ? count - at : kChunk;
            ToFloat(from, source + at * inStride, rgba, n);
            FromFloat(to, rgba, destination + at * outStride, n);
        }
        return true;
    }
}
