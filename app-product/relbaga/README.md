# relbaga

**Resilience primitives** for Baga services (Track **C6**).

| Tool | API |
|------|-----|
| Exponential backoff | `rel_backoff_ms(attempt, base, max, mult)` |
| Retry loop | `rel_retry<T>(op, arg, max_attempts, base, max, mult)` — `op` returns 0 on success |
| Jittered backoff | `rel_backoff_jitter_ms` / `rel_retry_jitter` — deterministic `[d/2, d]` |
| Circuit breaker | `brk_new` / `brk_tick` / `brk_allow` / `brk_on_start` / `brk_success` / `brk_failure` |
| Bulkhead | `bh_new(cap)` / `bh_acquire` / `bh_try_acquire` / `bh_release` (chan tokens) |

```baga
// retry
fn flaky(x: i64) -> i64 { /* 0 ok */ }
rel_retry(flaky, 0, 5, 10, 200, 2)?

// breaker
let mut b = brk_new(3, 1000)
b = brk_tick(b, now)
if brk_allow(b, now) == 1 {
    b = brk_on_start(b, now)
    // … call …
    b = brk_success(b, now)   // or brk_failure
}

// bulkhead
let bh = bh_new(4)?
bh_acquire(bh)?
// … limited work …
bh_release(bh)?
```

Honest: jitter is opt-in and deterministic (seeded); breaker is
single-threaded state (caller serializes); bulkhead is process-local.

## License

[MIT](LICENSE) — Copyright (c) 2026 Dim Gigov.
