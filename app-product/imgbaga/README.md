# imgbaga

Raster images for Baga — **pure Baga**, no libpng / libjpeg / FFI.

Modeled on the Rust [`image`](https://github.com/image-rs/image) crate:
one RGBA8 `Image` (the `DynamicImage` analogue), magic-byte `img_guess`,
per-format codecs, and a small `imageops` set.

| | |
|--|--|
| **sandak** | `imgbaga` **0.6.0** |
| **Layout** | `img` · `types` · `sniff` · `ops` · `png` · `jpeg` · `gif` · `qoi` · `ico` · `tiff` · `webp` · `bmp` · `pnm` |
| **Deps** | `std`, `zipbaga` (DEFLATE + CRC-32), `pathbaga` |
| **Tests** | `tests/img_test.baga` |
| **Demo** | `demo.baga` — `info` / `convert` / `make` |

## Formats

| Format | Sniff | Info (w×h) | Decode | Encode |
|--------|-------|------------|--------|--------|
| PNG | ✅ | ✅ | ✅ 8/16-bit, Adam7, tRNS; 1/2/4-bit gray/palette | ✅ RGBA8 |
| JPEG | ✅ | ✅ | ✅ baseline SOF0 (1 or 3 comp) | ✅ 4:2:0 q=85 |
| GIF | ✅ | ✅ | ✅ first frame + `gif_anim_decode` (disposal, delay) | — |
| BMP | ✅ | ✅ | ✅ BI_RGB 24/32 | ✅ 24-bit |
| PNM | ✅ | ✅ | ✅ P2/P3/P5/P6 | ✅ P6 |
| QOI | ✅ | ✅ | ✅ | ✅ RGBA |
| ICO | ✅ | ✅ | ✅ PNG or 24/32-bit DIB | ✅ PNG payload |
| TIFF | ✅ | ✅ | ✅ uncompressed + PackBits, 8-bit gray/RGB/RGBA | — |
| WebP | ✅ | ✅ | ✅ VP8L lossless + lossy VP8 keyframe | — |
| HDR | ✅ | — | — | — |

PNG IDAT is zlib (RFC 1950) over `zipbaga` raw DEFLATE. JPEG IDCT is the
integer nanojpeg / Loeffler transform; encode is Annex K Huffman + 4:2:0 FDCT.

### WebP

`webp_info` / `webp_decode` walk the RIFF (`VP8 `, `VP8L`, `VP8X`).

- **VP8L** — lossless: Huffman, LZ77, color cache, predictor / color /
  subtract-green / indexing transforms (`webp_l.baga`).
- **VP8** — lossy keyframe (RFC 6386, `golang.org/x/image/vp8`): bool
  decoder, tokens, iDCT + WHT, intra pred, loop filter
  (`webp_vp8*.baga`). YUV→RGB is libwebp BT.601 (bit-exact vs
  ImageMagick on uniform blocks and corners).

Not in v1: WebP encode, inter frames, VP8X alpha, fancy chroma upsample
(nearest 4:2:0; interior 2×2 edges can differ from `dwebp`).

## API

```baga
import "imgbaga/img.baga"

let im = img_load(file_bytes)
// im.ok, im.w, im.h, im.px   (RGBA8, row-major)
let p = img_pixel(im, x, y)   // p.r g b a
img_put(im, x, y, r, g, b, a)

let fmt = img_guess(file_bytes)          // img_fmt_png() …
let inf = img_info(file_bytes)           // format + w + h (no full decode)

let out = img_encode(im, img_fmt_png())  // png / jpeg / bmp / pnm / qoi / ico
img_save(im, "out.bmp")?

let g = img_grayscale(im)
let r = img_resize(im, 64, 64)           // nearest
let b = img_resize_bilinear(im, 64, 64)
let c = img_crop(im, 0, 0, 10, 10)
```

Also: `img_open`, `img_flip_h` / `img_flip_v`, `img_rotate90` / `img_rotate180`,
`img_invert`, `img_new`, `img_resize_nearest`.

GIF animation: `gif_anim_decode` → `gif_anim_frame(a, i)` / `gif_anim_delay(a, i)`.

`img_version()` is `"0.6.0"`. v1 limit: longest side 4096, whole image in memory.

## Modules

| File | Role |
|------|------|
| `img.baga` | public surface (`guess` / `info` / `load` / `encode` / `open` / `save`) |
| `types.baga` | `Image`, `ImgInfo`, `Pixel`, format ids |
| `sniff.baga` | magic bytes + extension → format |
| `ops.baga` | crop, flip, rotate, nearest + bilinear resize, gray, invert |
| `png.baga` / `zlib.baga` | PNG + zlib wrapper over zipbaga |
| `jpeg.baga` / `jpeg_enc.baga` | baseline decode / encode |
| `gif.baga` | first frame + animation |
| `webp.baga` / `webp_l.baga` / `webp_vp8*.baga` | WebP RIFF + VP8L + VP8 |
| `bmp` · `pnm` · `qoi` · `ico` · `tiff` | remaining codecs |

## Run

```bash
./baga -I . -I app-product tests/img_test.baga
# img_test: all passed

./baga -I . -I app-product app-product/imgbaga/demo.baga make /tmp/probe.png
./baga -I . -I app-product app-product/imgbaga/demo.baga info /tmp/probe.png
./baga -I . -I app-product app-product/imgbaga/demo.baga convert /tmp/probe.png /tmp/probe.jpg
# info/convert четат и .webp / .tif; запис: png, jpg, bmp, ppm, qoi, ico
```

`sandak build` from `app-product/imgbaga` (needs `baga` on `PATH`).

## Related

- Rust `image` — `ImageBuffer` / `DynamicImage` / `guess_format` / `imageops`
- `zipbaga` — inflate/deflate + IEEE CRC-32 used by PNG
- [gaps](gaps.md) · [PLAN](PLAN.md)
