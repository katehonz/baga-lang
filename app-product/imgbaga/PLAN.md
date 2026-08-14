# imgbaga — plan

Date: 2026-08-15
Status: P5 done
Goal: raster package in `app-product/`, shaped like Rust `image`.

## Phases

### P0 ✅

1. `Image` RGBA8 + sniff + info.
2. BMP / PNM full; PNG 8-bit; JPEG baseline decode.
3. `imageops` subset; demo CLI; `tests/img_test.baga`.

### P1 ✅

- JPEG encode (baseline, 4:2:0).
- PNG Adam7 + tRNS.
- GIF first frame (LZW).

### P2 ✅

- QOI encode/decode (tiny, good goldens).
- ICO (BMP/PNG payload).
- Better resize (bilinear).

### P3 ✅

- PNG 16-bit.
- GIF animation (frames, delay, disposal).
- TIFF uncompressed + PackBits.
- WebP info (+ VP8L without transforms).

### P4 ✅

- VP8L full lossless (transforms + entropy).

### P5 ✅

- Lossy VP8 keyframe (bool decoder, tokens, iDCT, intra, loop filter).

## Success criteria

**P0** — `sandak build` + `img_test`; PNG/BMP/PNM round-trip; JPEG red decodes.

**P1** — encode→decode JPEG near original; Adam7 4×4 pixels exact; tRNS alpha;
GIF 2×2 palette exact. Met.

**P2** — QOI golden + round-trip exact; ICO PNG and BMP-DIB decode; bilinear
ends stay source colors, midpoint blends. Met.

**P3** — 16-bit PNG scales; 2-frame GIF delays and colors; TIFF none/PackBits
round pixels; WebP VP8L info 2×2. Met.

**P4** — VP8L 2×2 checker, 4×4 solid red, 8×8 grid exact. Met.

**P5** — Lossy VP8 8×8 red, 2×2 gray, 8×8 quadrant corners match
ImageMagick. Met.
