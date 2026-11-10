#pragma once

#include <Helix/Core/Basics/Span.h>
#include <Helix/Core/Containers/Array.h>

namespace helix::image
{
    // Raw deflate streams (RFC 1951) and the zlib wrapper (RFC 1950). Decoded or encoded bytes are appended to `out`.
    bool Inflate(Span<const u8> in, Array<u8>& out);
    bool ZlibDecompress(Span<const u8> in, Array<u8>& out);

    // Level 0 stores, 1 to 9 trade speed for match search depth; every block picks stored, fixed or dynamic Huffman codes.
    void Deflate(Span<const u8> in, Array<u8>& out, int level = 6);
    void ZlibCompress(Span<const u8> in, Array<u8>& out, int level = 6);

    u32 Adler32(const void* data, usize size, u32 adler = 1);
}
