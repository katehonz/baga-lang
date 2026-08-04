# TLS 1.3 client in std (G6) Implementation Plan

> Progress: T1 bn ✓ · T2 x25519 ✓ · T3 hkdf/aes/gcm ✓ · T4 records+hello ✓
> · T5 encrypted handshake ✓ · T6 X.509 + RSA-PSS ✓ (live openssl) · T7–T8 ahead

**Goal:** Close gap G6 — the last production blocker found by the apps
roadmap (№10 OAuth proxy waits on it): a **TLS 1.3 client** in `std/`,
pure Baga, wired into `std/net/http_client.baga` so `https://` works
end-to-end against real servers.

**Lineage:** oauthbaga O1 (TLS blocks real OAuth); №2 shaped the client
API so TLS slots in without interface change; std/crypto README reserved
"ChaCha20-Poly1305 and TLS are v2" — this is that v2.

## Global Constraints

- **No compiler changes** unless a probe forces one (then: fix + regression
  in make test, as with tplbaga P5).
- Pure Baga on builtins + `std/`. No OpenSSL FFI at runtime — openssl is a
  **test peer only** (`openssl s_server`, real hosts) and the vector oracle
  (python3) is offline.
- Signed-i64 discipline (std/crypto README): 26-bit limbs for bignum, all
  intermediate sums proven below 2^63.
- Honest limits are written, not hidden: no client certificates, no
  session resumption/0-RTT, no revocation checks, trust anchors are
  supplied by the caller (no system store), timing side-channels in the
  bignum are out of scope for a probe.
- Every task ends green (make test + make self) before the next starts;
  each task is one commit.

## Scope decisions (TLS 1.3, RFC 8446)

- Key exchange: **X25519 only** (every real server offers it; avoids
  P-256 ECDH). Certificate signatures: **RSA-PSS first** (modexp over the
  bignum), **ECDSA-P256 second** (many real certs are ECDSA — the client
  is not done without it, T7).
- Cipher: **TLS_AES_128_GCM_SHA256** (+ TLS_AES_256_GCM_SHA256 — same
  code, key size apart). No ChaCha20-Poly1305 this milestone.
- Handshake: ClientHello (supported_versions, key_share x25519,
  signature_algorithms rsa_pss_rsae_sha256 + ecdsa_secp256r1_sha256) →
  ServerHello → {EncryptedExtensions, Certificate, CertificateVerify,
  Finished} → client Finished → application data (GET/POST via the
  existing http_request shape).
- Record layer: 16 KB max fragments, handshake reassembly,
  change-of-key detection, close_notify.

## Tasks

Status: T1–T6 done (2026-08-04). Next: T7 ECDSA-P256.

### T1 — `std/crypto/bn.baga`: fixed-width bignum
- 26-bit limbs in `Vec<i64>`, little-endian, fixed length per op.
- `bn_zero/bn_from_bytes_be/bn_to_bytes_be/bn_cmp/bn_add/bn_sub/bn_mul`
  (schoolbook, 2n limbs) `/bn_mod` (shift-subtract division) /
  `bn_modmul/bn_modexp` (square-and-multiply, L→R).
- Tests first: `tests/std/bn_test.baga` with python-generated golden
  vectors (small cases + 256-bit mul/mod + 2048-bit modexp). RSA-2048
  modexp may take ~seconds — measured and documented, not hidden.
- **Verify:** bn_test all passed; in the make test std loop.

### T2 — `std/crypto/x25519.baga` (RFC 7748)
- Field arithmetic mod 2^255-19 on top of bn (mul + fold-by-19
  reduction), Montgomery ladder, clamped scalars, u-coordinate only.
- Tests: RFC 7748 §5.2 vector (iter 1) + §6.1 Diffie–Hellman vector.

### T3 — `std/crypto/hkdf.baga` (RFC 5869) + AES-GCM
- HKDF-Extract/Expand over the existing hmac_sha256 (RFC 5869 vectors).
- `std/crypto/aes.baga`: AES-128/256 block (FIPS 197 vectors), then
  `std/crypto/gcm.baga`: CTR + GHASH (NIST GCM test vectors).

### T4 — TLS record layer + plaintext handshake parse
- `std/net/tls.baga`: record framing, ClientHello builder with real
  random + x25519 key_share; parse ServerHello. Test peer:
  `openssl s_server -tls1_3` on loopback (handshake up to ServerHello).

### T5 — Key schedule + encrypted handshake
- HKDF labels (derived/handshake/traffic), ECDHE shared secret →
  handshake keys; decrypt EncryptedExtensions/Certificate/
  CertificateVerify/Finished; send client Finished. Verify against
  openssl s_server with a self-signed cert.

### T6 — X.509: DER/ASN.1 + RSA-PSS chain verify ✅
- `std/crypto/der.baga` — minimal DER walker (SEQUENCE/OID/INTEGER/BIT STRING).
- `std/crypto/rsa.baga` — `rsa_pkcs1_sha256_verify` (cert signatures) +
  `rsa_pss_sha256_verify` (EMSA-PSS, sLen=32, MGF1-SHA256).
- `std/crypto/x509.baga` — parse Certificate → SPKI RSA n/e + TBS + sig;
  self-signed and trust-anchor checks (sha256WithRSAEncryption).
- `tls_verify_server(flight, anchor_der)` — leaf trust + CertificateVerify
  (`rsa_pss_rsae_sha256`); empty anchor accepts self-signed (dev peer).
- Tests: `tests/std/rsa_pss_test.baga` (python cryptography vectors) +
  live openssl path in `tls_handshake_test` (cert + CV both green).
- Honest gaps: name constraints/policies/time/revocation ignored; ECDSA
  leaves are T7.

### T7 — ECDSA-P256 verify + cipher hardening
- P-256 field/scalar arithmetic, point verify per RFC 5480/SEC1;
  TLS_AES_256_GCM; HelloRetryRequest if seen in the wild.

### T8 — `https://` in http_client + the OAuth proof
- `http_request` grows `https://` (TLS connect → request → read to
  close_notify/EOF); redirect chasing unchanged.
- Capstone: oauthbaga variant talking to a real provider over https
  (or an openssl-served https mock), closing №10 P2 and G6.
  CHANGELOG + README + gaps updates; idea/apps-10.md status final.

## Out of scope (written so nobody forgets)

- Server-side TLS, client certificates, session tickets/resumption,
  0-RTT, certificate revocation (OCSP/CRL), system trust stores,
  constant-time bignum, TLS ≤ 1.2 fallback, ChaCha20-Poly1305.
