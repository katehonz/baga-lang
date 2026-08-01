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

**Verdict.** Roadmap candidate. Two directions: (a) a distinct `bytes` type
(length-prefixed, binary-safe) with explicit conversions, or (b) make `str`
length-prefixed internally so `chr(0)` is a real byte. (a) is the cleaner, safer
design; (b) is the smaller change but breaks the C-FFI `char*` contract that
`extern fn` relies on.

## G6 — base64url (RFC 4648 §5) absent from std/bytes

**Symptom.** std/bytes has `base64_encode`/`base64_decode` with the **standard**
alphabet (`+`,`/`) and `=` padding only. JWT (and many web formats) need the
URL-safe alphabet (`-`,`_`, no padding).

**Evidence.** `base64_encode([251,255,191]) == "+/+/";` JWT requires `-_-_`.
`jwt.baga` implements `base64url_encode`/`base64url_decode` as a transform over
std base64.

**Workaround.** Translate `+`→`-`, `/`→`_`, strip/re-pad `=` (in `jwt.baga`).

**Severity.** Low-medium. Recurs for any JWT/JWS/URL-token consumer.

**Verdict.** Roadmap candidate (small, std): add `base64url_encode`/
`base64url_decode` to `std/bytes/bytes.baga`, then delete the copies here.

## G7 — no query-string parsing; `http_path` includes `?query`

**Symptom.** `httpdbaga` `http_path` returns the raw target including the query
string; there is no std/app helper to read a parameter. (Cross-ref httpdbaga.)

**Evidence.** `server.baga` `/token?sub=NAME` needed a hand-written
`query_param(path, name)` (split on `?`, then `&`, then `name=` prefix).

**Workaround.** Local `query_param` helper in `server.baga`.

**Severity.** Low-medium. Every web app rewrites this.

**Verdict.** Roadmap candidate (small): a `query_param` in httpdbaga (or a
`std/url`), decided when a second consumer appears.

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

**Verdict.** Roadmap candidate (small, this library): add `jwt_claim_str`
(decoded, unquoted) and `jwt_claim_int` convenience accessors when a second
consumer wants them. Not done now (YAGNI for one demo).

---

## Triage summary (end of milestone)

| Gap | Verdict | Next step |
|-----|---------|-----------|
| G5 binary-safe str / bytes type | **roadmap (design)** | spec a `bytes` type vs length-prefixed `str` |
| G6 base64url in std/bytes | **roadmap (small)** | add to std, delete the copy here |
| G7 query-string parsing | **roadmap (small)** | `query_param` in httpdbaga on 2nd consumer |
| G8 incomplete reason table | app-specific | fixed in http.baga (401 added) |
| G9 typed claim accessors | roadmap (small) / YAGNI-now | add on 2nd consumer |
