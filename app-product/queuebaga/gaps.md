# queuebaga — language & product gaps

Probe log from apps-roadmap **№5** (task queue).

## Q1 — `go`/`chan` carry only `i64`

**Symptom.** Workers cannot receive a Map/store or string payload. Shared
mutable job state cannot cross `go` without an out-of-band medium.

**Workaround.** Jobs on disk; channel only carries ids. Active prefix in
`/tmp/baga_queue_active` (no `setenv`).

**Severity.** High for in-memory brokers; defines the architecture.

**Verdict.** Same family as K1. Mitigations: (a) disk/DB as store (this
package), (b) `chan` of `str`/`bytes`, (c) function values + shared heap
with mutex. (a) is production-honest for durable queues.

## Q2 — no `setenv` / env mutation

**Symptom.** Parent cannot set `QUEUE_DIR` for workers; only `env` read.

**Workaround.** Side file `/tmp/baga_queue_active` written before `go_bg`.

**Severity.** Low–medium.

**Verdict.** `setenv` in std/os when multiple CLIs need it.

## Q3 — `write_file` truncate race

**Symptom.** `open(O_TRUNC)` then write — concurrent `read_file` can see
`""` mid-update. `q_wait` used to return `missing` spuriously.

**Workaround.** Treat `""` as transient in `q_wait` until timeout.

**Severity.** Medium for multi-thread file status.

**Verdict.** Write-temp + `rename` when rename exists; or record format
with length prefix. Logged.

## Q4 — ~~no `mkdir`~~ — UNBLOCKED 2026-08-05 (lsmbaga / std/os)

`mkdir` / `unlink` / `link` + `fs_rename` land in `std/os` with the
lsmbaga MVP. queuebaga can migrate off flat prefixes when desired.

## Q4 (historical) — no `mkdir`

**Symptom.** Cannot create job directories; flat prefix paths only.

**Workaround.** `<prefix>.<id>.job` under `/tmp`.

**Severity.** Low.

**Verdict.** `mkdir` extern in std/os (trivial).

## Q5 — ~~no function-value job handlers (L5)~~ — closed (in-process)

**Shipped 2026-08-14.** `q_process_fn(prefix, id, max, handler)` takes
`fn(str) -> str`. Same `fail:` retry convention. `go_bg` workers still
use the built-in reverse/`fail:` `q_process_one` (chan is i64-only — Q1).

## Closed / fine

- `mutex` for id allocation works.
- `chan_recv2` + close wakes workers cleanly enough for tests.
- Retry/backoff with `sleep_ms` is expressible.
