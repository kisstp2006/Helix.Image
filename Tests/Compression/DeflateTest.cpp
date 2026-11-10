#include <Support/TestVectors.h>

#include <Helix/Core/Hashing/Hash.h>
#include <Helix/Image/Compression/Deflate.h>
#include <Helix/Jobs/Jobs.h>
#include <Helix/Testing/Test.h>

#include <cstdio>
#include <cstring>

using namespace helix;
using namespace helix::image;

namespace
{
    Array<u8> Pattern(usize size, u32 kind)
    {
        Array<u8> data;
        data.Resize(size);
        u32 state = 12345;
        for (usize i = 0; i < size; i++)
        {
            state = state * 1103515245u + 12345u;
            switch (kind)
            {
            case 0: data[i] = u8(state >> 24); break;
            case 1: data[i] = 0; break;
            case 2: data[i] = u8("the quick brown fox jumps over the lazy dog "[(i * 7 + (state >> 30)) % 44]); break;
            default: data[i] = u8((i / 64) % 7 * 30 + (i % 3)); break;
            }
        }
        return data;
    }

    bool RoundTrip(const Array<u8>& input, int level, usize* compressedSize = nullptr)
    {
        Array<u8> packed, unpacked;
        ZlibCompress(input, packed, level);
        if (compressedSize)
            *compressedSize = packed.Size();
        return ZlibDecompress(packed, unpacked) && unpacked.Size() == input.Size() &&
               (input.Empty() || std::memcmp(unpacked.Data(), input.Data(), input.Size()) == 0);
    }
}

HX_TEST(Deflate, RoundTripsAllLevelsAndShapes)
{
    for (usize size : {usize(0), usize(1), usize(2), usize(3), usize(1000), usize(70000), usize(300000)})
        for (u32 kind = 0; kind < 4; kind++)
            for (int level : {0, 1, 6, 9})
            {
                bool ok = RoundTrip(Pattern(size, kind), level);
                if (!ok)
                    std::printf("    fail size %zu kind %u level %d\n", size, kind, level);
                HX_CHECK(ok);
            }
}

HX_TEST(Deflate, CompressesRedundantData)
{
    usize zeros = 0, text = 0, noise = 0;
    HX_REQUIRE(RoundTrip(Pattern(1 << 20, 1), 6, &zeros));
    HX_REQUIRE(RoundTrip(Pattern(100000, 2), 9, &text));
    HX_REQUIRE(RoundTrip(Pattern(100000, 0), 6, &noise));
    // zlib 1.3 reaches 1039, 44400 and 100041 bytes on the same inputs; stay within 5 percent of it.
    std::printf("    zeros %zu, text %zu, noise %zu bytes\n", zeros, text, noise);
    HX_CHECK(zeros < 1100);
    HX_CHECK(text < 46600);
    HX_CHECK(noise < 100100);
}

HX_TEST(Deflate, DecodesStreamsOfAnotherEncoder)
{
    struct Case
    {
        const u8* data;
        usize size;
    };
    const Case cases[] = {{vectors::kZlibLevel1, sizeof(vectors::kZlibLevel1)},
                          {vectors::kZlibLevel9, sizeof(vectors::kZlibLevel9)},
                          {vectors::kZlibStored, sizeof(vectors::kZlibStored)},
                          {vectors::kZlibFixed, sizeof(vectors::kZlibFixed)}};
    for (const Case& c : cases)
    {
        Array<u8> out;
        HX_REQUIRE(ZlibDecompress(Span<const u8>(c.data, c.size), out));
        HX_CHECK_EQ(out.Size(), usize(vectors::kZlibInputSize));
        HX_CHECK_EQ(Crc32(out.Data(), out.Size()), vectors::kZlibInputCrc);
    }
}

HX_TEST(Deflate, RejectsCorruptStreams)
{
    Array<u8> packed, out;
    Array<u8> input = Pattern(5000, 2);
    ZlibCompress(input, packed, 9);
    Array<u8> bad = packed;
    bad[bad.Size() - 1] ^= 1;
    HX_CHECK(!ZlibDecompress(bad, out));
    out.Clear();
    HX_CHECK(!ZlibDecompress(Span<const u8>(packed.Data(), packed.Size() / 2), out));
    u32 survived = 0;
    for (usize i = 2; i < packed.Size(); i += 7)
    {
        bad = packed;
        bad[i] ^= 0x5A;
        out.Clear();
        survived += ZlibDecompress(bad, out) ? 1 : 0;
    }
    HX_CHECK_EQ(survived, 0u);
}

HX_TEST(Deflate, ChunkedOutputIsIdenticalWithAndWithoutWorkers)
{
    Array<u8> input = Pattern(1500000, 2);
    for (usize i = 0; i < input.Size(); i += 97)
        input[i] = u8(i >> 3);
    Array<u8> serial, parallel, unpacked;
    ZlibCompress(input, serial, 6);
    jobs::Initialize();
    ZlibCompress(input, parallel, 6);
    jobs::Shutdown();
    HX_CHECK_EQ(serial.Size(), parallel.Size());
    HX_CHECK(std::memcmp(serial.Data(), parallel.Data(), serial.Size()) == 0);
    HX_REQUIRE(ZlibDecompress(parallel, unpacked));
    HX_REQUIRE(unpacked.Size() == input.Size());
    HX_CHECK(std::memcmp(unpacked.Data(), input.Data(), input.Size()) == 0);
    // Matches may reach into the previous chunk, so cutting into chunks costs little.
    std::printf("    chunked %zu bytes\n", parallel.Size());
}
