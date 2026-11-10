#pragma once

#include <Helix/Image/Texture/BlockCodec.h>

namespace helix::image
{
    using BlockEncode = void (*)(const u8* texels, u8* block, CompressionQuality quality);

    inline void SetEncoder(BlockCodec* codecs, usize count, PixelFormat format, BlockEncode encode)
    {
        for (usize i = 0; i < count; i++)
            if (codecs[i].format == format)
                codecs[i].encode = encode;
    }

    // Each codec file appends its decoders to the built in list, then sets the encoders it has.
    namespace bc
    {
        void AddDecoders(BlockCodec* codecs, usize& count);
        void AddEncoders(BlockCodec* codecs, usize count);
    }
    namespace bptc
    {
        void AddDecoders(BlockCodec* codecs, usize& count);
        void AddEncoders(BlockCodec* codecs, usize count);
    }
    namespace etc
    {
        void AddDecoders(BlockCodec* codecs, usize& count);
        void AddEncoders(BlockCodec* codecs, usize count);
    }
    namespace astc
    {
        void AddDecoders(BlockCodec* codecs, usize& count);
        void AddEncoders(BlockCodec* codecs, usize count);
    }

    // The block codecs of this library, registered on first use of the registry.
    inline const BlockCodec* BuiltInBlockCodecs(usize& count)
    {
        struct List
        {
            BlockCodec codecs[64];
            usize count = 0;
            List()
            {
                bc::AddDecoders(codecs, count);
                bptc::AddDecoders(codecs, count);
                etc::AddDecoders(codecs, count);
                astc::AddDecoders(codecs, count);
                bc::AddEncoders(codecs, count);
                bptc::AddEncoders(codecs, count);
                etc::AddEncoders(codecs, count);
                astc::AddEncoders(codecs, count);
            }
        };
        static const List list;
        count = list.count;
        return list.codecs;
    }
}
