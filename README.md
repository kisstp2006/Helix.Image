# Helix.Image

Image codecs and compression of the Helix engine, written from the specifications without third party code.

## Contents

| Path | Content |
|---|---|
| `Source/Helix/Image/Image/Image.h` | `Image`: 8 bit R, RG, RGB or RGBA pixels, rows top first |
| `Source/Helix/Image/Compression/Deflate.h` | `Inflate`, `Deflate` (RFC 1951), `ZlibDecompress`, `ZlibCompress` (RFC 1950), `Adler32` |
| `Source/Helix/Image/Codecs/Png.h` | `DecodePng`, `EncodePng` |
| `Tests/`, `Benchmarks/` | Round trips, streams and files of other encoders, damaged input, throughput |
| `Tools/GenerateTestVectors.py` | Writes the PNG and zlib test vectors with Python's zlib and a reference PNG writer |

## Deflate

- Decoder: stored, fixed and dynamic Huffman blocks, a 10 bit lookup table with a canonical walk for longer codes,
  bounds checks on every distance and on the input end.
- Encoder: LZ77 with hash chains (depth by level 1 to 9) and lazy matching from level 4; every block is written as stored,
  fixed or dynamic Huffman, whichever is smallest, with length limited codes.

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

## Build

```
git clone --recursive https://github.com/kisstp2006/Helix.Image
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

## License

GPL-3.0, see `LICENSE`.
