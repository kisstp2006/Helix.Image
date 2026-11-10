#pragma once

#include <Helix/Core/Basics/Span.h>

namespace helix::image
{
    // Bounds checked reads from a file in memory: a read past the end gives zero and marks the reader as failed, so a
    // parser checks `ok` once after a group of reads instead of before each one.
    struct ByteReader
    {
        const u8* data = nullptr;
        usize size = 0;
        usize at = 0;
        bool bigEndian = false;
        bool ok = true;

        ByteReader() = default;
        explicit ByteReader(Span<const u8> bytes, bool big = false) : data(bytes.Data()), size(bytes.Size()), bigEndian(big) {}

        bool Has(usize count) const { return at <= size && count <= size - at; }
        bool Seek(u64 offset)
        {
            if (offset > size)
                return ok = false;
            at = usize(offset);
            return true;
        }
        bool Skip(usize count) { return Has(count) ? (at += count, true) : (ok = false); }

        u8 U8()
        {
            if (!Has(1))
                return ok = false, u8(0);
            return data[at++];
        }
        u16 U16()
        {
            if (!Has(2))
                return ok = false, u16(0);
            const u8* p = data + at;
            at += 2;
            return bigEndian ? u16((p[0] << 8) | p[1]) : u16(p[0] | (p[1] << 8));
        }
        u32 U32()
        {
            if (!Has(4))
                return ok = false, 0u;
            const u8* p = data + at;
            at += 4;
            return bigEndian ? (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3] : p[0] | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
        }
        u64 U64()
        {
            u64 a = U32(), b = U32();
            return bigEndian ? (a << 32) | b : a | (b << 32);
        }
        // A view of the next `count` bytes; empty (and failed) when the file is shorter.
        Span<const u8> Bytes(usize count)
        {
            if (!Has(count))
                return ok = false, Span<const u8>();
            Span<const u8> s(data + at, count);
            at += count;
            return s;
        }
    };
}
