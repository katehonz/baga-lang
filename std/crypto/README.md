# std/crypto — hashes, MACs, bignum, X25519, P-256, HKDF, AES-GCM, RSA, X.509

Pure Baga cryptography: no OpenSSL FFI, deliberately — zero dependencies and
proof-of-language. SHA-256 is validated against NIST vectors, HMAC against
RFC 4231, the bignum against python-generated golden vectors (RSA-2048
modexp included), X25519 against RFC 7748, P-256/ECDSA against python
`cryptography`, HKDF against RFC 5869, AES against FIPS-197 and AES-GCM
against the same oracle, RSA-PSS / PKCS#1 likewise. The TLS 1.3 stack
(T1–T7) lives here plus `std/net/tls.baga`.

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
- `hkdf_extract(salt, ikm) -> bytes`, `hkdf_expand(prk, info, len) -> bytes`
  (`hkdf.baga`) — RFC 5869 over hmac_sha256_b; Appendix A test cases 1–3.
- `aes_expand(key) -> AesKey`, `aes_encrypt(k, block) -> bytes`
  (`aes.baga`) — AES-128/256 forward cipher (FIPS-197 C.1/C.3); the
  S-box is computed from GF(2^8) inversion, no literal table.
- `gcm_seal(k, nonce12, aad, pt) -> bytes` (ct‖tag),
  `gcm_open(k, nonce12, aad, ct_tag) -> GcmOpen { ok, pt }` (`gcm.baga`)
  — AES-GCM AEAD (SP 800-38D) with GHASH in GF(2^128); 12-byte nonces
  (the TLS 1.3 shape); tags compared with ct_eq_b. Vectors generated
  offline with python `cryptography`; the whole suite runs in ~0.5 s.
- `der_*` (`der.baga`) — minimal DER walker for X.509 (TLS T6).
- `rsa_pkcs1_sha256_verify` / `rsa_pss_sha256_verify` (`rsa.baga`) —
  RSA public verify for cert signatures (PKCS#1 v1.5) and TLS 1.3
  CertificateVerify (EMSA-PSS, sLen=32). Vectors in
  `tests/std/rsa_pss_test.baga`.
- `x509_parse` / `x509_verify_self_signed` / `x509_trust_anchor`
  (`x509.baga`) — Certificate → RSA SPKI + TBS; sha256WithRSAEncryption
  only. No name constraints, time checks, or revocation.

Implementation notes: Baga has only signed i64, so u32 arithmetic is emulated
by masking with `& 4294967295`; bitwise NOT is `(-1) ^ x`. All intermediate
sums stay below 2^36, well inside i64 (bn.baga: below 2^59 — 79-limb
schoolbook columns). The runtime arena is bump-only (never reclaimed), so
hot loops must not allocate per iteration — that is why `bn_mod` reduces
in place instead of rebuilding shifted copies (the naive form OOM'd on a
512-bit RSA exponent).

ChaCha20-Poly1305 stays v2; TLS 1.3 T1–T6 done, T7 ECDSA next —
`docs/superpowers/plans/2026-08-04-tls-client.md`.

Effects: none (pure). Memory: leak-tolerant.
