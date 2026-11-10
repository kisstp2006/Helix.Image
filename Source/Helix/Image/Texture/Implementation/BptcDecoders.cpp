#include <Helix/Image/Texture/Convert.h>
#include <Helix/Image/Texture/Implementation/BcCommon.h>
#include <Helix/Image/Texture/Implementation/BptcTables.h>
#include <Helix/Image/Texture/Implementation/BuiltInBlockCodecs.h>

#include <cstring>

// BC7 (RGBA8) and BC6H (RGBA16Float) blocks to texels, from the block compression formats of Direct3D 11.
namespace helix::image::bptc
{
    namespace
    {
        void DecodeBc7(const u8* block, u8* texels)
        {
            u32 mode = 0;
            while (mode < 8 && !(block[0] & (1u << mode)))
                mode++;
            if (mode == 8)
            {
                std::memset(texels, 0, 64);
                return;
            }
            const Bc7Mode& m = kBc7Modes[mode];
            BitReader r{block, mode + 1};
            u32 partition = r.Read(m.partitionBits), rotation = r.Read(m.rotationBits), selection = r.Read(m.selectionBits);
            u32 endpoints[3][2][4] = {};
            for (u32 c = 0; c < 3; c++)
                for (u32 s = 0; s < m.subsets; s++)
                    for (u32 e = 0; e < 2; e++)
                        endpoints[s][e][c] = r.Read(m.colorBits);
            for (u32 s = 0; s < m.subsets; s++)
                for (u32 e = 0; e < 2; e++)
                    endpoints[s][e][3] = m.alphaBits ? r.Read(m.alphaBits) : 255;
            u32 colorBits = m.colorBits, alphaBits = m.alphaBits;
            if (m.endpointPBits || m.sharedPBits)
            {
                u32 p[3][2];
                for (u32 s = 0; s < m.subsets; s++)
                {
                    if (m.sharedPBits)
                        p[s][0] = p[s][1] = r.Read(1);
                    else
                    {
                        p[s][0] = r.Read(1);
                        p[s][1] = r.Read(1);
                    }
                }
                for (u32 s = 0; s < m.subsets; s++)
                    for (u32 e = 0; e < 2; e++)
                        for (u32 c = 0; c < 4; c++)
                            if (c < 3 || m.alphaBits)
                                endpoints[s][e][c] = (endpoints[s][e][c] << 1) | p[s][e];
                colorBits++;
                alphaBits += m.alphaBits ? 1 : 0;
            }
            for (u32 s = 0; s < m.subsets; s++)
                for (u32 e = 0; e < 2; e++)
                {
                    for (u32 c = 0; c < 3; c++)
                        endpoints[s][e][c] = Unquantize(endpoints[s][e][c], colorBits);
                    if (m.alphaBits)
                        endpoints[s][e][3] = Unquantize(endpoints[s][e][3], alphaBits);
                }
            u32 primary[16], secondary[16];
            for (u32 i = 0; i < 16; i++)
                primary[i] = r.Read(m.indexBits - (IsAnchor(m.subsets, partition, i) && (i == 0 || m.subsets > 1) ? 1 : 0));
            if (m.secondaryBits)
                for (u32 i = 0; i < 16; i++)
                    secondary[i] = r.Read(m.secondaryBits - (i == 0 ? 1 : 0));
            for (u32 i = 0; i < 16; i++)
            {
                u32 s = Subset(m.subsets, partition, i);
                u32 colorIndex = primary[i], alphaIndex = primary[i], colorBitsUsed = m.indexBits, alphaBitsUsed = m.indexBits;
                if (m.secondaryBits)
                {
                    alphaIndex = secondary[i];
                    alphaBitsUsed = m.secondaryBits;
                    if (selection)
                    {
                        colorIndex = secondary[i];
                        colorBitsUsed = m.secondaryBits;
                        alphaIndex = primary[i];
                        alphaBitsUsed = m.indexBits;
                    }
                }
                u8* t = texels + i * 4;
                for (u32 c = 0; c < 3; c++)
                    t[c] = u8(Interpolate(endpoints[s][0][c], endpoints[s][1][c], Weights(colorBitsUsed)[colorIndex]));
                t[3] = u8(Interpolate(endpoints[s][0][3], endpoints[s][1][3], Weights(alphaBitsUsed)[alphaIndex]));
                if (rotation)
                {
                    u8 a = t[3];
                    t[3] = t[rotation - 1];
                    t[rotation - 1] = a;
                }
            }
        }

        template <bool Signed>
        void DecodeBc6(const u8* block, u8* texels)
        {
            const Bc6Layouts& layouts = Bc6LayoutTable();
            u16* out = reinterpret_cast<u16*>(texels);
            BitReader r{block};
            u32 code = r.Read(2);
            if (code > 1)
                code |= r.Read(3) << 2;
            i32 mode = -1;
            for (u32 m = 0; m < 14; m++)
                if (kBc6Modes[m].code == code)
                    mode = i32(m);
            if (mode < 0)
            {
                for (u32 i = 0; i < 16; i++)
                {
                    out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = 0;
                    out[i * 4 + 3] = 0x3C00;
                }
                return;
            }
            const Bc6Mode& m = kBc6Modes[mode];
            u32 e[4][3] = {};
            for (u32 i = 0; i < layouts.count[mode]; i++)
                e[layouts.field[mode][i] / 3][layouts.field[mode][i] % 3] |= r.Read(1) << layouts.bit[mode][i];
            u32 partition = m.regions == 2 ? r.Read(5) : 0;
            u32 endpoints = m.regions * 2u;
            i32 v[4][3];
            u32 mask = (1u << m.endpointBits) - 1;
            for (u32 c = 0; c < 3; c++)
            {
                v[0][c] = Signed ? SignExtend(e[0][c], m.endpointBits) : i32(e[0][c]);
                for (u32 k = 1; k < endpoints; k++)
                {
                    if (m.transformed)
                    {
                        i32 sum = i32((u32(e[0][c]) + u32(SignExtend(e[k][c], m.deltaBits[c]))) & mask);
                        v[k][c] = Signed ? SignExtend(u32(sum), m.endpointBits) : sum;
                    }
                    else
                        v[k][c] = Signed ? SignExtend(e[k][c], m.endpointBits) : i32(e[k][c]);
                }
                for (u32 k = 0; k < endpoints; k++)
                    v[k][c] = UnquantizeHalf(v[k][c], m.endpointBits, Signed);
            }
            u32 indexBits = m.regions == 2 ? 3 : 4;
            const u8* weights = Weights(indexBits);
            for (u32 i = 0; i < 16; i++)
            {
                bool anchor = i == 0 || (m.regions == 2 && i == kAnchors2[partition]);
                u32 index = r.Read(indexBits - (anchor ? 1 : 0));
                u32 s = m.regions == 2 ? (kPartitions2[partition] >> i) & 1 : 0;
                for (u32 c = 0; c < 3; c++)
                {
                    i32 a = v[s * 2][c], b = v[s * 2 + 1][c], w = weights[index];
                    i32 x = ((64 - w) * a + w * b + 32) >> 6;
                    out[i * 4 + c] = FinishHalf(x, Signed);
                }
                out[i * 4 + 3] = 0x3C00;
            }
        }
    }

    void AddDecoders(BlockCodec* codecs, usize& count)
    {
        codecs[count++] = {PixelFormat::BC6H, PixelFormat::RGBA16Float, &DecodeBc6<false>, nullptr};
        codecs[count++] = {PixelFormat::BC6HSigned, PixelFormat::RGBA16Float, &DecodeBc6<true>, nullptr};
        codecs[count++] = {PixelFormat::BC7, PixelFormat::RGBA8, &DecodeBc7, nullptr};
    }
}
