#include <Helix/Image/Codecs/Implementation/BuiltInEncoders.h>
#include <Helix/Image/Codecs/ImageEncoder.h>

#include <Helix/Core/Strings/Format.h>
#include <Helix/Platform/Threading/Thread.h>

namespace helix::image
{
    namespace
    {
        struct Registry
        {
            platform::Mutex mutex;
            Array<ImageEncoder> encoders;

            Registry()
            {
                usize count = 0;
                const ImageEncoder* const* builtIn = BuiltInEncoders(count);
                for (usize i = 0; i < count; i++)
                    encoders.Push(*builtIn[i]);
            }
        };

        Registry& Encoders()
        {
            static Registry registry;
            return registry;
        }

        bool SameIgnoringCase(StringView a, StringView b)
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); i++)
            {
                char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] + 32) : a[i];
                char y = b[i] >= 'A' && b[i] <= 'Z' ? char(b[i] + 32) : b[i];
                if (x != y)
                    return false;
            }
            return true;
        }
    }

    void RegisterImageEncoder(const ImageEncoder& encoder)
    {
        Registry& r = Encoders();
        platform::ScopedLock lock(r.mutex);
        for (ImageEncoder& e : r.encoders)
            if (e.name == encoder.name)
            {
                e = encoder;
                return;
            }
        r.encoders.Push(encoder);
    }

    void ImageEncoders(Array<ImageEncoder>& out)
    {
        Registry& r = Encoders();
        platform::ScopedLock lock(r.mutex);
        out.Clear();
        for (const ImageEncoder& e : r.encoders)
            out.Push(e);
    }

    bool FindImageEncoder(StringView nameOrExtension, ImageEncoder& out)
    {
        StringView key = nameOrExtension;
        if (!key.Empty() && key[0] == '.')
            key = StringView(key.Data() + 1, key.Size() - 1);
        Registry& r = Encoders();
        platform::ScopedLock lock(r.mutex);
        for (const ImageEncoder& e : r.encoders)
        {
            bool match = SameIgnoringCase(e.name, key);
            for (StringView extension : e.extensions)
                match = match || SameIgnoringCase(extension, key);
            if (match)
            {
                out = e;
                return true;
            }
        }
        return false;
    }

    bool EncodeImage(const TextureData& texture, StringView nameOrExtension, Array<u8>& out, String& error, const EncodeOptions& options)
    {
        ImageEncoder encoder;
        if (!FindImageEncoder(nameOrExtension, encoder))
        {
            error = Format("no encoder for {}", nameOrExtension);
            return false;
        }
        out.Clear();
        error.Clear();
        if (encoder.encode(texture, options, out, error))
            return true;
        error = Format("{}: {}", encoder.name, error.Empty() ? StringView("the texture cannot be written") : StringView(error));
        return false;
    }
}
