# relbaga — gaps

## R1 — ~~no automatic jitter~~ — closed

`rel_backoff_jitter_ms` / `rel_retry_jitter` — deterministic jitter in
`[d - d/2, d]`, mixed from `seed + attempt`. Optional; the plain
`rel_backoff_ms` path stays exact.

## R2 — breaker not shared across threads without mutex

Maps/`Breaker` struct must be owned by one loop or guarded by `mutex`.

## R3 — ~~retry only for `fn(i64)->i64`~~ — closed (M21)

`rel_retry<T>` / `rel_retry_jitter<T>` take `fn(T) -> i64`. The op still
returns 0 on success (no effect polymorphism on the failure itself).
