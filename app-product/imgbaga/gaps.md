# imgbaga — gaps

## Closed 0.1.0

- Unified RGBA8 `Image` (image-rs `DynamicImage` analogue)
- Magic-byte sniff for PNG/JPEG/GIF/BMP/WEBP/PNM/ICO/TIFF/QOI/HDR
- PNG 8-bit decode + encode via zipbaga DEFLATE + Adler-32 zlib wrapper
- Baseline JPEG decode (SOF0, Huffman, integer IDCT)
- BMP 24/32 BI_RGB; PNM P2/P3/P5/P6
- crop / flip / rotate90 / nearest resize / grayscale / invert

## Closed 0.2.0

- JPEG encode — baseline 4:2:0, Annex K Huffman, quality 85 (`jpeg_encode_q`)
- PNG Adam7 + tRNS (gray / RGB / palette); packed 1/2/4-bit gray & palette
- GIF first frame — LZW, GCE transparency, interlace

## Closed 0.3.0

- QOI encode/decode (qoiformat.org; lossless RGBA)
- ICO decode (PNG payload or 24/32-bit DIB + AND mask); encode as PNG-in-ICO
- `img_resize_bilinear` (integer 8.8 lerp); `img_resize` stays nearest

## Closed 0.4.0

- PNG 16-bit (gray/RGB/grayA/RGBA + tRNS) scaled to RGBA8 (`v/257`)
- GIF animation: `gif_anim_decode` / frames / delay / disposal 2–3
- TIFF: uncompressed + PackBits, 8-bit gray/RGB/RGBA, II/MM
- WebP: `webp_info` for VP8 / VP8L / VP8X

## Closed 0.5.0

- VP8L lossless decode: Huffman, LZ77, color cache, predictor / color /
  subtract-green / color-indexing transforms (ImageMagick lossless WebP).

## Closed 0.6.0

- Lossy VP8 keyframe decode (bool coder, tokens, iDCT/WHT, intra, loop
  filter). RGB via libwebp BT.601. Inter frames / VP8X alpha still out.

## Open

### I1 — ~~no JPEG encode~~ — closed 0.2.0

### I2 — ~~16-bit PNG~~ — closed 0.4.0

### I3 — ~~lossy VP8 (WebP)~~ — closed 0.6.0

Keyframe stills done. Not in v1: inter frames (video), VP8X alpha,
libwebp fancy chroma upsample (nearest 4:2:0; edges can differ from
`dwebp`).

### I4 — whole image in memory

Same as csvbaga C1. Fine for UI thumbs and tests; streaming decode later.

### I5 — 4096 px cap

Defensive v1 limit (arena). Raise when a product needs posters.
