# kvbaga — language & protocol gaps

Probe log from building the first product on the `Map<K,V>` type.
Same shape as pgbaga/httpdbaga/jwtbaga/fmrbaga.

## K1 — `go()` cannot carry a map/store to a worker

**Symptom.** `go(f, x)` passes one `i64`; the store is a `KvStore` (map
pointers). A second thread cannot receive the store, so the server is serial
(one connection at a time).

**Workaround (P0).** Redis-1.x model: serial accept loop; idle connections
bounded by `SO_RCVTIMEO`.

**Severity.** High for throughput; correctness is fine.

**Path open (2026-08-04, chatbaga).** `std/net/poll.baga` is the designated
fix: one thread, many fds, store owned by the loop — chatbaga ships this
for WebSocket rooms. kvbaga can migrate `kv_serve` to the same pattern
without further language changes. (Still serial *today*; not closed until
kvbaga adopts poll.)

## K2 — ~~`str` is NUL-terminated → no binary values~~ — language path CLOSED

**Symptom.** RESP bulk strings are length-prefixed and binary-safe; Baga `str`
is not. Values with NUL cannot round-trip through a `Map<str,str>` store.

**Closed at language level (2026-08-04, chatbaga).** `Map` values may be
`bytes` (checker + C runtime); residual buffers in chat use
`Map<i64, bytes>`. kvbaga still stores `Map<str,str>` text — migrating the
store to `Map<str, bytes>` is pure product work (P2).

## K3 — ~~C wrapper swallows `main`'s exit code~~ — CLOSED

**Symptom.** `fn main() -> i64 { return 1 }` still exits 0: the emitted C
`main` called `b_main(); return 0;`.

**Closed (2026-08-04, iteration №2).** The wrapper now emits
`return (int)b_main();` when `main` is declared `-> i64`/`-> i32`
(void mains keep `return 0`). The baga CLI already propagated
`WEXITSTATUS`, so the code now travels end to end. Regression check in
`make test` ("main -> i64 връща exit кода на процеса").

## K4 — KEYS without glob; DBSIZE walks all keys

**Symptom.** No pattern matching (`KEYS user:*`); both commands do a full
map walk + liveness check.

**Workaround.** Fine at probe scale.

**Severity.** Low.

**Verdict.** Glob matching is app-level (std/str probe); a live-count field
would need map metadata.

## K5 — No glob/pattern type anywhere in std

**Symptom.** `str_find`/`str_starts_with` only; no `fnmatch`-style matcher
for KEYS, routing, or file globs.

**Verdict.** Candidate for `std/str` (small, broadly useful).

## Closed / fine

- `Map<str,str>` + `Map<str,i64>` carry the whole store — zero workarounds
  needed (the point of the exercise).
- Struct holding maps passes by value but shares the maps — exactly the
  mutate-through semantics a server wants (contrast: PgConn by-value churn).
- RESP parse/encode is clean with the `bytes` API (no NUL traps in framing).
- Lazy TTL expiry works with `monotonic_ms` (std/time).
