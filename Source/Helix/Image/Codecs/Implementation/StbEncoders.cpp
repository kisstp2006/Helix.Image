#include <Helix/Image/Codecs/Implementation/BuiltInEncoders.h>
#include <Helix/Image/Codecs/Implementation/StbAllocator.h>

#include <Helix/Core/Basics/Assert.h>

// JPG and TGA through the public domain stb_image_write (ThirdParty/stb_image_write, see THIRD-PARTY.txt).
#define STBI_WRITE_NO_STDIO
#define STBIW_MALLOC(size) ::helix::image::stb::Allocate(size)
#define STBIW_REALLOC(memory, size) ::helix::image::stb::Reallocate(memory, size)
#define STBIW_FREE(memory) ::helix::image::stb::Free(memory)
#define STBIW_ASSERT(expression) HX_ASSERT(expression)
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
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
#include <stb_image_write/stb_image_write.h>
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace helix::image
{
    namespace
    {
        void Append(void* context, void* data, int size)
        {
            if (size > 0)
                static_cast<Array<u8>*>(context)->Append(static_cast<const u8*>(data), usize(size));
        }

        bool Fits(const TextureData& texture, String& error)
        {
            if (texture.width > 65535 || texture.height > 65535)
            {
                error = "the image is larger than 65535 pixels on a side";
                return false;
            }
            return true;
        }

        // RGB at the given quality; the alpha channel is dropped, as JPG has none.
        bool EncodeJpg(const TextureData& texture, const EncodeOptions& options, Array<u8>& out, String& error)
        {
            Array<u8> rgb;
            if (!Fits(texture, error) || !FirstSurface(texture, PixelFormat::RGB8, rgb, error))
                return false;
            int quality = int(options.quality < 1 ? 1 : options.quality > 100 ? 100 : options.quality);
            if (!stbi_write_jpg_to_func(&Append, &out, int(texture.width), int(texture.height), 3, rgb.Data(), quality))
            {
                error = "the image could not be written";
                return false;
            }
            return true;
        }

        // Run length encoded RGBA, or RGB for formats without alpha.
        bool EncodeTga(const TextureData& texture, const EncodeOptions&, Array<u8>& out, String& error)
        {
            bool alpha = FormatInfo(texture.format).channels == 4;
            Array<u8> pixels;
            if (!Fits(texture, error) || !FirstSurface(texture, alpha ? PixelFormat::RGBA8 : PixelFormat::RGB8, pixels, error))
                return false;
            if (!stbi_write_tga_to_func(&Append, &out, int(texture.width), int(texture.height), alpha ? 4 : 3, pixels.Data()))
            {
                error = "the image could not be written";
                return false;
            }
            return true;
        }
    }

    const ImageEncoder& JpgImageEncoder()
    {
        static constexpr StringView kExtensions[] = {"jpg", "jpeg"};
        static const ImageEncoder kEncoder{"JPG", Span<const StringView>(kExtensions, 2), &EncodeJpg};
        return kEncoder;
    }

    const ImageEncoder& TgaImageEncoder()
    {
        static constexpr StringView kExtensions[] = {"tga"};
        static const ImageEncoder kEncoder{"TGA", Span<const StringView>(kExtensions, 1), &EncodeTga};
        return kEncoder;
    }
}
