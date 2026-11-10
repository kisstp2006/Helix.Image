#include <Helix/Image/Codecs/Implementation/BuiltInDecoders.h>
#include <Helix/Image/Codecs/ImageDecoder.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/Platform/Threading/Thread.h>

namespace helix::image
{
    namespace
    {
        struct Registry
        {
            platform::Mutex mutex;
            Array<ImageDecoder> decoders;

            Registry()
            {
                usize count = 0;
                const ImageDecoder* const* builtIn = BuiltInDecoders(count);
                for (usize i = 0; i < count; i++)
                    decoders.Push(*builtIn[i]);
            }
        };

        Registry& Decoders()
        {
            static Registry registry;
            return registry;
        }

        bool SameExtension(StringView a, StringView b)
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); i++)
            {
                char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] + 32) : a[i];
                if (x != b[i])
                    return false;
            }
            return true;
        }
    }

    void RegisterImageDecoder(const ImageDecoder& decoder)
    {
        Registry& r = Decoders();
        platform::ScopedLock lock(r.mutex);
        for (ImageDecoder& d : r.decoders)
            if (d.name == decoder.name)
            {
                d = decoder;
                return;
            }
        r.decoders.Push(decoder);
    }

    void ImageDecoders(Array<ImageDecoder>& out)
    {
        Registry& r = Decoders();
        platform::ScopedLock lock(r.mutex);
        out.Clear();
        for (const ImageDecoder& d : r.decoders)
            out.Push(d);
    }

    bool FindImageDecoder(Span<const u8> data, StringView extension, ImageDecoder& out)
    {
        if (!extension.Empty() && extension[0] == '.')
            extension = StringView(extension.Data() + 1, extension.Size() - 1);
        Registry& r = Decoders();
        platform::ScopedLock lock(r.mutex);
        for (const ImageDecoder& d : r.decoders)
            if (d.matches && d.matches(data))
            {
                out = d;
                return true;
            }
        for (const ImageDecoder& d : r.decoders)
            for (StringView e : d.extensions)
                if (!d.matches && SameExtension(extension, e))
                {
                    out = d;
                    return true;
                }
        return false;
    }

    bool DecodeImage(Span<const u8> data, TextureData& out, String& error, StringView extension)
    {
        ImageDecoder decoder;
        if (!FindImageDecoder(data, extension, decoder))
        {
            error = extension.Empty() ? String(StringView("not an image format that has a decoder")) : Format("no decoder for this file (extension {})", extension);
            return false;
        }
        out = TextureData();
        error.Clear();
        if (decoder.decode(data, out, error))
            return true;
        if (error.Empty())
            error = Format("damaged {} file", decoder.name);
        else
            error = Format("{}: {}", decoder.name, error);
        return false;
    }
}
