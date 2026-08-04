# jwtbaga

JWT / JWS (RFC 7519 / 7518) for Baga — pure library + small demo HTTP server.

Built on the full TLS crypto stack:

| Alg | Sign | Verify | Backend |
|-----|------|--------|---------|
| **HS256** | ✅ | ✅ | `hmac_sha256_b` |
| **RS256** | — | ✅ | `rsa_pkcs1_sha256_verify` (bn) |
| **ES256** | — | ✅ | `ecdsa_p256_verify_sha256_raw` |
| `none` | never | **rejected** | — |

Asymmetric *signing* needs private-key ops (not exposed yet). **Verify** of
RS256/ES256 is what OIDC / API gateways need when tokens come from a provider.

## API

```baga
// --- HS256 (issue + accept) ---
fn jwt_encode(key, payload_json) -> str          // always alg=HS256
fn jwt_verify_hs256(key, token) -> i64           // checks header.alg
fn jwt_decode(key, token) -> str                 // payload or ""
fn jwt_accept_hs256(key, token, now_s, iss, aud) -> i64  // + exp/nbf/iss/aud

// --- discover / claims (read after verify) ---
fn jwt_alg(token) -> str
fn jwt_claim / jwt_claim_str / jwt_claim_int
fn jwt_time_ok(token, now_s) -> i64              // exp / nbf
fn jwt_iss_ok / jwt_aud_ok

// --- RS256 / ES256 verify only ---
fn jwt_verify_rs256(n: bytes, e: bytes, token) -> i64
fn jwt_decode_rs256(n, e, token) -> str
fn jwt_verify_es256(qx: bytes, qy: bytes, token) -> i64   // P-256 coords
fn jwt_decode_es256(qx, qy, token) -> str
```

ES256 signatures use the JWS raw **R‖S** (64 bytes), not DER.

## Demo server

```bash
PORT=8080 JWT_SECRET=s3cret ./baga app-product/jwtbaga/server.baga
curl -s 'localhost:8080/token?sub=bagatur'
curl -H "Authorization: Bearer $TOKEN" localhost:8080/protected
```

## Test

```bash
./baga -I . -I app-product tests/jwt_test.baga
# HS256 golden, alg:none reject, exp, RS256 + ES256 vectors (python oracle)
```

## Effects

`jwt.baga` is **pure**. Effects only in `server.baga` (`!IO !Net !Par`).
