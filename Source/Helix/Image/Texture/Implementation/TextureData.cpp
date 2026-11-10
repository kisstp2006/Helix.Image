#include <Helix/Image/Texture/TextureData.h>

namespace helix::image
{
    bool TextureData::Allocate(PixelFormat pixelFormat, u32 w, u32 h, u32 d, u32 layerCount, u32 faceCount, u32 mipCount)
    {
        if (!w || !h || !d || !layerCount || !faceCount || !mipCount || mipCount > 32 || pixelFormat == PixelFormat::Unknown ||
            pixelFormat >= PixelFormat::Count || u64(layerCount) * faceCount > (1u << 16))
            return false;
        format = pixelFormat;
        width = w;
        height = h;
        depth = d;
        layers = layerCount;
        faces = faceCount;
        mips = mipCount;
        subresources.Clear();
        subresources.Reserve(usize(layerCount) * faceCount * mipCount);
        u64 total = 0;
        for (u32 layer = 0; layer < layerCount; layer++)
            for (u32 face = 0; face < faceCount; face++)
                for (u32 mip = 0; mip < mipCount; mip++)
                {
                    Subresource& s = subresources.Emplace();
                    s.width = w >> mip ? w >> mip : 1;
                    s.height = h >> mip ? h >> mip : 1;
                    s.depth = d >> mip ? d >> mip : 1;
                    s.offset = total;
                    s.size = SurfaceBytes(pixelFormat, s.width, s.height) * s.depth;
                    total += s.size;
                    if (total > kMaxTextureBytes)
                        return false;
                }
        data.Resize(usize(total));
        return true;
    }
}
