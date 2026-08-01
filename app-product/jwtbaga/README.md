# jwtbaga

A pure JWT (RFC 7519) **HS256** signer + verifier for Baga, built only on
`std/` (`crypto/hmac`, `crypto/ct`, `bytes`, `json`, `str`). The demo server is
the first consumer of [`../httpdbaga/http.baga`](../httpdbaga/README.md).

HS256 only. `alg:none` is never produced and never accepted. Asymmetric
algorithms (RS256/ES256) need bignum crypto that `std/` does not have yet.

This product is also a probe of the language — see [`gaps.md`](gaps.md).

## Files

| File | What |
|------|------|
| `jwt.baga` | the library: base64url, sign/verify, encode/decode/claim (all pure) |
| `server.baga` | demo auth server (issue / verify / protected) |
| `gaps.md` | language gaps found, with evidence and triage |

## API (all pure)

```baga
fn base64url_encode(b: Vec<i64>) -> str        // RFC 4648 §5, no padding
fn base64url_decode(s: str) -> Vec<i64>        // tolerant of missing padding

fn jwt_sign(key: str, msg: str) -> str         // base64url(HMAC-SHA256(key,msg))
fn jwt_verify(key: str, msg: str, sig: str) -> bool   // constant-time

fn jwt_encode(key: str, payload_json: str) -> str     // header.payload.signature
fn jwt_decode(key: str, token: str) -> str            // payload JSON; "" if invalid
fn jwt_claim(token: str, name: str) -> str            // raw claim text; "" if absent
```

**Binary-safety note.** A decoded signature is 32 arbitrary bytes and may
contain `0x00`, which a Baga `str` cannot hold (`chr(0) == ""`). So signatures
stay `Vec<i64>` and are compared with `ct_eq_bytes` — never coerced to `str`.
`jwt_decode` returns the payload as a `str` safely because JSON text has no null
bytes. See `gaps.md` G5.

## Run the demo

```bash
./baga --emit-c app-product/jwtbaga/server.baga > /tmp/jwtd.c
gcc -O2 -Iinclude -o /tmp/jwtd /tmp/jwtd.c -lm
PORT=8080 JWT_SECRET=s3cret /tmp/jwtd &

# 1. issue a token
TOKEN=$(curl -s 'localhost:8080/token?sub=bagatur' | sed 's/.*"token":"//; s/".*//')

# 2. use it
curl -H "Authorization: Bearer $TOKEN" localhost:8080/verify      # {"valid":true,"sub":"bagatur"}
curl -H "Authorization: Bearer $TOKEN" localhost:8080/protected   # {"secret":"...","sub":"bagatur"}
curl -i localhost:8080/protected                                   # 401 Unauthorized
```

## Test

```bash
./baga tests/jwt_test.baga    # golden vector (Python-cross-checked), tamper, wrong key, claims
```

Also wired into `make test`. The golden token in the test was generated
independently with Python `hmac`/`hashlib`/`base64` and matches byte-for-byte.

## Effects

Everything in `jwt.baga` is **pure** — visible purity in the type. Effects
(`!IO !Net`) appear only in `server.baga`, via http/tcp.
