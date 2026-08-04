# TLS 1.3 client in std (G6) Implementation Plan

> Progress: T1–T8 complete (2026-08-04). https:// live against openssl mock.

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

Status: T1–T8 done (2026-08-04). G6 client closed via openssl mock (no real
OAuth account).

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

### T7 — ECDSA-P256 verify + cipher hardening ✅
- `std/crypto/p256.baga`: affine Weierstrass over bn (10 limbs), on-curve
  check, scalar mult, `ecdsa_p256_verify_sha256` (SEC1 §4.1.4, DER r||s).
- `x509.baga`: EC SPKI (id-ecPublicKey + prime256v1 uncompressed point)
  and ecdsa-with-SHA256 cert signatures.
- `tls_verify_server`: accepts `ecdsa_secp256r1_sha256` (0x0403) as well
  as RSA-PSS (0x0804).
- Tests: `tests/std/p256_test.baga` (~9 s) + live openssl dual peer
  (RSA-PSS and ECDSA-P256) in `scripts/run_tests.sh`.
- Cipher note: ClientHello still offers AES_256_GCM_SHA384 second, but
  the suite needs SHA-384 HKDF — servers pick AES_128 first; full 256
  stays deferred. HelloRetryRequest not observed with x25519-only CH.

### T8 — `https://` in http_client + mock proof ✅
- App traffic secrets (`c ap traffic` / `s ap traffic`) after handshake;
  `TlsConn` seal/open; `tls_connect` / `tls_handshake`.
- **Client Finished transcript bug fixed:** verify_data must cover the
  server Finished (RFC 8446 §4.4.4). The T5 live check only looked at
  outer record type ≠ 21 and missed encrypted `decrypt_error` alerts.
- `http_parse_url` / `http_request` accept `https://` (default port 443);
  empty trust anchor accepts self-signed (dev/mock).
- Capstone without a real account: `tests/std/https_test.baga` against
  `openssl s_server -tls1_3 -www` (self-signed). oauthbaga O1 client half
  closed; serving oauth over TLS is a separate server-side task.
- SNI omitted for dotted IPv4 hosts (servers often reject IP names).

## Out of scope (written so nobody forgets)

- Server-side TLS, client certificates, session tickets/resumption,
  0-RTT, certificate revocation (OCSP/CRL), system trust stores,
  constant-time bignum, TLS ≤ 1.2 fallback, ChaCha20-Poly1305.
