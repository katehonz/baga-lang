# Operations

## Process model

One OS process, one listen socket per binary:

| Binary | Default port | Protocol |
|--------|--------------|----------|
| `tools/serve.baga` | 6570 | HTTP/1.1 + JSON |
| `tools/serve_pg.baga` | 6575 | PostgreSQL frontend/backend v3 |
| `tools/shell.baga` | — | In-process REPL |

Both servers: poll accept → bounded `BOILA_WORKERS` pool (default 4,
cap 64) **or** `BOILA_WORKERS=0` → `go_bg` per connection. Over
`BOILA_MAX_CONN` the new fd is closed (HTTP 503 / PG `53300`).

Data SQL takes a **shared** lock. Schema DDL
(`CREATE`/`DROP`/`ALTER`/`TRUNCATE`/`GRANT`/`REVOKE`) takes an
**exclusive** lock per database. Each shard has its own mutex; the
worker locks only the shard of the key (hop-less — no channel hop per
GET).

## Durability

- Writes go through a per-session txn buffer and commit as one fsync
  batch with a monotonic LSN (`m|next_lsn` in sys).
- Data/index values are `[lsn 8B BE][row]`.
- rocksbaga WAL wraps the window as `WAL_OP_BATCH` (op 19) with one
  CRC (up to 16 MiB per statement group). A torn tail replays as
  “drop the whole batch” — row and secondary index stay together.
- Kill before `COMMIT` → empty storage (nothing durable).
- Secondary indexes live on the **same shard** as the row (same WAL
  batch). No index drift after crash.

Chaos coverage: `tests/boila_chaos_test.baga` + rocksbaga
`lsm_recover_test` `chaos_*` / `grp_*`.

Measured insert durability (2026-08-14, cost-based levels): **1M
rows, 169 s, 1.35 GB RSS, 0 lost, index valid without rebuild**,
group commit ~3120% vs sync-per-write.
`bench/boila/results/insert-write-2026-08-14-levels.md`.

## Memory

Long-lived servers use the C runtime persist arena
(`mem_persist_begin` / `mem_mark` / `mem_rewind`). Per-statement
ephemeral allocations rewind after commit. Shared state (page cache,
plan cache, open DBs, GUC maps) is persist-allocated.

Plan cache is per database, keyed by SQL text, invalidated on DDL.
Trivial PK point queries have a fast path.

## Query budget

Cooperative: every 64 keys the executor checks

- wall clock (`BOILA_BUDGET_MS`, default 5 s) → `57014`
- `max_scan` / `max_rows` (100000) → `54000`

This is how p99 stays bounded. It is not a preemptive abort inside
rocksbaga.

## Health and metrics

`GET /health`, `GET /ready`, `GET /metrics` — [http.md](http.md).
Wire `server_version` is `boilaDB 0.1.0`.

## Benchmarks

From the repo root:

```bash
# sequential ladder (point / insert / mix)
./baga -I . -I app-product bench/boila/harness.baga

# HTTP fan-out vs worker counts
BOILA_WORKERS=4 ./baga -I . -I app-product bench/boila/mt_ladder.baga

# FTS / vector / time-series / graph
bash bench/boila/run_modality_benches.sh all
```

Published numbers (`bench/boila/results/`):

| Workload | Result | Date |
|----------|--------|------|
| Point SELECT @10k | 156k ops/s (6.4 µs) | 2026-08-09 harness |
| Insert @10k | 524 ops/s | 2026-08-09 |
| Mix 80/20 @10k | 2.6k ops/s | 2026-08-09 |
| HTTP mt_ladder 4w c=32 | 7070 ops/s (+48% vs go_bg) | 2026-08-14 |
| FTS AND @20k docs | 73 µs | 2026-08-08 |
| kNN 5k×128d | ~12 ms | gaps V2 |
| time_bucket rollup @100k | ~2.9 ms | 2026-08-10 |
| Graph BFS d=3 @100k edges | 42 µs | 2026-08-10 |
| 1M INSERT RSS | 1.35 GB, DURABLE OK | 2026-08-14 |

SQL point SELECT is still ~5× a raw rocksbaga GET (lex+parse+catalog).
Plan cache + prepared statements cut the parse share on repeats
(gaps Q1). External SQLite / barabadb binaries are not in the repo
CI (gaps P11-4).

## Engineering gates

```bash
bash app-product/boilaDB/scripts/filesize.sh   # ≤ 400 lines per .baga
bash app-product/boilaDB/scripts/deps.sh       # one-way layer imports
./scripts/baga-test                            # tests/boila_*_test.baga
```

Layer ranks (`scripts/deps.sh`):

```
core < storage < txn < catalog|index < fts|vector|ts|graph
    < sql < server < api < tools
```

No upward imports. Prefixes (`boila_*`, `sql_*`, `vec_*`, `fts_*`,
`txn_*`) stay stable when files move.

## v1 limits (honest)

- One node. Shards are in-process, not networked. No raft / replica.
- No cross-database queries or transactions.
- No Geo/GPS — not on the roadmap.
- No `NUMERIC`, window functions, subqueries, `COPY`, TLS, SCRAM.
- Transactions are a session buffer, not multi-version keys.
- UTF-8 only; no ICU collation.
- `FLOAT8` is reserved in the codec, not a creatable column type.

Full residual list: [gaps.md](../gaps.md). Phase history:
[PLAN.md](../PLAN.md).
