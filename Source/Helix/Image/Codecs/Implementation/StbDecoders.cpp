#include <Helix/Image/Codecs/Implementation/BuiltInDecoders.h>
#include <Helix/Image/Codecs/Implementation/StbAllocator.h>

#include <Helix/Core/Basics/Assert.h>
#include <Helix/Core/Memory/Allocator.h>
#include <Helix/Core/Strings/Format.h>

#include <cstring>

// JPEG, BMP, TGA, PSD and GIF through the public domain stb_image (ThirdParty/stb_image, see THIRD-PARTY.txt).
namespace helix::image::stb
{
    // stb frees without a size, so every block keeps its size in front of it.
    constexpr usize kHeader = 16;

    // Damaged headers make stb ask for huge blocks and expect a null pointer when there is no memory; the heap would stop
    // the process instead, so blocks larger than any texture we take (smaller in 32 bit address spaces) get null.
    constexpr usize kLargest = sizeof(void*) == 4 ? usize(1) << 28 : usize(kMaxTextureBytes);

    void* Allocate(usize size)
    {
        if (size > kLargest)
            return nullptr;
        u8* block = static_cast<u8*>(HeapAllocator().Allocate(size + kHeader, kHeader));
        if (!block)
            return nullptr;
        std::memcpy(block, &size, sizeof(size));
        return block + kHeader;
    }

    void Free(void* memory)
    {
        if (!memory)
            return;
        u8* block = static_cast<u8*>(memory) - kHeader;
        usize size;
        std::memcpy(&size, block, sizeof(size));
        HeapAllocator().Free(block, size + kHeader, kHeader);
    }

    // As C realloc: when the new block cannot be had, the old one stays (stb frees it on its out of memory path).
    void* Reallocate(void* memory, usize size)
    {
        void* moved = Allocate(size);
        if (!moved)
            return nullptr;
        if (memory)
        {
            usize old;
            std::memcpy(&old, static_cast<u8*>(memory) - kHeader, sizeof(old));
            std::memcpy(moved, memory, old < size ? old : size);
        }
        Free(memory);
        return moved;
    }
}

#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_PSD
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_MALLOC(size) ::helix::image::stb::Allocate(size)
#define STBI_REALLOC(memory, size) ::helix::image::stb::Reallocate(memory, size)
#define STBI_FREE(memory) ::helix::image::stb::Free(memory)
#define STBI_ASSERT(expression) HX_ASSERT(expression)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#pragma warning(disable : 4244 4245 4456 4457 4701 4702 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#endif
#include <stb_image/stb_image.h>
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace helix::image
{
    namespace
    {
        bool Fail(String& error)
        {
            const char* reason = stbi_failure_reason();
            error = reason ? reason : "damaged file";
            return false;
        }

        int Length(Span<const u8> data) { return data.Size() > 0x7FFFFFFF ? 0x7FFFFFFF : int(data.Size()); }

        // Every format of stb_image gives RGBA: 8 bits, or 16 for 16 bit PSD files.
        bool DecodeStill(Span<const u8> data, TextureData& out, String& error)
        {
            // The pixels are decoded first, so a damaged header that claims a huge size costs no allocation of ours.
            int w = 0, h = 0, channels = 0;
            // Only PSD can be 16 bit here. Its depth is read directly: stbi_is_16_bit skips header fields through
            // STBI_NOTUSED, which outside MSVC does not evaluate its argument.
            bool wide = data.Size() >= 26 && std::memcmp(data.Data(), "8BPS", 4) == 0 && data[22] == 0 && data[23] == 16;
            void* pixels = wide ? static_cast<void*>(stbi_load_16_from_memory(data.Data(), Length(data), &w, &h, &channels, 4))
                                : static_cast<void*>(stbi_load_from_memory(data.Data(), Length(data), &w, &h, &channels, 4));
            if (!pixels)
                return Fail(error);
            bool ok = w > 0 && h > 0 && out.Allocate(wide ? PixelFormat::RGBA16 : PixelFormat::RGBA8, u32(w), u32(h));
            if (ok)
                std::memcpy(out.data.Data(), pixels, out.data.Size());
            else
                error = "the image is too large";
            stbi_image_free(pixels);
            return ok;
        }

        // stb_image fills what a short file lacks, so a header of a few bytes could ask for gigabytes of pixels. TGA and
        // BMP say how their pixels are stored: an image needs at least the bytes its storage takes (raw), or the
        // fewest bytes its run length packets can expand from (RLE). Claims beyond that are damaged files.
        u64 Le(Span<const u8> d, usize at, u32 bytes)
        {
            u64 v = 0;
            for (u32 i = 0; i < bytes; i++)
                v |= u64(d[at + i]) << (8 * i);
            return v;
        }

        bool PlausibleTga(Span<const u8> d, String& error)
        {
            if (d.Size() < 18)
                return true;
            u64 colorMapBytes = d[1] ? Le(d, 5, 2) * ((u64(d[7]) + 7) / 8) : 0;
            u64 start = 18 + u64(d[0]) + colorMapBytes;
            u64 pixels = Le(d, 12, 2) * Le(d, 14, 2), bytes = (u64(d[16]) + 7) / 8;
            u64 left = start < d.Size() ? d.Size() - start : 0;
            u32 type = d[2];
            // Raw: every pixel's bytes. Run length: a packet of a header byte and one pixel gives up to 128 pixels.
            u64 most = type >= 9 && type <= 11 ? left / (1 + (bytes ? bytes : 1)) * 128 : bytes ? left / bytes : pixels;
            if (pixels <= most)
                return true;
            error = Format("{} pixels where the file holds at most {}", pixels, most);
            return false;
        }

        bool PlausibleBmp(Span<const u8> d, String& error)
        {
            if (d.Size() < 34)
                return true;
            u64 offset = Le(d, 10, 4), header = Le(d, 14, 4);
            if (header < 40)
                return true;
            u64 w = Le(d, 18, 4), h = Le(d, 22, 4), bits = Le(d, 28, 2), compression = Le(d, 30, 4);
            // Negative heights are top down.
            if (h >= 0x80000000u)
                h = 0x100000000ull - h;
            if (w >= 0x80000000u)
                return true;
            u64 left = offset < d.Size() ? d.Size() - offset : 0;
            u64 most;
            if (compression == 1 || compression == 2)
                most = left / 2 * 255; // RLE8 / RLE4: two bytes give up to 255 pixels
            else if (compression == 0 || compression == 3 || compression == 6)
                most = bits ? (left / ((w * bits + 31) / 32 * 4 ? (w * bits + 31) / 32 * 4 : 1) + 1) * w : w * h;
            else
                return true;
            if (w * h <= most)
                return true;
            error = Format("{} pixels where the file holds at most {}", w * h, most);
            return false;
        }

        u64 Be(Span<const u8> d, usize at, u32 bytes)
        {
            u64 v = 0;
            for (u32 i = 0; i < bytes; i++)
                v = v << 8 | d[at + i];
            return v;
        }

        // PSD: the header, three sections with their lengths, the compression, then the pixels of each channel (stb
        // reads the first four). Raw planes take width * height * bytes each; run length rows (after a table of two
        // bytes per row and channel) take at least two bytes per 128 pixels. stb reads zeros past the end, so a damaged
        // header would make it walk billions of pixels.
        bool PlausiblePsd(Span<const u8> d, String& error)
        {
            if (d.Size() < 26)
                return true;
            u64 channels = Be(d, 12, 2), h = Be(d, 14, 4), w = Be(d, 18, 4), depth = Be(d, 22, 2);
            // Sections that run past the end leave no pixels (stb reads zeros there: raw data).
            u64 at = 26;
            for (int section = 0; section < 3 && at < d.Size(); section++)
                at = at + 4 <= d.Size() ? at + 4 + Be(d, usize(at), 4) : d.Size();
            u64 compression = at + 2 <= d.Size() ? Be(d, usize(at), 2) : 0;
            at += 2;
            u64 left = at < d.Size() ? d.Size() - at : 0;
            u64 read = channels < 4 ? channels : 4;
            u64 bytes = depth == 16 ? 2 : 1;
            // In 64 bits: width and height are below 2^31 (or stb refuses them), channels at most 16.
            u64 need = compression == 1 ? h * channels * 2 + read * h * ((w + 127) / 128) * 2 : compression == 0 ? read * w * h * bytes : 0;
            if (w >= 0x80000000u || h >= 0x80000000u || need <= left)
                return true;
            error = Format("{} x {} pixels where the file holds at most {} bytes of them ({} needed)", w, h, left, need);
            return false;
        }

        bool DecodePsd(Span<const u8> data, TextureData& out, String& error) { return PlausiblePsd(data, error) && DecodeStill(data, out, error); }
        bool DecodeTga(Span<const u8> data, TextureData& out, String& error) { return PlausibleTga(data, error) && DecodeStill(data, out, error); }
        bool DecodeBmp(Span<const u8> data, TextureData& out, String& error) { return PlausibleBmp(data, error) && DecodeStill(data, out, error); }

        // Every frame becomes an array layer, its delay goes to frameDelays.
        bool DecodeGif(Span<const u8> data, TextureData& out, String& error)
        {
            int* delays = nullptr;
            int w = 0, h = 0, frames = 0, channels = 0;
            stbi_uc* pixels = stbi_load_gif_from_memory(data.Data(), Length(data), &delays, &w, &h, &frames, &channels, 4);
            if (!pixels)
                return Fail(error);
            bool ok = w > 0 && h > 0 && frames > 0 && out.Allocate(PixelFormat::RGBA8, u32(w), u32(h), 1, u32(frames));
            if (ok)
            {
                std::memcpy(out.data.Data(), pixels, out.data.Size());
                for (int i = 0; i < frames; i++)
                    out.frameDelays.Push(delays ? u32(delays[i] < 0 ? 0 : delays[i]) : 0u);
            }
            else
                error = "the animation is too large";
            stbi_image_free(pixels);
            stbi_image_free(delays);
            return ok;
        }

        bool Starts(Span<const u8> data, const char* signature, usize size) { return data.Size() >= size && std::memcmp(data.Data(), signature, size) == 0; }
        bool MatchesJpeg(Span<const u8> data) { return Starts(data, "\xFF\xD8\xFF", 3); }
        bool MatchesBmp(Span<const u8> data) { return Starts(data, "BM", 2) && data.Size() >= 26; }
        bool MatchesPsd(Span<const u8> data) { return Starts(data, "8BPS", 4); }
        bool MatchesGif(Span<const u8> data) { return Starts(data, "GIF87a", 6) || Starts(data, "GIF89a", 6); }
    }

    const ImageDecoder& JpegImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"jpg", "jpeg", "jpe", "jfif"};
        static const ImageDecoder kDecoder{"JPEG", Span<const StringView>(kExtensions, 4), &MatchesJpeg, &DecodeStill};
        return kDecoder;
    }

    const ImageDecoder& BmpImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"bmp", "dib"};
        static const ImageDecoder kDecoder{"BMP", Span<const StringView>(kExtensions, 2), &MatchesBmp, &DecodeBmp};
        return kDecoder;
    }

    const ImageDecoder& TgaImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"tga", "targa"};
        static const ImageDecoder kDecoder{"TGA", Span<const StringView>(kExtensions, 2), nullptr, &DecodeTga};
        return kDecoder;
    }

    const ImageDecoder& PsdImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"psd"};
        static const ImageDecoder kDecoder{"PSD", Span<const StringView>(kExtensions, 1), &MatchesPsd, &DecodePsd};
        return kDecoder;
    }

    const ImageDecoder& GifImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"gif"};
        static const ImageDecoder kDecoder{"GIF", Span<const StringView>(kExtensions, 1), &MatchesGif, &DecodeGif};
        return kDecoder;
    }
}
