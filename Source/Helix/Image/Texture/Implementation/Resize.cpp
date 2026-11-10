#include <Helix/Image/Texture/Color.h>
#include <Helix/Image/Texture/Convert.h>
#include <Helix/Image/Texture/Resize.h>

#include <Helix/Jobs/Jobs.h>

#include <cmath>

namespace helix::image
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        double Mitchell(double x)
        {
            // B = C = 1/3.
            constexpr double b = 1.0 / 3.0, c = 1.0 / 3.0;
            x = std::fabs(x);
            if (x < 1.0)
                return ((12 - 9 * b - 6 * c) * x * x * x + (-18 + 12 * b + 6 * c) * x * x + (6 - 2 * b)) / 6.0;
            if (x < 2.0)
                return ((-b - 6 * c) * x * x * x + (6 * b + 30 * c) * x * x + (-12 * b - 48 * c) * x + (8 * b + 24 * c)) / 6.0;
            return 0.0;
        }

        double Bessel0(double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 32; k++)
            {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
                if (term < sum * 1e-16)
                    break;
            }
            return sum;
        }

        double Kaiser(double x)
        {
            constexpr double kWidth = 3.0, kAlpha = 4.0;
            x = std::fabs(x);
            if (x >= kWidth)
                return 0.0;
            double sinc = x < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            double r = x / kWidth;
            return sinc * Bessel0(kAlpha * std::sqrt(1.0 - r * r)) / Bessel0(kAlpha);
        }

        double Radius(ResampleFilter filter) { return filter == ResampleFilter::Mitchell ? 2.0 : filter == ResampleFilter::Kaiser ? 3.0 : 1.0; }

        double Kernel(ResampleFilter filter, double x)
        {
            switch (filter)
            {
            case ResampleFilter::Mitchell: return Mitchell(x);
            case ResampleFilter::Kaiser: return Kaiser(x);
            default: return std::fabs(x) < 1.0 ? 1.0 - std::fabs(x) : 0.0;
            }
        }

        // For one axis: which source texels each target texel reads and with what weights (normalized to 1).
        struct Weights
        {
            Array<u32> first, count;
            Array<float> weights;
            u32 stride = 0;

            Weights(u32 from, u32 to, ResampleFilter filter)
            {
                double scale = double(from) / double(to);
                double stretch = scale > 1.0 ? scale : 1.0;
                double support = filter == ResampleFilter::Box ? stretch * 0.5 + 1.0 : Radius(filter) * stretch;
                stride = u32(std::ceil(support * 2.0)) + 2;
                weights.Resize(usize(to) * stride);
                for (u32 i = 0; i < to; i++)
                {
                    double center = (double(i) + 0.5) * scale;
                    i64 lo = i64(std::floor(center - support)), hi = i64(std::ceil(center + support));
                    double total = 0.0;
                    u32 n = 0;
                    i64 start = lo;
                    float* w = weights.Data() + usize(i) * stride;
                    for (i64 j = lo; j < hi && n < stride; j++)
                    {
                        double weight;
                        if (filter == ResampleFilter::Box)
                        {
                            // The overlap of [j, j + 1) with the span of the target texel (at least one texel wide).
                            double a = center - stretch * 0.5, b = center + stretch * 0.5;
                            double overlap = (b < double(j + 1) ? b : double(j + 1)) - (a > double(j) ? a : double(j));
                            weight = overlap > 0.0 ? overlap : 0.0;
                        }
                        else
                            weight = Kernel(filter, (double(j) + 0.5 - center) / stretch);
                        if (n == 0 && weight == 0.0)
                        {
                            start = j + 1;
                            continue;
                        }
                        w[n++] = float(weight);
                        total += weight;
                    }
                    while (n > 0 && w[n - 1] == 0.0f)
                        n--;
                    if (total == 0.0)
                    {
                        start = i64(center);
                        w[0] = 1.0f;
                        n = 1;
                        total = 1.0;
                    }
                    for (u32 k = 0; k < n; k++)
                        w[k] = float(w[k] / total);
                    first.Push(u32(start < 0 ? 0 : start));
                    count.Push(n);
                    // Taps outside the image read the edge: fold them onto it.
                    if (start < 0 || start + i64(n) > i64(from))
                    {
                        Array<float> folded;
                        folded.Resize(n);
                        u32 low = u32(start < 0 ? 0 : start);
                        u32 high = low;
                        for (u32 k = 0; k < n; k++)
                        {
                            i64 j = start + k;
                            u32 at = u32(j < 0 ? 0 : j >= i64(from) ? i64(from) - 1 : j);
                            low = at < low ? at : low;
                            high = at > high ? at : high;
                        }
                        for (u32 k = 0; k < n; k++)
                        {
                            i64 j = start + k;
                            u32 at = u32(j < 0 ? 0 : j >= i64(from) ? i64(from) - 1 : j);
                            folded[at - low] += w[k];
                        }
                        n = high - low + 1;
                        for (u32 k = 0; k < n; k++)
                            w[k] = folded[k];
                        first.Back() = low;
                        count.Back() = n;
                    }
                }
            }
        };

        struct Pass
        {
            const float* source;
            float* destination;
            const Weights* weights;
            u32 width, height, newSize;
            bool horizontal;
        };

        // One target row: a horizontal pass reads one source row, a vertical pass a column of rows.
        void Row(const Pass& p, u32 row)
        {
            const Weights& w = *p.weights;
            if (p.horizontal)
            {
                const float* src = p.source + usize(row) * p.width * 4;
                float* dst = p.destination + usize(row) * p.newSize * 4;
                for (u32 x = 0; x < p.newSize; x++)
                {
                    float sum[4] = {};
                    const float* k = w.weights.Data() + usize(x) * w.stride;
                    const float* s = src + usize(w.first[x]) * 4;
                    for (u32 i = 0; i < w.count[x]; i++, s += 4)
                        for (int c = 0; c < 4; c++)
                            sum[c] += s[c] * k[i];
                    for (int c = 0; c < 4; c++)
                        dst[x * 4 + c] = sum[c];
                }
                return;
            }
            float* dst = p.destination + usize(row) * p.width * 4;
            const float* k = w.weights.Data() + usize(row) * w.stride;
            for (u32 x = 0; x < p.width * 4; x++)
                dst[x] = 0.0f;
            for (u32 i = 0; i < w.count[row]; i++)
            {
                const float* src = p.source + usize(w.first[row] + i) * p.width * 4;
                for (u32 x = 0; x < p.width * 4; x++)
                    dst[x] += src[x] * k[i];
            }
        }

        void Run(const Pass& pass, u32 rows)
        {
            if (jobs::IsInitialized() && rows >= 64)
            {
                const Pass* shared = &pass;
                jobs::Complete(jobs::ParallelFor(rows, 16, [shared](u32 row) { Row(*shared, row); }));
            }
            else
                for (u32 row = 0; row < rows; row++)
                    Row(pass, row);
        }
    }

    void FitToMaxSize(u32 width, u32 height, u32 maxSize, u32& outWidth, u32& outHeight)
    {
        outWidth = width;
        outHeight = height;
        u32 larger = width > height ? width : height;
        if (maxSize == 0 || larger <= maxSize)
            return;
        double scale = double(maxSize) / double(larger);
        outWidth = width == larger ? maxSize : u32(std::lround(width * scale));
        outHeight = height == larger ? maxSize : u32(std::lround(height * scale));
        outWidth = outWidth ? outWidth : 1;
        outHeight = outHeight ? outHeight : 1;
    }

    void Resample(const float* source, u32 width, u32 height, float* destination, u32 newWidth, u32 newHeight, ResampleFilter filter)
    {
        // Halving both even sides with the box filter averages 2 x 2 texels: one pass, no buffer in between.
        if (filter == ResampleFilter::Box && newWidth * 2 == width && newHeight * 2 == height)
        {
            struct Half
            {
                const float* source;
                float* destination;
                u32 width, newWidth;
            };
            Half half{source, destination, width, newWidth};
            auto row = [](const Half& h, u32 y) {
                const float* a = h.source + usize(y) * 2 * h.width * 4;
                const float* b = a + usize(h.width) * 4;
                float* d = h.destination + usize(y) * h.newWidth * 4;
                for (u32 x = 0; x < h.newWidth; x++, a += 8, b += 8, d += 4)
                    for (int c = 0; c < 4; c++)
                        d[c] = (a[c] + a[c + 4] + b[c] + b[c + 4]) * 0.25f;
            };
            if (jobs::IsInitialized() && newHeight >= 64)
            {
                const Half* shared = &half;
                jobs::Complete(jobs::ParallelFor(newHeight, 16, [shared, row](u32 y) { row(*shared, y); }));
            }
            else
                for (u32 y = 0; y < newHeight; y++)
                    row(half, y);
            return;
        }
        Array<float> middle;
        middle.Resize(usize(newWidth) * height * 4);
        Weights across(width, newWidth, filter);
        Pass horizontal{source, middle.Data(), &across, width, height, newWidth, true};
        Run(horizontal, height);
        Weights down(height, newHeight, filter);
        Pass vertical{middle.Data(), destination, &down, newWidth, height, newHeight, false};
        Run(vertical, newHeight);
    }

    bool ResizeTexture(const TextureData& source, u32 width, u32 height, ResampleFilter filter, bool srgb, TextureData& out, String& error)
    {
        if (!CanConvert(source.format) || source.depth != 1)
        {
            error = "only uncompressed 2D, cube and array textures can be resized";
            return false;
        }
        if (!out.Allocate(source.format, width, height, 1, source.layers, source.faces, 1))
        {
            error = "the size is empty or too large";
            return false;
        }
        out.colorSpace = source.colorSpace;
        out.dimension = source.dimension;
        Array<float> from, to;
        from.Resize(usize(source.width) * source.height * 4);
        to.Resize(usize(width) * height * 4);
        for (u32 layer = 0; layer < source.layers; layer++)
            for (u32 face = 0; face < source.faces; face++)
            {
                ToFloat(source.format, source.Pixels(layer, face, 0), from.Data(), usize(source.width) * source.height);
                if (srgb)
                    SrgbToLinear(from.Data(), usize(source.width) * source.height);
                Resample(from.Data(), source.width, source.height, to.Data(), width, height, filter);
                if (srgb)
                    LinearToSrgb(to.Data(), usize(width) * height);
                FromFloat(out.format, to.Data(), out.Pixels(layer, face, 0), usize(width) * height);
            }
        return true;
    }
}
