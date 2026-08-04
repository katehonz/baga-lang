# std/crypto — hashes, MACs, bignum, X25519

Pure Baga cryptography: no OpenSSL FFI, deliberately — zero dependencies and
proof-of-language. SHA-256 is validated against NIST vectors, HMAC against
RFC 4231, the bignum against python-generated golden vectors (RSA-2048
modexp included), X25519 against RFC 7748.

- `sha256_bytes(data: Vec<i64>) -> Vec<i64>` — SHA-256 (FIPS 180-4) over a byte buffer; returns the 32-byte digest.
- `sha256(msg: str) -> Vec<i64>` — SHA-256 over the raw bytes of `msg`.
- `sha256_hex(msg: str) -> str` — lowercase hex digest of `msg`.
- `sha256_b(data: bytes) -> bytes` — SHA-256 over native `bytes` (binary-safe).
- `sha256_b_hex(data: bytes) -> str` — hex digest of native `bytes`.
- `hmac_sha256(key: str, msg: str) -> Vec<i64>` — HMAC-SHA256 (RFC 2104); keys longer than 64 bytes are hashed first, per spec.
- `hmac_sha256_hex(key: str, msg: str) -> str` — lowercase hex MAC.
- `hmac_sha256_b(key: bytes, msg: bytes) -> bytes` — HMAC over native `bytes`.
- `hmac_sha256_b_hex(key: bytes, msg: bytes) -> str` — hex MAC over native `bytes`.
- `ct_eq(a: str, b: str) -> bool` — constant-time string equality; time depends on length only, never on content.
- `ct_eq_bytes(a: Vec<i64>, b: Vec<i64>) -> bool` — constant-time equality over byte buffers.
- `ct_eq_b(a: bytes, b: bytes) -> bool` — constant-time equality over native `bytes`.
- `bn_*` (`bn.baga`) — fixed-width unsigned bignum (TLS milestone T1):
  26-bit limbs in `Vec<i64>`; add/sub/cmp/mul/mod/modmul/modexp plus a
  big-endian byte codec. RSA-2048 modexp with a 65537 exponent is
  milliseconds; with a 512-bit exponent ~1.5 s (schoolbook multiply +
  in-place shift-subtract mod; measured in `tests/std/bn_test.baga`, not
  hidden). Timing is secret-dependent — for signature verification
  (public exponents) only, never private keys.
- `x25519(scalar_le: bytes, u_le: bytes) -> bytes`,
  `x25519_public(scalar_le: bytes) -> bytes` (`x25519.baga`) — RFC 7748
  ECDH: clamped scalars, Montgomery ladder with constant-time cswap,
  field reduction by the 2^255 ≡ 19 fold. A full scalar multiply is
  ~0.1 s; RFC 7748 §5.2/§6.1 vectors in `tests/std/x25519_test.baga`.

Implementation notes: Baga has only signed i64, so u32 arithmetic is emulated
by masking with `& 4294967295`; bitwise NOT is `(-1) ^ x`. All intermediate
sums stay below 2^36, well inside i64 (bn.baga: below 2^59 — 79-limb
schoolbook columns). The runtime arena is bump-only (never reclaimed), so
hot loops must not allocate per iteration — that is why `bn_mod` reduces
in place instead of rebuilding shifted copies (the naive form OOM'd on a
512-bit RSA exponent).

ChaCha20-Poly1305 stays v2; TLS 1.3 is in progress —
`docs/superpowers/plans/2026-08-04-tls-client.md` (bn.baga is its T1).

Effects: none (pure). Memory: leak-tolerant.
