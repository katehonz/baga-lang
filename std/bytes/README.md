# std/bytes — byte buffers

A byte buffer is a `Vec<i64>` holding values in `0..255`. Includes hex and
Base64 codecs (RFC 4648 standard alphabet with padding, and URL-safe §5).

- `bytes_from_str(s: str) -> Vec<i64>` — byte buffer holding the raw bytes of `s`.
- `bytes_eq(a: Vec<i64>, b: Vec<i64>) -> bool` — true if both buffers have equal length and contents.
- `bytes_to_hex(b: Vec<i64>) -> str` — lowercase hex string, two digits per byte.
- `bytes_from_hex(h: str) -> Vec<i64>` — decodes pairs of hex digits; a trailing half-pair is ignored.
- `base64_encode(b: Vec<i64>) -> str` — RFC 4648 Base64 encoding with `=` padding (`+`, `/`).
- `base64_decode(s: str) -> Vec<i64>` — decodes padded Base64; input length must be a multiple of 4.
- `base64url_encode(b: Vec<i64>) -> str` — RFC 4648 §5 URL-safe alphabet (`-`, `_`), no padding.
- `base64url_decode(s: str) -> Vec<i64>` — decodes base64url; missing padding is tolerated.
- `base64url_encode_b(b: bytes) -> str` / `base64url_decode_b(s: str) -> bytes` — native `bytes` wrappers.
- Language builtins (no import): native `bytes` type plus `bytes_from_vec` /
  `vec_from_bytes` bridges for crypto that still uses `Vec<i64>` internally.
- `bytes_eq` is exported; `hex_digit`, `hex_val`, `b64_char`, `b64_val` are internal helpers.

Effects: none (pure). Memory: leak-tolerant.
