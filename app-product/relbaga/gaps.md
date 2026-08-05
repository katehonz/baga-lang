# relbaga — gaps

## R1 — no automatic jitter

Backoff is deterministic. Add `base + (attempt * seed) % span` if needed.

## R2 — breaker not shared across threads without mutex

Maps/`Breaker` struct must be owned by one loop or guarded by `mutex`.

## R3 — retry only for `fn(i64)->i64`

No effect polymorphism on the op beyond pure/!Par; IO ops wrap into i64 codes.
