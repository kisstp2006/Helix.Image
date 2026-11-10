#include <Helix/Image/Codecs/Implementation/BuiltInDecoders.h>

#include <Helix/Core/Basics/Assert.h>
#include <Helix/Core/Memory/Allocator.h>

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

    void* Reallocate(void* memory, usize size)
    {
        void* moved = Allocate(size);
        if (moved && memory)
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
        static const ImageDecoder kDecoder{"BMP", Span<const StringView>(kExtensions, 2), &MatchesBmp, &DecodeStill};
        return kDecoder;
    }

    const ImageDecoder& TgaImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"tga", "targa"};
        static const ImageDecoder kDecoder{"TGA", Span<const StringView>(kExtensions, 2), nullptr, &DecodeStill};
        return kDecoder;
    }

    const ImageDecoder& PsdImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"psd"};
        static const ImageDecoder kDecoder{"PSD", Span<const StringView>(kExtensions, 1), &MatchesPsd, &DecodeStill};
        return kDecoder;
    }

    const ImageDecoder& GifImageDecoder()
    {
        static constexpr StringView kExtensions[] = {"gif"};
        static const ImageDecoder kDecoder{"GIF", Span<const StringView>(kExtensions, 1), &MatchesGif, &DecodeGif};
        return kDecoder;
    }
}
