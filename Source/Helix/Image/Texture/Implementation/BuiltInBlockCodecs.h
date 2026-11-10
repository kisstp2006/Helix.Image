#pragma once

#include <Helix/Image/Texture/BlockCodec.h>

namespace helix::image
{
    // Each codec file appends its codecs to the built in list.
    namespace bc
    {
        void AddDecoders(BlockCodec* codecs, usize& count);
    }
    namespace bptc
    {
        void AddDecoders(BlockCodec* codecs, usize& count);
    }
    namespace etc
    {
        void AddDecoders(BlockCodec* codecs, usize& count);
    }
    namespace astc
    {
        void AddDecoders(BlockCodec* codecs, usize& count);
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
            }
        };
        static const List list;
        count = list.count;
        return list.codecs;
    }
}
