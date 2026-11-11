# Helix.Image

Image codecs and compression of the Helix engine. PNG, deflate and the other own codecs are written from the
specifications; JPEG, BMP, TGA, PSD and GIF are read with the public domain stb_image, JPG and TGA written with stb_image_write
(see `THIRD-PARTY.txt`).

## Contents

| Path | Content |
|---|---|
| `Source/Helix/Image/Image/Image.h` | `Image`: 8 bit R, RG, RGB or RGBA pixels, rows top first |
| `Source/Helix/Image/Formats/PixelFormat.h` | `PixelFormat`: 8 / 16 bit, half, float, packed, BC1 to BC7, ETC2 / EAC, ASTC; `FormatInfo`, `SurfaceBytes` |
| `Source/Helix/Image/Texture/TextureData.h` | `TextureData`: layers, faces and mips in one block with a subresource table |
| `Source/Helix/Image/Codecs/ImageDecoder.h` | `DecodeImage` and the decoder registry (`RegisterImageDecoder`, `FindImageDecoder`) |
| `Source/Helix/Image/Codecs/ImageEncoder.h` | `EncodeImage`, `EncodeOptions` (JPG quality, `ExrFlags`) and the encoder registry |
| `Source/Helix/Image/Texture/Convert.h` | `ConvertPixels`, `ToFloat`, `FromFloat` for every uncompressed format, `FloatToHalf`, `HalfToFloat` |
| `Source/Helix/Image/Texture/Color.h` | sRGB and linear curves, 8 bit tables |
| `Source/Helix/Image/Texture/Resize.h` | `Resample` (Mitchell, Bilinear, Box, Kaiser), `ResizeTexture`, `FitToMaxSize` |
| `Source/Helix/Image/Texture/Mipmaps.h` | `GenerateMipmaps` with the importer's mip settings |
| `Source/Helix/Image/Compression/Deflate.h` | `Inflate`, `Deflate` (RFC 1951), `ZlibDecompress`, `ZlibCompress` (RFC 1950), `Adler32` |
| `Source/Helix/Image/Codecs/Png.h` | `DecodePng`, `EncodePng` |
| `Tests/`, `Benchmarks/`, `Fuzz/` | Round trips, files of other encoders, damaged input, throughput, a fuzz target per decoder |
| `Tools/GenerateTestImages.ps1` | Writes JPEG, GIF, BMP and TIFF test files with the Windows imaging codecs and the pixels Windows decodes |
| `ThirdParty/stb_image/`, `ThirdParty/stb_image_write/` | stb_image v2.30 and stb_image_write v1.16, unchanged |
| `Tools/GenerateTestVectors.py` | Writes the PNG and zlib test vectors with Python's zlib and a reference PNG writer |

## Deflate

- Decoder: stored, fixed and dynamic Huffman blocks, a 10 bit lookup table with a canonical walk for longer codes,
  bounds checks on every distance and on the input end.
- Encoder: LZ77 with hash chains (depth by level 1 to 9) and lazy matching from level 4; every block is written as stored,
  fixed or dynamic Huffman, whichever is smallest, with length limited codes.

- Inputs of 512 KB and more are cut into 256 KB chunks that compress as separate jobs on Helix.Jobs, each using the 32 KB
  before it as dictionary. The cut depends only on the size, so the output is identical with any number of workers; without an
  initialized job system the chunks run on the calling thread. Decoding is sequential by nature of the format.

## Decoding

```cpp
helix::image::TextureData texture;
helix::String error;
if (helix::image::DecodeImage(bytes, texture, error, ".tga"))
    Upload(texture.format, texture.Pixels(), texture.width, texture.height);
```

Decoders are found by their signature, and by the extension for formats without one (TGA). A decoder is added with
`RegisterImageDecoder` (a decoder with the same name is replaced). `TextureData` keeps every layer, face and mip in one
block, ordered layer, face, mip, with a table of offsets and sizes. Damaged files are errors that name the problem.

| Format | Decoder | Result |
|---|---|---|
| PNG | own | RGBA8, RGBA16 for 16 bit files |
| JPEG, BMP, TGA | stb_image | RGBA8 |
| PSD | stb_image | RGBA8, RGBA16 for 16 bit files (flattened composite) |
| GIF | stb_image | RGBA8, one array layer per frame, `frameDelays` in milliseconds |
| HDR (Radiance RGBE) | own | RGB32Float, linear; flat, old and per channel run length scanlines, both orientations |
| TIFF | own | RGBA8, RGBA16 or RGBA32Float; strips and tiles, chunky and planar, none / PackBits / LZW / Deflate, horizontal and float predictors, gray, RGB, palette, alpha (associated alpha made straight); CCITT, JPEG and other compressions are errors that name them |
| EXR | own | RGBA16Float when every used channel is half, else RGBA32Float; linear; scanline and tiled (first level), NONE, RLE, ZIPS, ZIP, PIZ; R, G, B, A, layer prefixed names or Y |
| DDS | own | the stored format: legacy FourCC and pixel masks or the DX10 header; 2D, cube, array, volume, mips; legacy luminance (L8, A8L8, L16, known by the flag) widened to gray RGBA8 / RGBA16 |
| KTX, KTX2 | own | the stored format; KTX rows unpadded, KTX2 supercompression none or zlib (Basis and Zstandard are errors) |
| PVR (v3) | own | the stored format: BC, ETC2 / EAC, ASTC and uncompressed channel layouts (PVRTC is an error) |

## Encoding

```cpp
helix::image::EncodeOptions options;
options.exrFlags = helix::image::ExrCompressPiz | helix::image::ExrOutputAsFloat;
helix::Array<helix::u8> file;
helix::String error;
helix::image::EncodeImage(texture, "exr", file, error, options);
```

Encoders write the first subresource of an uncompressed texture, converted from its format; they are found by name or
extension and added with `RegisterImageEncoder`.

| Format | Encoder | Output |
|---|---|---|
| PNG | own | gray, RGB or RGBA from the channels of the format; 16 bit samples for 16 bit, float and 10 bit formats |
| JPG | stb_image_write | RGB at `quality` (1 to 100, 75 by default), alpha dropped |
| TGA | stb_image_write | run length encoded RGBA, or RGB for formats without alpha |
| EXR | own | R, G, B, A as the format has them, half or float (`ExrOutputAsFloat`), NONE, RLE (`ExrCompressRle`), ZIP (`ExrCompressZip`) or PIZ (`ExrCompressPiz`) |

## Mip maps and resizing

```cpp
helix::image::MipmapSettings settings;
settings.filter = helix::image::MipFilter::Kaiser;
settings.srgb = true;
settings.preserveCoverage = true;
helix::image::GenerateMipmaps(texture, settings, withMips, error);
```

- Every level is filtered from the one above: Box (the area each texel covers, 2 x 2 when halving, 3 taps on an odd
  side) or Kaiser (a Kaiser windowed sinc of radius 3). sRGB data is filtered in linear light.
- `preserveCoverage` scales alpha so each level keeps the share of texels above `alphaReference`; `normalMap`
  renormalizes; `borderMipmap` keeps the top level's edge; `fadeout` fades levels `fadeStart` to `fadeEnd` to gray
  (to a flat normal for normal maps). The top level is copied unchanged.
- `Resample` and `ResizeTexture` take the Mitchell (B = C = 1/3) and Bilinear filters; `FitToMaxSize` gives the size a
  Max Size setting leaves. Rows and conversions run as jobs on Helix.Jobs with the same result as one thread.

Mip chain of a 4096 x 4096 RGBA8 sRGB texture, release, Ryzen 7 3750H: 0.58 s on one thread, 0.24 s with 7 workers.

## PNG

```cpp
helix::image::Image image;
if (helix::image::DecodePng(bytes, image))
    UseRgba(image.pixels.Data(), image.width, image.height);

helix::Array<helix::u8> file;
helix::image::EncodePng(image, file);
```

- Decoding: all color types, bit depths 1 to 16, palettes with `tRNS`, gray and RGB transparency keys, Adam7 interlacing,
  split `IDAT` chunks, chunk CRC and zlib checksum verification; unknown ancillary chunks are skipped. Output is RGBA8.
- Encoding: gray, gray alpha, RGB or RGBA at 8 bits from the image format, the filter of each row chosen by the smallest sum
  of absolute residuals.

512 x 512 RGBA gradient, release build, Ryzen 7 3750H: encode about 12 ms, decode about 3 ms. Deflate and inflate run at
the speed of zlib level 6 on 1 MB of mixed data.
With 7 workers a 2048 x 2048 image encodes about 3.6 times faster (row filters and deflate chunks run as jobs) and 4 MB
deflate about 5 times faster than on one thread.

## Build

```
git clone https://github.com/kisstp2006/Helix.Image
git -C Helix.Image submodule update --init
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

## License

GPL-3.0, see `LICENSE`.
