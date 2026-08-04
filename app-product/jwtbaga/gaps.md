# jwtbaga — language gaps found while building

Second probe of the language (after `../httpdbaga/gaps.md`). Numbering
continues from httpdbaga (G1–G4). Each entry: symptom → repro → workaround →
severity → verdict (roadmap / YAGNI / app-specific).

---

## G5 — `str` cannot represent binary data (no null byte; no `bytes_to_str`)

**Symptom.** Baga strings are C strings: they cannot hold byte 0, and there is
no `Vec<i64> -> str` conversion. Any binary payload (a 32-byte HMAC signature,
a decrypted block, a hash) cannot travel as a `str`.

**Evidence (probe).**
```
chr(0)                          -> ""            (len 0, not a 1-byte NUL)
concat("A", chr(0), "B")        -> "AB"          (len 2 — the NUL is dropped)
base64 round-trip of [65,0,66]  -> [65,0,66]     (exact, via Vec<i64>)
```
A decoded JWT signature is 32 arbitrary bytes and frequently contains 0x00, so
it *cannot* be a `str`.

**Workaround.** Keep binary data as `Vec<i64>` end to end; compare signatures
with `ct_eq_bytes(Vec<i64>, Vec<i64>)`; convert to `str` only for genuinely
textual data (JSON payloads) via a local `bytes_to_str` (chr per byte — safe
only because JSON has no NUL). See `jwt.baga`.

**Severity.** Medium-high. Every crypto/compression/serialization consumer hits
this; the `Vec<i64>` plumbing is verbose and the `str`/`bytes` boundary is
invisible in types (a `bytes_to_str` on signature bytes would silently corrupt).

**Verdict.** **Closed** (core type + crypto + jwt). First-class `bytes`;
crypto `sha256_b` / `hmac_sha256_b` / `ct_eq_b`; jwt signs/verifies via
native `bytes` (no `Vec` MAC path).

## G6 — base64url (RFC 4648 §5) absent from std/bytes

**Symptom.** std/bytes has `base64_encode`/`base64_decode` with the **standard**
alphabet (`+`,`/`) and `=` padding only. JWT (and many web formats) need the
URL-safe alphabet (`-`,`_`, no padding).

**Evidence.** `base64_encode([251,255,191]) == "+/+/";` JWT requires `-_-_`.
`jwt.baga` implements `base64url_encode`/`base64url_decode` as a transform over
std base64.

**Workaround.** Translate `+`→`-`, `/`→`_`, strip/re-pad `=` (in `jwt.baga`).

**Severity.** Low-medium. Recurs for any JWT/JWS/URL-token consumer.

**Verdict.** **Closed.** `base64url_encode` / `base64url_decode` live in
`std/bytes/bytes.baga`; the local copies in `jwt.baga` were deleted.

## G7 — no query-string parsing; `http_path` includes `?query`

**Symptom.** `httpdbaga` `http_path` returns the raw target including the query
string; there is no std/app helper to read a parameter. (Cross-ref httpdbaga.)

**Evidence.** `server.baga` `/token?sub=NAME` needed a hand-written
`query_param(path, name)` (split on `?`, then `&`, then `name=` prefix).

**Workaround.** Local `query_param` helper in `server.baga` (was).

**Severity.** Low-medium. Every web app rewrites this.

**Verdict.** **Closed.** `http_query_param` / `query_param` / `http_path_only`
live in `httpdbaga/http.baga`; jwt server uses them.

## G8 — `http.baga` reason-phrase table was incomplete (401)

**Symptom.** First use of a 401 response rendered `HTTP/1.1 401 Status` —
`reason_phrase` only knew 200/400/404/405/500.

**Evidence.** jwtbaga demo `/protected` without a token (curl `-i`).

**Workaround/Fix.** Added `401 -> "Unauthorized"` to `http.baga` (a library
completion, not a compiler change). The table is still finite — any code not in
it falls back to "Status".

**Severity.** Low.

**Verdict.** App-specific (fixed in-library). A data-driven table or a fallback
that always emits a valid reason would be a minor httpdbaga improvement.

## G9 — `jwt_claim` returns quoted strings; no typed accessors

**Symptom.** `jwt_claim(tok, "sub")` returns `"bagatur"` *with* the JSON quotes
(it is raw JSON text); the server needs an `unquote` helper. Number claims come
back bare (`7`). The caller must know each claim's type.

**Evidence.** `server.baga` `unquote`; `tests/jwt_test.baga` asserts
`claim_sub == "\"bagatur\""` and `claim_n == "7"`.

**Workaround.** `unquote` for strings; the raw text is fine for numbers.

**Severity.** Low.

**Verdict.** **Closed.** `jwt_claim_str` / `jwt_claim_int` in `jwt.baga`.

---

## G10 — RS256/ES256 verify (closed with TLS crypto)

**Symptom (historical).** README said RS256/ES256 blocked on missing bignum.

**Closed (2026-08-04).** After TLS T6/T7: `jwt_verify_rs256` /
`jwt_verify_es256` use `rsa` + `p256`. Signing still HS256-only (no private
key API).

## G11 — private key sign for RS256/ES256

**Symptom.** Cannot *issue* RS256/ES256 tokens; only verify.

**Workaround.** Issue with HS256 or an external IdP; verify provider tokens
with RS256/ES256.

**Severity.** Medium for a full auth server; none for resource servers.

**Verdict.** Future: RSA/ECDSA sign + PEM/JWK loaders.

---

## Triage summary

| Gap | Verdict |
|-----|---------|
| G5–G9 | **closed** |
| G10 RS256/ES256 verify | **closed** (TLS crypto) |
| G11 asymmetric sign | open (optional) |
