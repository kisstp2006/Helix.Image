#include <Helix/Image/Texture/Color.h>

#include <cmath>

namespace helix::image
{
    float SrgbToLinear(float value)
    {
        if (!(value > 0.0f))
            return 0.0f;
        return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    }

    float LinearToSrgb(float value)
    {
        if (!(value > 0.0f))
            return 0.0f;
        return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    }

    float SrgbByteToLinear(u8 value)
    {
        struct Table
        {
            float values[256];
            Table()
            {
                for (int i = 0; i < 256; i++)
                    values[i] = SrgbToLinear(float(i) / 255.0f);
            }
        };
        static const Table table;
        return table.values[value];
    }

    u8 LinearToSrgbByte(float value)
    {
        struct Table
        {
            // bounds[b]: the smallest linear value that rounds to byte b + 1.
            float bounds[255];
            Table()
            {
                for (int b = 0; b < 255; b++)
                {
                    // The exact rounding point, then nudged so it agrees with the float formula at its boundary.
                    float t = SrgbToLinear((float(b) + 0.5f) / 255.0f);
                    while (t > 0.0f && LinearToSrgb(std::nextafter(t, 0.0f)) * 255.0f + 0.5f >= float(b + 1))
                        t = std::nextafter(t, 0.0f);
                    while (LinearToSrgb(t) * 255.0f + 0.5f < float(b + 1))
                        t = std::nextafter(t, 2.0f);
                    bounds[b] = t;
                }
            }
        };
        static const Table table;
        if (!(value > 0.0f))
            return 0;
        u32 lo = 0, hi = 255;
        while (lo < hi)
        {
            u32 mid = (lo + hi) / 2;
            if (value >= table.bounds[mid])
                lo = mid + 1;
            else
                hi = mid;
        }
        return u8(lo);
    }

    void SrgbToLinear(float* rgba, usize count)
    {
        for (usize i = 0; i < count; i++)
            for (int c = 0; c < 3; c++)
                rgba[i * 4 + c] = SrgbToLinear(rgba[i * 4 + c]);
    }

    void LinearToSrgb(float* rgba, usize count)
    {
        for (usize i = 0; i < count; i++)
            for (int c = 0; c < 3; c++)
                rgba[i * 4 + c] = LinearToSrgb(rgba[i * 4 + c]);
    }
}
