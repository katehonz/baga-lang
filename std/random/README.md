# std/random — getrandom(2) wrappers

- `extern getrandom(buf: str, n: i64, flags: i64) -> i64 !Random` — libc `getrandom(2)` into a heap buffer.
- `random_bytes(n: i64) -> Vec<i64> !Random` — `n` random bytes as a Vec (values 0..255).
- `random_i64() -> i64 !Random` — 8 random bytes as a little-endian i64; 0 if the read comes up short.

Buffer contract: like std/os, the extern writes into the passed `str`
buffer — it must be heap-allocated (`str_repeat`), never a literal.

Effects: !Random. Memory: buffers are heap strings (leak-tolerant); no arena needed.
