# std/crypto — hashes and MACs

Pure Baga cryptography: no OpenSSL FFI, deliberately — zero dependencies and
proof-of-language. SHA-256 is validated against NIST vectors, HMAC against
RFC 4231.

- `sha256_bytes(data: Vec<i64>) -> Vec<i64>` — SHA-256 (FIPS 180-4) over a byte buffer; returns the 32-byte digest.
- `sha256(msg: str) -> Vec<i64>` — SHA-256 over the raw bytes of `msg`.
- `sha256_hex(msg: str) -> str` — lowercase hex digest of `msg`.
- `hmac_sha256(key: str, msg: str) -> Vec<i64>` — HMAC-SHA256 (RFC 2104); keys longer than 64 bytes are hashed first, per spec.
- `hmac_sha256_hex(key: str, msg: str) -> str` — lowercase hex MAC.
- `ct_eq(a: str, b: str) -> bool` — constant-time string equality; time depends on length only, never on content.
- `ct_eq_bytes(a: Vec<i64>, b: Vec<i64>) -> bool` — constant-time equality over byte buffers.

Implementation notes: Baga has only signed i64, so u32 arithmetic is emulated
by masking with `& 4294967295`; bitwise NOT is `(-1) ^ x`. All intermediate
sums stay below 2^36, well inside i64.

ChaCha20-Poly1305 and TLS are explicitly v2.

Effects: none (pure). Memory: leak-tolerant.
